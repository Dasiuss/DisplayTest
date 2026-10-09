#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h not found; copy firmware/secrets.example.h into the sketch folder"
#define WIFI_NETWORK_COUNT 0
#define OTA_PASSWORD "hud-ota"
#define ESPNOW_FALLBACK_CHANNEL 1
static const char *WIFI_SSIDS[1] = {""};
static const char *WIFI_PASSES[1] = {""};
#endif

static const char *FIRMWARE_VERSION = "v0.7.0";
static const uint8_t I2C_SDA_PIN = 1;
static const uint8_t I2C_SCL_PIN = 2;
static const uint8_t OLED_ADDRESS = 0x3C;
// Set to 1 for the 72x40 test panel, 0 for the original 128x64 SSD1306.
#define DISPLAY_72X40 1

#if DISPLAY_72X40
static const uint8_t SCREEN_WIDTH = 72;
static const uint8_t SCREEN_HEIGHT = 40;
// The 0.42" 72x40 SSD1306 glass maps SEG0 to GDDRAM column 28.
static const uint8_t OLED_COLUMN_OFFSET = 28;
static const bool MAP_ENABLED = false;
#else
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const uint8_t OLED_COLUMN_OFFSET = 0;
static const bool MAP_ENABLED = true;
#endif

static const uint8_t MAP_WIDTH = 40;
static const uint8_t MAP_HEIGHT = 64;
static const uint8_t MAP_ROW_BYTES = (MAP_WIDTH + 7) / 8;
static const uint16_t MAP_BYTES = MAP_ROW_BYTES * MAP_HEIGHT;
static const uint8_t MAP_CHUNK_BYTES = 16;
static const uint16_t ROUTE_STEP_MS = 28;

static const char *SERVICE_UUID = "5f8a0001-4e56-4e46-9a7c-000000000001";
static const char *CHARACTERISTIC_UUID = "5f8a0001-4e56-4e46-9a7c-000000000002";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BLECharacteristic *displayCharacteristic = nullptr;

struct HudState {
  uint16_t speed = 23;
  int16_t navigationAngle = 42;
  uint16_t average = 18;
  uint16_t remaining = 13;
  uint16_t total = 43;
  bool frameEnabled = true;
};

HudState hudState;
uint8_t mapBitmap[MAP_BYTES] = {};
uint8_t routeBitmap[MAP_BYTES] = {};

struct BitmapTransfer {
  uint8_t buffer[MAP_BYTES] = {};
  uint32_t receivedMask = 0;
  uint8_t sequence = 0;
  uint8_t chunks = 0;
  bool active = false;
};

BitmapTransfer mapTransfer;
BitmapTransfer routeTransfer;
bool screenDirty = true;
bool displayReady = false;
bool clientConnected = false;
unsigned long lastRender = 0;
unsigned long lastStateLog = 0;
unsigned long lastRouteStep = 0;
uint16_t routeCursor = 0;

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 12000;
static const unsigned long WIFI_SEARCH_INTERVAL_MS = 30000;
static const unsigned long ESPNOW_SEND_INTERVAL_MS = 200;

bool espNowReady = false;
bool wifiConnected = false;
bool wifiSearching = false;
int wifiNetIndex = 0;
unsigned long wifiAttemptStart = 0;
unsigned long lastEspNowSend = 0;
uint8_t espNowSequence = 0;
volatile bool espNowPending = false;

static const uint8_t MAP_MODE_STATS = 0;
static const uint8_t MAP_MODE_MAP = 1;
static const unsigned long LOCATION_HEARTBEAT_MS = 2000;

volatile uint8_t displayMode = MAP_MODE_STATS;
volatile uint8_t locationZoom = 1;
volatile uint16_t geoFisheye = 220;
volatile int32_t geoLatE7 = 0;
volatile int32_t geoLonE7 = 0;
volatile int16_t geoHeading = -1;
volatile bool geoValid = false;
volatile bool locationPending = false;

unsigned long lastLocationSend = 0;
uint8_t locationSequence = 0;

uint16_t readUint16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int16_t readInt16(const uint8_t *data) {
  return static_cast<int16_t>(readUint16(data));
}

