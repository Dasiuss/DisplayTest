#pragma once

// Copy this file to firmware/secrets.h (untracked, see .gitignore) and fill in
// your own networks. Both sketches try the networks in order until one connects.
//
//   copy firmware\secrets.example.h firmware\secrets.h

#define WIFI_NETWORK_COUNT 2

static const char *WIFI_SSIDS[WIFI_NETWORK_COUNT] = {
    "MyNetwork2G",
    "AnotherNetwork",
};

static const char *WIFI_PASSES[WIFI_NETWORK_COUNT] = {
    "password1",
    "password2",
};

// Password for ArduinoOTA and the web /update page (user: admin).
#define OTA_PASSWORD "hud-ota"

// Channel used by the sender when it has no WiFi connection yet.
#define ESPNOW_FALLBACK_CHANNEL 1
