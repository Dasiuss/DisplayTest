// DisplayTest HUD receiver for the GeekMagic SmallTV / SmallTV-Ultra.
//
// Hardware: ESP-12F (ESP8266, 4MB), 1.54" 240x240 ST7789 over hardware SPI.
// Wiring:   MOSI GPIO13, SCK GPIO14, CS tied to GND, DC GPIO0, RST GPIO2,
//           backlight GPIO5 (active low).
//
// It receives the same 13-byte state packet the PWA sends over BLE and renders
// it on the colour display. OTA is available over WiFi (ArduinoOTA + /update).

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>

#include "map_data.h"

extern "C" {
#include <espnow.h>
}

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

static const char *FIRMWARE_VERSION = "v0.1.0";
static const char *DEVICE_NAME = "hud-receiver";
static const char *AP_SSID = "HUD-Receiver";
static const char *AP_PASSWORD = "hudreceiver";

// ST7789 wiring (GeekMagic SmallTV / SmallTV-Ultra).
static const int8_t TFT_CS_PIN = -1;
static const int8_t TFT_DC_PIN = 0;
static const int8_t TFT_RST_PIN = 2;
static const int8_t TFT_BL_PIN = 5;
static const uint32_t TFT_SPI_HZ = 40000000;

// RGB565 palette.
static const uint16_t COLOR_BG = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_GRAY = 0x39E7;
static const uint16_t COLOR_DARK = 0x2124;
static const uint16_t COLOR_CYAN = 0x07FF;
static const uint16_t COLOR_GREEN = 0x07E0;
static const uint16_t COLOR_ORANGE = 0xFD20;
static const uint16_t COLOR_MAGENTA = 0xF81F;
static const uint16_t COLOR_VIOLET = 0x901A;
static const uint16_t COLOR_YELLOW = 0xFFE0;
static const uint16_t COLOR_RED = 0xF800;

static const unsigned long NET_ATTEMPT_TIMEOUT_MS = 12000;
static const unsigned long NET_SEARCH_INTERVAL_MS = 30000;
static const unsigned long LINK_TIMEOUT_MS = 3000;
static const unsigned long RENDER_MIN_INTERVAL_MS = 50;

// Map palette (RGB565). Advanced/black is drawn white so it stays visible on
// the black background.
static const uint16_t COLOR_NOVICE = 0x07E0;
static const uint16_t COLOR_EASY = 0x04DF;
static const uint16_t COLOR_INTERMEDIATE = 0xF800;
static const uint16_t COLOR_ADVANCED = 0xFFFF;
static const uint16_t COLOR_FREERIDE = 0xFFE0;
static const uint16_t COLOR_UNKNOWN = 0x7BEF;
static const uint16_t COLOR_SNOWPARK = 0xF81F;
static const uint16_t COLOR_CONNECTION = 0x7BEF;
static const uint16_t COLOR_LIFT = 0xFD20;
static const uint16_t COLOR_RIDER = 0x001F;

static const int16_t MAP_RIDER_X = 120;
static const int16_t MAP_RIDER_Y = 190;
static const uint16_t MAP_VIEW_METERS[5] = {250, 500, 1000, 2000, 4000};
static const uint8_t MAP_VIEW_COUNT = 5;
static const uint8_t MAP_MAX_LABELS = 16;
static const uint16_t MAP_MAX_CANDIDATES = 220;
static const float MAP_METERS_PER_DEG_LAT = 110540.0f;
// Fisheye default: screen radius = R * r / (r + K). Bigger R = more
// magnification in the centre and stronger compression at the edge. R can be
// changed live from the PWA.
static const uint16_t MAP_FISHEYE_RADIUS_DEFAULT = 220;