void drawNavigationArrow(int16_t centerX, int16_t centerY, int16_t angle, float tipLength, float baseLength) {
  const float radians = angle * PI / 180.0f;
  const int16_t tipX = centerX + static_cast<int16_t>(sin(radians) * tipLength);
  const int16_t tipY = centerY - static_cast<int16_t>(cos(radians) * tipLength);
  const int16_t leftX = centerX + static_cast<int16_t>(sin(radians + 2.45f) * baseLength);
  const int16_t leftY = centerY - static_cast<int16_t>(cos(radians + 2.45f) * baseLength);
  const int16_t rightX = centerX + static_cast<int16_t>(sin(radians - 2.45f) * baseLength);
  const int16_t rightY = centerY - static_cast<int16_t>(cos(radians - 2.45f) * baseLength);

  display.drawLine(centerX, centerY, tipX, tipY, SSD1306_WHITE);
  display.drawLine(tipX, tipY, leftX, leftY, SSD1306_WHITE);
  display.drawLine(tipX, tipY, rightX, rightY, SSD1306_WHITE);
}

uint16_t countRoutePixels() {
  uint16_t count = 0;
  for (uint16_t index = 0; index < MAP_BYTES; index++) {
    uint8_t bits = routeBitmap[index];
    while (bits != 0) {
      count++;
      bits &= bits - 1;
    }
  }
  return count;
}

uint8_t getRouteGapCount(uint16_t routePixels) {
  if (routePixels <= 15) return 1;
  if (routePixels <= 30) return 2;
  if (routePixels <= 45) return 3;
  return 4;
}

void drawAnimatedRoute() {
  const uint16_t routePixels = countRoutePixels();
  if (routePixels == 0) return;
  if (routeCursor >= routePixels) routeCursor = 0;

  const uint8_t gapCount = getRouteGapCount(routePixels);
  const uint16_t gapLength = 5;
  uint16_t current = 0;
  for (int16_t y = MAP_HEIGHT - 1; y >= 0; y--) {
    for (uint8_t x = 0; x < MAP_WIDTH; x++) {
      const uint16_t index = y * MAP_WIDTH + x;
      if (routeBitmap[(y * MAP_ROW_BYTES) + (x / 8)] & (0x80 >> (x % 8))) {
        bool isGap = false;
        for (uint8_t gapIndex = 0; gapIndex < gapCount; gapIndex++) {
          const uint16_t gapStart = (routeCursor + (static_cast<uint32_t>(gapIndex) * routePixels) / gapCount) % routePixels;
          const uint16_t distanceFromGap = (current + routePixels - gapStart) % routePixels;
          if (distanceFromGap < gapLength) {
            isGap = true;
            break;
          }
        }
        if (isGap) {
          display.drawPixel(88 + x, y, SSD1306_BLACK);
        }
        current++;
      }
    }
  }
}

void pushDisplay() {
  display.ssd1306_command(SSD1306_COLUMNADDR);
  display.ssd1306_command(OLED_COLUMN_OFFSET);
  display.ssd1306_command(OLED_COLUMN_OFFSET + SCREEN_WIDTH - 1);
  display.ssd1306_command(SSD1306_PAGEADDR);
  display.ssd1306_command(0);
  display.ssd1306_command((SCREEN_HEIGHT / 8) - 1);

  const uint16_t total = SCREEN_WIDTH * ((SCREEN_HEIGHT + 7) / 8);
  const uint8_t *ptr = display.getBuffer();
  const uint16_t chunkSize = 128;
  uint16_t remaining = total;
  while (remaining > 0) {
    const uint16_t batch = min(remaining, chunkSize);
    Wire.beginTransmission(OLED_ADDRESS);
    Wire.write(static_cast<uint8_t>(0x40));
    Wire.write(ptr, batch);
    Wire.endTransmission();
    ptr += batch;
    remaining -= batch;
  }
}

void renderHud() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

#if DISPLAY_72X40
  display.setFont(&TomThumb);
  display.setCursor(3, 11);
  display.print("SPD");
  display.setCursor(3, 27);
  display.print("REM");

  char averageLine[12];
  char totalLine[12];
  snprintf(averageLine, sizeof(averageLine), "MAX %3u", hudState.average);
  snprintf(totalLine, sizeof(totalLine), "TOT %3u", hudState.total);
  display.setCursor(3, 38);
  display.print(averageLine);
  int16_t boundX = 0;
  int16_t boundY = 0;
  uint16_t boundW = 0;
  uint16_t boundH = 0;
  display.getTextBounds(totalLine, 0, 0, &boundX, &boundY, &boundW, &boundH);
  display.setCursor(69 - static_cast<int16_t>(boundW), 38);
  display.print(totalLine);

  display.setFont();
  display.setTextSize(2);
  display.setCursor(20, 1);
  display.printf("%3u", hudState.speed);
  display.setCursor(20, 17);
  display.printf("%3u", hudState.remaining);
  display.setTextSize(1);

  drawNavigationArrow(63, 9, hudState.navigationAngle, 6.0f, 4.0f);
  if (hudState.frameEnabled) {
    display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  }
