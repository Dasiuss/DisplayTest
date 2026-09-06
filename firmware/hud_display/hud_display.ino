#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

static const char *FIRMWARE_VERSION = "v0.2.0";
static const uint8_t I2C_SDA_PIN = 1;
static const uint8_t I2C_SCL_PIN = 2;
static const uint8_t OLED_ADDRESS = 0x3C;
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const uint8_t MAP_WIDTH = 40;
static const uint8_t MAP_HEIGHT = 64;
static const uint8_t MAP_ROW_BYTES = (MAP_WIDTH + 7) / 8;
static const uint16_t MAP_BYTES = MAP_ROW_BYTES * MAP_HEIGHT;
static const uint8_t MAP_CHUNK_BYTES = 16;
static const uint16_t ROUTE_STEP_MS = 85;

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

uint16_t readUint16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int16_t readInt16(const uint8_t *data) {
  return static_cast<int16_t>(readUint16(data));
}

void drawNavigationArrow(int16_t centerX, int16_t centerY, int16_t angle) {
  const float radians = angle * PI / 180.0f;
  const int16_t tipX = centerX + static_cast<int16_t>(sin(radians) * 9.0f);
  const int16_t tipY = centerY - static_cast<int16_t>(cos(radians) * 9.0f);
  const int16_t leftX = centerX + static_cast<int16_t>(sin(radians + 2.45f) * 6.0f);
  const int16_t leftY = centerY - static_cast<int16_t>(cos(radians + 2.45f) * 6.0f);
  const int16_t rightX = centerX + static_cast<int16_t>(sin(radians - 2.45f) * 6.0f);
  const int16_t rightY = centerY - static_cast<int16_t>(cos(radians - 2.45f) * 6.0f);

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

void drawAnimatedRoute() {
  const uint16_t routePixels = countRoutePixels();
  if (routePixels == 0) return;
  if (routeCursor >= routePixels) routeCursor = 0;

  uint16_t current = 0;
  for (int16_t y = MAP_HEIGHT - 1; y >= 0; y--) {
    for (uint8_t x = 0; x < MAP_WIDTH; x++) {
      const uint16_t index = y * MAP_WIDTH + x;
      if (routeBitmap[(y * MAP_ROW_BYTES) + (x / 8)] & (0x80 >> (x % 8))) {
        if (current == routeCursor) {
          display.drawPixel(88 + x, y, SSD1306_WHITE);
          return;
        }
        current++;
      }
    }
  }
}

void renderHud() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(1, 0);
  display.print("SPD");
  display.setTextSize(2);
  display.setCursor(1, 7);
  display.printf("%3u", hudState.speed);

  display.setTextSize(1);
  display.setCursor(1, 22);
  display.print("REM");
  display.setCursor(1, 29);
  display.printf("%3u", hudState.remaining);

  drawNavigationArrow(69, 32, hudState.navigationAngle);

  display.drawLine(0, 45, 86, 45, SSD1306_WHITE);
  display.setCursor(1, 52);
  display.printf("AVG %3u", hudState.average);
  display.setCursor(44, 52);
  display.printf("TOT %3u", hudState.total);

  display.drawLine(87, 0, 87, 63, SSD1306_WHITE);
  display.drawBitmap(88, 0, mapBitmap, MAP_WIDTH, MAP_HEIGHT, SSD1306_WHITE);
  drawAnimatedRoute();
  display.display();
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
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.printf("[%s] ERROR: SSD1306 not found at 0x%02X\n", FIRMWARE_VERSION, OLED_ADDRESS);
  } else {
    displayReady = true;
    Serial.printf("[%s] SSD1306 initialized\n", FIRMWARE_VERSION);
    renderHud();
  }

  startBluetooth();
}

void loop() {
  if (displayReady && millis() - lastRouteStep >= ROUTE_STEP_MS) {
    routeCursor++;
    lastRouteStep = millis();
    screenDirty = true;
  }
  if (displayReady && screenDirty && millis() - lastRender >= 10) renderHud();
  delay(1);
}