Adafruit_ST7789 tft(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

struct Telemetry {
  uint16_t speed = 0;
  int16_t navAngle = 0;
  uint16_t average = 0;
  uint16_t remaining = 0;
  uint16_t total = 0;
  bool frameEnabled = true;
  bool valid = false;
};

Telemetry telemetry;
volatile bool telemetryDirty = true;
volatile unsigned long lastPacketMs = 0;
volatile bool linkSeen = false;

bool espNowReady = false;
bool wifiConnected = false;
bool wifiSearching = false;
bool mdnsStarted = false;
int wifiNetIndex = 0;
unsigned long wifiAttemptStart = 0;
unsigned long lastRender = 0;

// Incremental rendering: track what is already on screen so we only touch
// pixels that actually changed.
bool layoutDrawn = false;
int16_t lastSpeed = -1;
int16_t lastRemaining = -1;
int16_t lastAverage = -1;
int16_t lastTotal = -1;
int16_t lastAngle = -1000;
bool lastFresh = false;

uint8_t displayMode = 0;
uint8_t mapZoomIndex = 1;
uint16_t mapFisheyeRadius = MAP_FISHEYE_RADIUS_DEFAULT;
int32_t riderLatE7 = MAP_ORIGIN_LAT_E7;
int32_t riderLonE7 = MAP_ORIGIN_LON_E7;
int16_t mapHeading = -1;
int16_t mapBearing = 0;
bool mapHasFix = false;
volatile bool mapDirty = false;
float mapMetersPerDegLon = 0.0f;

struct MapLabel {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
  uint16_t nameOffset;
  uint8_t nameLength;
  uint8_t textSize;
  uint16_t color;
  int32_t distanceSq;
};

MapLabel mapLabelCandidates[MAP_MAX_CANDIDATES];
MapLabel mapPlacedLabels[MAP_MAX_LABELS];

int32_t readInt32(const uint8_t *data) {
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) |
                              (static_cast<uint32_t>(data[3]) << 24));
}

uint16_t mapWayColor(const MapWay &way) {
  if (way.kind == 1) return COLOR_SNOWPARK;
  if (way.kind == 2) return COLOR_CONNECTION;
  switch (way.difficulty) {
    case 0: return COLOR_NOVICE;
    case 1: return COLOR_EASY;
    case 2: return COLOR_INTERMEDIATE;
    case 3: return COLOR_ADVANCED;
    case 4: return COLOR_FREERIDE;
    default: return COLOR_UNKNOWN;
  }
}

int16_t mapClamp(int32_t value) {
  if (value < -400) return -400;
  if (value > 640) return 640;
  return static_cast<int16_t>(value);
}

void projectMapPoint(int16_t east, int16_t north, float riderEast, float riderNorth, float cosBearing, float sinBearing, float fisheyeK, int16_t *outX, int16_t *outY) {
  const float dx = east - riderEast;
  const float dy = north - riderNorth;
  const float rotatedX = dx * cosBearing - dy * sinBearing;
  const float rotatedY = -dx * sinBearing - dy * cosBearing;
  const float worldRadius = sqrtf(rotatedX * rotatedX + rotatedY * rotatedY);
  const float factor = static_cast<float>(mapFisheyeRadius) / (worldRadius + fisheyeK);
  *outX = mapClamp(MAP_RIDER_X + static_cast<int32_t>(rotatedX * factor));
  *outY = mapClamp(MAP_RIDER_Y + static_cast<int32_t>(rotatedY * factor));
}

int16_t findNearestDownhillBearing(float riderEast, float riderNorth, bool *found) {
  int32_t best = 0x7FFFFFFF;
  int16_t bearing = 0;
  *found = false;
  for (uint16_t i = 0; i < MAP_WAY_COUNT; i++) {
    MapWay way;
    memcpy_P(&way, &mapWays[i], sizeof(MapWay));
    if (way.kind != 0 || way.pointCount == 0) continue;
    for (uint16_t j = 0; j < way.pointCount; j++) {
      MapPoint point;
      memcpy_P(&point, &mapPoints[way.firstPoint + j], sizeof(MapPoint));
      const int32_t dx = point.east - static_cast<int32_t>(riderEast);
      const int32_t dy = point.north - static_cast<int32_t>(riderNorth);
      const int32_t distance = dx * dx + dy * dy;
      if (distance < best) {
        best = distance;
        bearing = way.bearing;
        *found = true;
      }
    }
  }
  return bearing;
}