#else
  display.setCursor(1, 0);
  display.print("SPD");
  display.setTextSize(3);
  display.setCursor(1, 9);
  display.printf("%3u", hudState.speed);

  display.setTextSize(1);
  display.setCursor(1, 37);
  display.print("REM");
  display.setTextSize(2);
  display.setCursor(1, 48);
  display.printf("%3u", hudState.remaining);
  display.setTextSize(1);

  drawNavigationArrow(69, 10, hudState.navigationAngle, 9.0f, 6.0f);

  display.setCursor(62, 27);
  display.print("MAX");
  display.setCursor(50, 48);
  display.print("TOTAL");
  display.setCursor(62, 35);
  display.printf("%3u", hudState.average);
  display.setCursor(62, 56);
  display.printf("%3u", hudState.total);

  display.drawLine(87, 0, 87, 63, SSD1306_WHITE);
  display.drawBitmap(88, 0, mapBitmap, MAP_WIDTH, MAP_HEIGHT, SSD1306_WHITE);
  display.drawBitmap(88, 0, routeBitmap, MAP_WIDTH, MAP_HEIGHT, SSD1306_WHITE);
  drawAnimatedRoute();
#endif

  pushDisplay();
  screenDirty = false;
  lastRender = millis();
}

void applyStatePacket(const uint8_t *data, size_t length) {
  if (length < 12 || data[0] != 'S') return;

  hudState.speed = readUint16(data + 2);
  hudState.navigationAngle = readInt16(data + 4);
  hudState.average = readUint16(data + 6);
  hudState.remaining = readUint16(data + 8);
  hudState.total = readUint16(data + 10);
  if (length >= 13) hudState.frameEnabled = (data[12] & 0x01) != 0;
  if (hudState.navigationAngle < -359) hudState.navigationAngle = -359;
  if (hudState.navigationAngle > 359) hudState.navigationAngle = 359;
  screenDirty = true;

  if (millis() - lastStateLog > 1000) {
    Serial.printf("[%s] HUD state: speed=%u nav=%d remaining=%u\n",
                  FIRMWARE_VERSION,
                  hudState.speed,
                  hudState.navigationAngle,
                  hudState.remaining);
    lastStateLog = millis();
  }
  espNowPending = true;
}

void applyBitmapPacket(const uint8_t *data, size_t length) {
  if (length < 4 || (data[0] != 'M' && data[0] != 'R')) return;

  BitmapTransfer *transfer = data[0] == 'M' ? &mapTransfer : &routeTransfer;
  uint8_t *target = data[0] == 'M' ? mapBitmap : routeBitmap;

  const uint8_t sequence = data[1];
  const uint8_t chunkIndex = data[2];
  const uint8_t chunkCount = data[3];
  if (chunkCount == 0 || chunkCount > 20 || chunkIndex >= chunkCount) return;

  if (!transfer->active || sequence != transfer->sequence || chunkCount != transfer->chunks) {
    memset(transfer->buffer, 0, sizeof(transfer->buffer));
    transfer->receivedMask = 0;
    transfer->sequence = sequence;
    transfer->chunks = chunkCount;
    transfer->active = true;
  }

  const uint16_t offset = chunkIndex * MAP_CHUNK_BYTES;
  const uint8_t available = min(static_cast<size_t>(MAP_CHUNK_BYTES), length - 4);
  if (offset < MAP_BYTES) {
    memcpy(transfer->buffer + offset, data + 4, min(static_cast<uint16_t>(available), static_cast<uint16_t>(MAP_BYTES - offset)));
  }
  transfer->receivedMask |= static_cast<uint32_t>(1UL << chunkIndex);

  const uint32_t expectedMask = (1UL << chunkCount) - 1UL;
  if (transfer->receivedMask == expectedMask) {
    memcpy(target, transfer->buffer, MAP_BYTES);
    transfer->active = false;
    if (data[0] == 'R') routeCursor = 0;
    screenDirty = true;
    Serial.printf("[%s] %s updated: %u chunks\n", FIRMWARE_VERSION, data[0] == 'M' ? "Map" : "Route", chunkCount);
  }
}

void startEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.printf("[%s] ESP-NOW init failed\n", FIRMWARE_VERSION);
    return;
  }
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, ESPNOW_BROADCAST, sizeof(ESPNOW_BROADCAST));
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.printf("[%s] ESP-NOW add peer failed\n", FIRMWARE_VERSION);
    return;
  }
  espNowReady = true;
  Serial.printf("[%s] ESP-NOW ready (broadcast)\n", FIRMWARE_VERSION);
}

void useFallbackChannel() {
  esp_wifi_set_channel(ESPNOW_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[%s] ESP-NOW fallback channel %u\n", FIRMWARE_VERSION, ESPNOW_FALLBACK_CHANNEL);
}

void wifiStartNext() {
#if WIFI_NETWORK_COUNT > 0
  WiFi.disconnect();
  WiFi.begin(WIFI_SSIDS[wifiNetIndex], WIFI_PASSES[wifiNetIndex]);
  Serial.printf("[%s] WiFi: trying %s\n", FIRMWARE_VERSION, WIFI_SSIDS[wifiNetIndex]);
  wifiNetIndex = (wifiNetIndex + 1) % WIFI_NETWORK_COUNT;
  wifiAttemptStart = millis();
  wifiSearching = true;
#else
  useFallbackChannel();
  wifiAttemptStart = millis();
  wifiSearching = false;
#endif
}

void serviceWifi() {
  if (wifiConnected) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      wifiSearching = false;
      wifiAttemptStart = millis();
      useFallbackChannel();
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiSearching = false;
    Serial.printf("[%s] WiFi connected: %s channel=%d rssi=%d ip=%s\n",
                  FIRMWARE_VERSION,
                  WiFi.SSID().c_str(),
                  WiFi.channel(),
                  WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
    return;
  }
  const unsigned long elapsed = millis() - wifiAttemptStart;
  if (wifiSearching) {
    if (elapsed > WIFI_ATTEMPT_TIMEOUT_MS) {
      wifiSearching = false;
      wifiAttemptStart = millis();
      useFallbackChannel();
    }
  } else if (elapsed > WIFI_SEARCH_INTERVAL_MS) {
    wifiStartNext();
  }
}

void sendEspNowState() {
  if (!espNowReady) return;

  uint8_t packet[13];
  packet[0] = 'S';
  packet[1] = espNowSequence++;
  packet[2] = hudState.speed & 0xFF;
  packet[3] = (hudState.speed >> 8) & 0xFF;
  const uint16_t angle = static_cast<uint16_t>(hudState.navigationAngle);
  packet[4] = angle & 0xFF;
  packet[5] = (angle >> 8) & 0xFF;
  packet[6] = hudState.average & 0xFF;
  packet[7] = (hudState.average >> 8) & 0xFF;
  packet[8] = hudState.remaining & 0xFF;
  packet[9] = (hudState.remaining >> 8) & 0xFF;
  packet[10] = hudState.total & 0xFF;
  packet[11] = (hudState.total >> 8) & 0xFF;
  packet[12] = hudState.frameEnabled ? 0x01 : 0x00;
  esp_now_send(ESPNOW_BROADCAST, packet, sizeof(packet));
}

int32_t readInt32(const uint8_t *data) {
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) |
                              (static_cast<uint32_t>(data[3]) << 24));
}

void sendLocationPacket() {
  if (!espNowReady || !geoValid) return;

  const int32_t lat = geoLatE7;
  const int32_t lon = geoLonE7;
  const int16_t heading = geoHeading;
  const uint16_t fisheye = geoFisheye;

  uint8_t packet[16];
  packet[0] = 'L';
  packet[1] = locationSequence++;
  packet[2] = displayMode;
  packet[3] = locationZoom;
  packet[4] = static_cast<uint8_t>(lat & 0xFF);
  packet[5] = static_cast<uint8_t>((lat >> 8) & 0xFF);
  packet[6] = static_cast<uint8_t>((lat >> 16) & 0xFF);
  packet[7] = static_cast<uint8_t>((lat >> 24) & 0xFF);
  packet[8] = static_cast<uint8_t>(lon & 0xFF);
  packet[9] = static_cast<uint8_t>((lon >> 8) & 0xFF);
  packet[10] = static_cast<uint8_t>((lon >> 16) & 0xFF);
  packet[11] = static_cast<uint8_t>((lon >> 24) & 0xFF);
  packet[12] = static_cast<uint8_t>(heading & 0xFF);
  packet[13] = static_cast<uint8_t>((heading >> 8) & 0xFF);
  packet[14] = static_cast<uint8_t>(fisheye & 0xFF);
  packet[15] = static_cast<uint8_t>((fisheye >> 8) & 0xFF);
  esp_now_send(ESPNOW_BROADCAST, packet, sizeof(packet));
  lastLocationSend = millis();
}