void drawMapPolyline(uint16_t firstPoint, uint16_t pointCount, uint16_t color, float riderEast, float riderNorth, float cosBearing, float sinBearing, float fisheyeK) {
  if (pointCount < 2) return;
  MapPoint previous;
  memcpy_P(&previous, &mapPoints[firstPoint], sizeof(MapPoint));
  int16_t previousX;
  int16_t previousY;
  projectMapPoint(previous.east, previous.north, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK, &previousX, &previousY);
  for (uint16_t index = 1; index < pointCount; index++) {
    MapPoint current;
    memcpy_P(&current, &mapPoints[firstPoint + index], sizeof(MapPoint));
    int16_t currentX;
    int16_t currentY;
    projectMapPoint(current.east, current.north, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK, &currentX, &currentY);
    const bool bothLeft = previousX < -20 && currentX < -20;
    const bool bothRight = previousX > 259 && currentX > 259;
    const bool bothTop = previousY < -20 && currentY < -20;
    const bool bothBottom = previousY > 259 && currentY > 259;
    if (!(bothLeft || bothRight || bothTop || bothBottom)) {
      tft.drawLine(previousX, previousY, currentX, currentY, color);
    }
    previousX = currentX;
    previousY = currentY;
  }
}

void copyLabelText(char *buffer, uint8_t bufferSize, uint16_t offset, uint8_t length) {
  uint8_t count = length;
  if (count > bufferSize - 1) count = bufferSize - 1;
  for (uint8_t index = 0; index < count; index++) {
    buffer[index] = static_cast<char>(pgm_read_byte(&mapNames[offset + index]));
  }
  buffer[count] = '\0';
}

bool labelOverlaps(const MapLabel &label) {
  for (uint8_t index = 0; index < MAP_MAX_LABELS; index++) {
    const MapLabel &other = mapPlacedLabels[index];
    if (other.width == 0) continue;
    if (!(label.x + label.width <= other.x || other.x + other.width <= label.x ||
          label.y + label.height <= other.y || other.y + other.height <= label.y)) {
      return true;
    }
  }
  return false;
}

void addLabelCandidate(uint16_t nameOffset, uint8_t nameLength, uint8_t textSize, uint16_t color, uint16_t firstPoint, uint16_t pointCount, float riderEast, float riderNorth, float cosBearing, float sinBearing, float fisheyeK, uint16_t *count) {
  if (nameLength == 0 || *count >= MAP_MAX_CANDIDATES || pointCount == 0) return;
  MapPoint middle;
  memcpy_P(&middle, &mapPoints[firstPoint + pointCount / 2], sizeof(MapPoint));
  int16_t screenX;
  int16_t screenY;
  projectMapPoint(middle.east, middle.north, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK, &screenX, &screenY);
  if (screenX < 0 || screenX > 239 || screenY < 0 || screenY > 239) return;

  const int16_t width = static_cast<int16_t>(6 * textSize) * nameLength;
  const int16_t height = static_cast<int16_t>(8 * textSize);
  int16_t left = screenX - width / 2;
  int16_t top = screenY - height / 2;
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (left + width > 239) left = 239 - width;
  if (top + height > 239) top = 239 - height;

  const float dx = middle.east - riderEast;
  const float dy = middle.north - riderNorth;
  MapLabel &label = mapLabelCandidates[*count];
  label.x = left;
  label.y = top;
  label.width = width;
  label.height = height;
  label.nameOffset = nameOffset;
  label.nameLength = nameLength;
  label.textSize = textSize;
  label.color = color;
  label.distanceSq = static_cast<int32_t>(dx * dx + dy * dy);
  *count += 1;
}

void drawMapLabels(float riderEast, float riderNorth, float cosBearing, float sinBearing, float fisheyeK) {
  uint16_t count = 0;
  for (uint16_t i = 0; i < MAP_WAY_COUNT; i++) {
    MapWay way;
    memcpy_P(&way, &mapWays[i], sizeof(MapWay));
    addLabelCandidate(way.nameOffset, way.nameLength, 1, mapWayColor(way), way.firstPoint, way.pointCount, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK, &count);
  }
  for (uint16_t i = 0; i < MAP_LIFT_COUNT; i++) {
    MapLift lift;
    memcpy_P(&lift, &mapLifts[i], sizeof(MapLift));
    addLabelCandidate(lift.nameOffset, lift.nameLength, 1, COLOR_WHITE, lift.firstPoint, lift.pointCount, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK, &count);
  }

  for (uint16_t i = 1; i < count; i++) {
    MapLabel key = mapLabelCandidates[i];
    int16_t j = static_cast<int16_t>(i) - 1;
    while (j >= 0 && mapLabelCandidates[j].distanceSq > key.distanceSq) {
      mapLabelCandidates[j + 1] = mapLabelCandidates[j];
      j -= 1;
    }
    mapLabelCandidates[j + 1] = key;
  }

  for (uint8_t index = 0; index < MAP_MAX_LABELS; index++) mapPlacedLabels[index].width = 0;

  uint8_t placed = 0;
  for (uint16_t i = 0; i < count && placed < MAP_MAX_LABELS; i++) {
    const MapLabel &candidate = mapLabelCandidates[i];
    if (labelOverlaps(candidate)) continue;
    mapPlacedLabels[placed++] = candidate;

    char buffer[12];
    copyLabelText(buffer, sizeof(buffer), candidate.nameOffset, candidate.nameLength);
    tft.setFont(NULL);
    tft.setTextSize(candidate.textSize);
    tft.setTextColor(candidate.color);
    tft.fillRect(candidate.x - 1, candidate.y - 1, candidate.width + 2, candidate.height + 2, COLOR_BG);
    tft.setCursor(candidate.x, candidate.y);
    tft.print(buffer);
  }
  tft.setTextSize(1);
}

void drawRiderDot() {
  tft.fillCircle(MAP_RIDER_X, MAP_RIDER_Y, 5, COLOR_RIDER);
  tft.drawCircle(MAP_RIDER_X, MAP_RIDER_Y, 6, COLOR_WHITE);
}

void drawMap() {
  if (mapMetersPerDegLon == 0.0f) {
    mapMetersPerDegLon = 111320.0f * cosf((static_cast<float>(MAP_ORIGIN_LAT_E7) / 1e7f) * PI / 180.0f);
  }
  uint8_t zoom = mapZoomIndex;
  if (zoom >= MAP_VIEW_COUNT) zoom = MAP_VIEW_COUNT - 1;
  const float fisheyeK = static_cast<float>(MAP_VIEW_METERS[zoom]);

  const float riderEast = (static_cast<float>(riderLonE7) / 1e7f - static_cast<float>(MAP_ORIGIN_LON_E7) / 1e7f) * mapMetersPerDegLon;
  const float riderNorth = (static_cast<float>(riderLatE7) / 1e7f - static_cast<float>(MAP_ORIGIN_LAT_E7) / 1e7f) * MAP_METERS_PER_DEG_LAT;

  bool found = false;
  const int16_t nearest = findNearestDownhillBearing(riderEast, riderNorth, &found);
  if (found) mapBearing = nearest;
  const float cosBearing = cosf(static_cast<float>(mapBearing) * PI / 180.0f);
  const float sinBearing = sinf(static_cast<float>(mapBearing) * PI / 180.0f);

  tft.fillScreen(COLOR_BG);

  for (uint16_t i = 0; i < MAP_WAY_COUNT; i++) {
    MapWay way;
    memcpy_P(&way, &mapWays[i], sizeof(MapWay));
    drawMapPolyline(way.firstPoint, way.pointCount, mapWayColor(way), riderEast, riderNorth, cosBearing, sinBearing, fisheyeK);
  }
  for (uint16_t i = 0; i < MAP_LIFT_COUNT; i++) {
    MapLift lift;
    memcpy_P(&lift, &mapLifts[i], sizeof(MapLift));
    drawMapPolyline(lift.firstPoint, lift.pointCount, COLOR_LIFT, riderEast, riderNorth, cosBearing, sinBearing, fisheyeK);
  }

  drawMapLabels(riderEast, riderNorth, cosBearing, sinBearing, fisheyeK);
  drawRiderDot();
  Serial.printf("[%s] Map drawn: bearing=%d K=%u m\n", FIRMWARE_VERSION, mapBearing, MAP_VIEW_METERS[zoom]);
}