void applyGeoPacket(const uint8_t *data, size_t length) {
  if (length < 16) return;

  geoLatE7 = readInt32(data + 2);
  geoLonE7 = readInt32(data + 6);
  displayMode = data[10];
  geoHeading = static_cast<int16_t>(data[11] | (data[12] << 8));
  locationZoom = data[13];
  geoFisheye = static_cast<uint16_t>(data[14] | (data[15] << 8));
  geoValid = true;
  locationPending = true;

  Serial.printf("[%s] Geo %.5f, %.5f mode=%u zoom=%u fisheye=%u heading=%d\n",
                FIRMWARE_VERSION,
                geoLatE7 / 1e7,
                geoLonE7 / 1e7,
                static_cast<unsigned>(displayMode),
                static_cast<unsigned>(locationZoom),
                static_cast<unsigned>(geoFisheye),
                geoHeading);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    clientConnected = true;
    Serial.printf("[%s] BLE client connected\n", FIRMWARE_VERSION);
  }

  void onDisconnect(BLEServer *) override {
    clientConnected = false;
    Serial.printf("[%s] BLE client disconnected\n", FIRMWARE_VERSION);
    BLEDevice::startAdvertising();
  }
};

class DisplayCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    uint8_t *data = characteristic->getData();
    const size_t length = characteristic->getLength();
    if (data == nullptr || length == 0) return;

    if (data[0] == 'S') {
      applyStatePacket(data, length);
    } else if (data[0] == 'M' || data[0] == 'R') {
      applyBitmapPacket(data, length);
    } else if (data[0] == 'G') {
      applyGeoPacket(data, length);
    }
  }
};

void startBluetooth() {
  BLEDevice::init("HUD ESP32-S3");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);
  displayCharacteristic = service->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  displayCharacteristic->setCallbacks(new DisplayCallbacks());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[%s] BLE advertising as HUD ESP32-S3\n", FIRMWARE_VERSION);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\nDisplayTest HUD firmware %s\n", FIRMWARE_VERSION);
  Serial.printf("I2C: SDA=GPIO%u SCL=GPIO%u address=0x%02X\n", I2C_SDA_PIN, I2C_SCL_PIN, OLED_ADDRESS);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false, false)) {
    Serial.printf("[%s] ERROR: SSD1306 not found at 0x%02X\n", FIRMWARE_VERSION, OLED_ADDRESS);
  } else {
    displayReady = true;
#if DISPLAY_72X40
    // 0.42" 72x40 panel overrides on top of the Adafruit_SSD1306 init.
    display.ssd1306_command(0xAD); // internal IREF setting
    display.ssd1306_command(0x30);
    display.ssd1306_command(SSD1306_SETCOMPINS);
    display.ssd1306_command(0x12);
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xAF);
    display.ssd1306_command(SSD1306_SETPRECHARGE);
    display.ssd1306_command(0x22);
    display.ssd1306_command(SSD1306_SETVCOMDETECT);
    display.ssd1306_command(0x20);
    display.ssd1306_command(SSD1306_DISPLAYON);
    Serial.printf("[%s] 72x40 panel initialized (column offset %u)\n", FIRMWARE_VERSION, OLED_COLUMN_OFFSET);
#else
    Serial.printf("[%s] SSD1306 initialized\n", FIRMWARE_VERSION);
#endif
    renderHud();
  }

  startBluetooth();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  startEspNow();
  wifiStartNext();
}

void loop() {
  if (displayReady && MAP_ENABLED && millis() - lastRouteStep >= ROUTE_STEP_MS) {
    routeCursor++;
    lastRouteStep = millis();
    screenDirty = true;
  }
  if (displayReady && screenDirty && millis() - lastRender >= 10) renderHud();

  serviceWifi();
  if (espNowReady && (espNowPending || millis() - lastEspNowSend >= ESPNOW_SEND_INTERVAL_MS)) {
    sendEspNowState();
    espNowPending = false;
    lastEspNowSend = millis();
  }
  if (geoValid && espNowReady && (locationPending || millis() - lastLocationSend >= LOCATION_HEARTBEAT_MS)) {
    sendLocationPacket();
    locationPending = false;
    lastLocationSend = millis();
  }

  delay(1);
}