void applyLocationPacket(const uint8_t *data, uint8_t len) {
  if (len < 16) return;
  const uint8_t mode = data[2];
  const uint8_t zoom = data[3];
  const int32_t lat = readInt32(data + 4);
  const int32_t lon = readInt32(data + 8);
  const int16_t heading = static_cast<int16_t>(data[12] | (data[13] << 8));
  uint16_t fisheye = static_cast<uint16_t>(data[14] | (data[15] << 8));
  if (fisheye < 100) fisheye = 100;
  if (fisheye > 500) fisheye = 500;

  linkSeen = true;
  lastPacketMs = millis();

  const bool changed = lat != riderLatE7 || lon != riderLonE7 || zoom != mapZoomIndex || heading != mapHeading || fisheye != mapFisheyeRadius;
  riderLatE7 = lat;
  riderLonE7 = lon;
  mapZoomIndex = zoom;
  mapHeading = heading;
  mapFisheyeRadius = fisheye;

  if (mode != displayMode) {
    displayMode = mode;
    if (displayMode == 1) {
      mapHasFix = true;
      mapDirty = true;
    } else {
      layoutDrawn = false;
      telemetryDirty = true;
    }
  } else if (displayMode == 1 && changed) {
    mapDirty = true;
  }
  if (displayMode == 1) mapHasFix = true;
}

void onEspNowRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 3) return;

  if (data[0] == 'S') {
    if (len < 12) return;
    telemetry.speed = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
    telemetry.navAngle = static_cast<int16_t>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
    telemetry.average = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
    telemetry.remaining = static_cast<uint16_t>(data[8]) | (static_cast<uint16_t>(data[9]) << 8);
    telemetry.total = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    if (len >= 13) telemetry.frameEnabled = (data[12] & 0x01) != 0;
    if (telemetry.navAngle < -359) telemetry.navAngle = -359;
    if (telemetry.navAngle > 359) telemetry.navAngle = 359;
    telemetry.valid = true;
    linkSeen = true;
    lastPacketMs = millis();
    telemetryDirty = true;
  } else if (data[0] == 'L') {
    applyLocationPacket(data, len);
  }
}

void startEspNow() {
  if (esp_now_init() != 0) {
    Serial.printf("[%s] ESP-NOW init failed\n", FIRMWARE_VERSION);
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onEspNowRecv);
  espNowReady = true;
  Serial.printf("[%s] ESP-NOW ready (receiver)\n", FIRMWARE_VERSION);
}

void useFallbackChannel() {
  wifi_set_channel(ESPNOW_FALLBACK_CHANNEL);
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
  Serial.printf("[%s] No WiFi networks configured\n", FIRMWARE_VERSION);
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
    if (!mdnsStarted) {
      MDNS.begin(DEVICE_NAME);
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    return;
  }
  const unsigned long elapsed = millis() - wifiAttemptStart;
  if (wifiSearching) {
    if (elapsed > NET_ATTEMPT_TIMEOUT_MS) {
      wifiSearching = false;
      wifiAttemptStart = millis();
      useFallbackChannel();
    }
  } else if (elapsed > NET_SEARCH_INTERVAL_MS) {
    wifiStartNext();
  }
}

void drawArrow(int16_t centerX, int16_t centerY, int16_t angle, float tipLength, float baseLength, uint16_t color) {
  const float radians = angle * PI / 180.0f;
  const int16_t tipX = centerX + static_cast<int16_t>(sinf(radians) * tipLength);
  const int16_t tipY = centerY - static_cast<int16_t>(cosf(radians) * tipLength);
  const int16_t leftX = centerX + static_cast<int16_t>(sinf(radians + 2.45f) * baseLength);
  const int16_t leftY = centerY - static_cast<int16_t>(cosf(radians + 2.45f) * baseLength);
  const int16_t rightX = centerX + static_cast<int16_t>(sinf(radians - 2.45f) * baseLength);
  const int16_t rightY = centerY - static_cast<int16_t>(cosf(radians - 2.45f) * baseLength);

  tft.drawLine(centerX, centerY, tipX, tipY, color);
  tft.drawLine(tipX, tipY, leftX, leftY, color);
  tft.drawLine(tipX, tipY, rightX, rightY, color);
}

void drawLabel(const char *text, int16_t x, int16_t baseline, uint16_t color) {
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(color);
  tft.setCursor(x, baseline);
  tft.print(text);
}

void drawValueRight(const char *text, int16_t right, int16_t baseline, uint16_t color) {
  tft.setFont(&FreeSansBold12pt7b);
  int16_t boundX = 0;
  int16_t boundY = 0;
  uint16_t boundW = 0;
  uint16_t boundH = 0;
  tft.getTextBounds(text, 0, 0, &boundX, &boundY, &boundW, &boundH);
  tft.setTextColor(color);
  tft.setCursor(right - static_cast<int16_t>(boundW), baseline);
  tft.print(text);
}

bool linkFresh() {
  return linkSeen && (millis() - lastPacketMs < LINK_TIMEOUT_MS);
}

void drawStaticLayout() {
  tft.fillScreen(COLOR_BG);
  drawLabel("SPD", 14, 36, COLOR_CYAN);
  drawLabel("REM", 14, 150, COLOR_ORANGE);
  drawLabel("MAX", 14, 188, COLOR_VIOLET);
  drawLabel("TOT", 14, 226, COLOR_YELLOW);
  tft.drawFastHLine(12, 114, 240 - 24, COLOR_DARK);
  layoutDrawn = true;
}

void drawSpeedValue(uint16_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(telemetry.speed));
  tft.fillRect(4, 50, 138, 54, COLOR_BG);
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextColor(color);
  tft.setCursor(14, 92);
  tft.print(buf);
}

void drawRowValue(uint16_t value, int16_t baseline, uint16_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(value));
  tft.fillRect(150, baseline - 22, 86, 30, COLOR_BG);
  drawValueRight(buf, 226, baseline, color);
}

void drawNavArrow(bool fresh) {
  tft.fillRect(140, 18, 94, 92, COLOR_BG);
  tft.drawCircle(186, 64, 44, fresh ? COLOR_GREEN : COLOR_DARK);
  drawArrow(186, 64, fresh ? telemetry.navAngle : 0, 30.0f, 22.0f, fresh ? COLOR_GREEN : COLOR_GRAY);
}

void drawLinkDot(bool fresh) {
  tft.fillRect(10, 8, 16, 16, COLOR_BG);
  tft.fillCircle(18, 16, 5, fresh ? COLOR_GREEN : COLOR_RED);
}

void updateDisplay() {
  if (displayMode != 0) {
    telemetryDirty = false;
    return;
  }

  const bool fresh = linkFresh();
  const uint16_t valueColor = fresh ? COLOR_WHITE : COLOR_GRAY;

  if (!layoutDrawn) {
    drawStaticLayout();
    lastSpeed = lastRemaining = lastAverage = lastTotal = -1;
    lastAngle = -1000;
    lastFresh = !fresh;
  }

  if (telemetry.speed != lastSpeed || fresh != lastFresh) {
    drawSpeedValue(valueColor);
    lastSpeed = telemetry.speed;
  }
  if (telemetry.remaining != lastRemaining || fresh != lastFresh) {
    drawRowValue(telemetry.remaining, 150, valueColor);
    lastRemaining = telemetry.remaining;
  }
  if (telemetry.average != lastAverage || fresh != lastFresh) {
    drawRowValue(telemetry.average, 188, valueColor);
    lastAverage = telemetry.average;
  }
  if (telemetry.total != lastTotal || fresh != lastFresh) {
    drawRowValue(telemetry.total, 226, valueColor);
    lastTotal = telemetry.total;
  }
  if (telemetry.navAngle != lastAngle || fresh != lastFresh) {
    drawNavArrow(fresh);
    lastAngle = telemetry.navAngle;
  }
  if (fresh != lastFresh) {
    drawLinkDot(fresh);
    lastFresh = fresh;
  }

  lastRender = millis();
  telemetryDirty = false;
}

void handleRoot() {
  const bool fresh = linkFresh();
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>HUD Receiver</title></head><body style='font-family:sans-serif'>");
  html += F("<h2>HUD Receiver</h2>");
  html += "<p>Version: " + String(FIRMWARE_VERSION) + "</p>";
  html += "<p>WiFi: " + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("disconnected")) + "</p>";
  html += "<p>IP: " + WiFi.localIP().toString() + " channel: " + String(WiFi.channel()) +
          " RSSI: " + String(WiFi.RSSI()) + " dBm</p>";
  html += "<p>SoftAP: " + String(AP_SSID) + " at " + WiFi.softAPIP().toString() + "</p>";
  html += "<p>ESP-NOW: " + String(espNowReady ? "ready" : "off") + ", link: " +
          (fresh ? String("fresh") : String("stale")) + ", last packet: " +
          (linkSeen ? String((millis() - lastPacketMs)) + " ms ago" : String("never")) + "</p>";
  html += "<ul>";
  html += "<li>SPD " + String(telemetry.speed) + "</li>";
  html += "<li>REM " + String(telemetry.remaining) + "</li>";
  html += "<li>MAX " + String(telemetry.average) + "</li>";
  html += "<li>TOT " + String(telemetry.total) + "</li>";
  html += "<li>NAV " + String(telemetry.navAngle) + "</li>";
  html += "</ul>";
  html += F("<p><a href='/update'>Firmware update</a></p></body></html>");
  httpServer.send(200, "text/html", html);
}

void setupWebServer() {
  httpServer.on("/", handleRoot);
  httpUpdater.setup(&httpServer, "/update", "admin", OTA_PASSWORD);
  httpServer.begin();
}

void setupOta() {
  ArduinoOTA.setHostname(DEVICE_NAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.printf("[%s] OTA update starting\n", FIRMWARE_VERSION); });
  ArduinoOTA.onEnd([]() { Serial.printf("[%s] OTA update finished\n", FIRMWARE_VERSION); });
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nDisplayTest HUD receiver %s\n", FIRMWARE_VERSION);

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, LOW);

  tft.setSPISpeed(TFT_SPI_HZ);
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(0);
  tft.invertDisplay(true);
  tft.fillScreen(COLOR_BG);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.hostname(DEVICE_NAME);
  WiFi.softAP(AP_SSID, AP_PASSWORD, ESPNOW_FALLBACK_CHANNEL);
  Serial.printf("[%s] SoftAP '%s' at %s\n", FIRMWARE_VERSION, AP_SSID, WiFi.softAPIP().toString().c_str());

  startEspNow();
  setupWebServer();
  setupOta();
  wifiStartNext();

  drawStaticLayout();
  updateDisplay();
}

void loop() {
  httpServer.handleClient();
  ArduinoOTA.handle();
  serviceWifi();
  if (mdnsStarted) MDNS.update();

  if (displayMode == 1) {
    if (mapDirty && millis() - lastRender > RENDER_MIN_INTERVAL_MS) {
      drawMap();
      mapDirty = false;
      lastRender = millis();
    }
  } else if ((telemetryDirty || linkFresh() != lastFresh) && millis() - lastRender > RENDER_MIN_INTERVAL_MS) {
    updateDisplay();
  }
  delay(1);
}
