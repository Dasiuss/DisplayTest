# Firmware

Two sketches plus one offline data generator:

- `hud_display/`: ESP32-S3 (Waveshare ESP32-S3-Zero). Drives the small
  SSD1306 OLED and is a BLE server for the PWA. It forwards telemetry (`S`) and
  the rider position (`L`) over ESP-NOW. It no longer talks to the internet.
- `hud_receiver/`: ESP8266 (GeekMagic SmallTV / SmallTV-Ultra; in conversation we
  call this device the **duży ekran** / **rękaw**). Holds the whole
  Sölden map in flash (`map_data.h`) and renders it locally on the 1.54" 240x240
  ST7789. Receives only a small position update every few seconds. OTA over WiFi
  (ArduinoOTA + web `/update`).
- `tools/build-map.mjs`: fetches and filters the map once and writes
  `hud_receiver/map_data.h` (see below).

## Secrets

Each sketch loads `secrets.h` from its own folder (untracked). Copy the example
into both sketch folders and fill in your networks:

```text
copy firmware\secrets.example.h firmware\hud_display\secrets.h
copy firmware\secrets.example.h firmware\hud_receiver\secrets.h
```

`secrets.h` holds an ordered list of WiFi networks, `OTA_PASSWORD`, and
`ESPNOW_FALLBACK_CHANNEL`. Both boards try the networks in order and, when none
is reachable, pin the radio to `ESPNOW_FALLBACK_CHANNEL` (default `1`) so
ESP-NOW keeps working without a router. The receiver also raises its own
fallback AP `HUD-Receiver` / `hudreceiver` (192.168.4.1) so it stays reachable
for OTA even if none of the configured networks work.

## Map data generator (`tools/build-map.mjs`)

The receiver keeps the map in flash, so OpenStreetMap is queried only when the
data has to be refreshed. The generator:

- downloads Overpass data for the Sölden bbox `46.93,10.92,47.02,11.10`;
- keeps only line ways (no `area=yes`, no relations):
  - pistes with `piste:type` in `downhill`, `snow_park`, `connection`
    (stored with name and difficulty);
  - real lifts (`gondola`, `chair_lift`, `cable_car`, `mixed_lift`, `drag_lift`,
    `t-bar`, `j-bar`, `platter`, `rope_tow`, `magic_carpet`, `funicular`);
    `station`/`pylon`/`goods`/`explosive` and `proposed:`/`construction:`/...
    are dropped;
- simplifies geometry and stores points as `int16` metres;
- computes each piste downhill bearing from the OSM way direction (for
  `piste:type=downhill` the way is drawn downhill by convention);
- validates that bearing against the Open-Meteo elevation API and flips the ones
  whose elevation says they point uphill (pass `--no-flip` to only list them);
  every decision is recorded in `tools/map-report.txt`.

```text
node tools/build-map.mjs            # use the cache, only hit the API if needed
node tools/build-map.mjs --refresh  # force a fresh Overpass + elevation fetch
```

Raw responses are cached under `tools/data/` (gitignored) so re-runs never
touch the API. Output: `hud_receiver/map_data.h` (~33 KB, ~1.5k points).

Data (c) OpenStreetMap contributors (ODbL). Elevation (c) Open-Meteo.

## ESP32-S3 sender (`hud_display`)

```text
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library"
arduino-cli compile --fqbn "esp32:esp32:waveshare_esp32_s3_zero:PartitionScheme=min_spiffs" firmware/hud_display
arduino-cli upload --fqbn "esp32:esp32:waveshare_esp32_s3_zero:PartitionScheme=min_spiffs" --port COM6 firmware/hud_display
```

`min_spiffs` is kept as the flash layout used by the board (the current sketch
also fits the default partition). The sender broadcasts telemetry (`S`) and, on
every BLE `G`, the rider position (`L`), plus a 2 s heartbeat. It transmits
whenever ESP-NOW is ready — it does not need a WiFi connection. See
[ESP-NOW link](#esp-now-link) for how the two radios stay on the same channel.

## ESP8266 receiver (`hud_receiver`)

```text
arduino-cli core install esp8266:esp8266
arduino-cli lib install "Adafruit ST7735 and ST7789 Library"
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M firmware/hud_receiver
```

The `4M1M` layout matches the original GeekMagic firmware and gives an OTA slot
of about 1019 KB. First install over UART (GPIO0 to GND, USB-TTL at 3.3V);
afterwards update over the air:

```text
arduino-cli upload -p 192.168.100.107 --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M --upload-field password=<OTA_PASSWORD> firmware/hud_receiver
```

Or open `http://192.168.100.107/update` (user `admin`, password `OTA_PASSWORD`)
and upload the built `hud_receiver.ino.bin`.

`arduino-cli upload` uses `espota` and can fail with `No response from device`
(seen with a weak WiFi signal). The `/update` page is the reliable fallback:
build the binary first, then POST it.

```text
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M --output-dir build firmware/hud_receiver
curl -u admin:<OTA_PASSWORD> -F "firmware=@build/hud_receiver.ino.bin" http://192.168.100.107/update
```

## ESP-NOW link

ESP-NOW has no association step: it only needs both radios on the same WiFi
channel. The firmware uses a hybrid scheme so the link works both at home
(with a router) and away (without one):

- While a network from the shared `secrets.h` list is reachable, both boards
  connect to the first reachable network and use that AP's channel.
- When no network is reachable, both boards pin the radio to
  `ESPNOW_FALLBACK_CHANNEL` (default `1`) — `esp_wifi_set_channel()` on the
  ESP32-S3, `wifi_set_channel()` on the ESP8266 — and retry the network list
  every 30 s.
- The sender transmits whenever `espNowReady` is true; it is not gated on
  `wifiConnected`. The receiver always listens.
- `WiFi.setAutoReconnect(false)` is set on both boards so a background
  reconnect does not move the radio to another channel.

The receiver's SoftAP is only for programming/OTA and is not used by ESP-NOW.
In `WIFI_AP_STA` the ESP8266 SoftAP follows the STA channel, so it is on the
router channel when connected and on the fallback channel otherwise.

Transitions between the router channel and the fallback channel can briefly
desynchronize the two radios until both settle on the same channel.

## Display modes

The PWA has a `Statystyki` / `Mapa` switch that selects what the receiver shows.

- Stats mode: the SPD / REM / MAX / TOT layout, updating only changed values.
- Map mode: a rider-centred map rendered from `map_data.h`. The view is rotated
  so the downhill bearing of the nearest `downhill` piste points up. Zoom is a
  fixed set of presets (250 / 500 / 1000 / 2000 / 4000 m) selected in the PWA.
  Pistes are coloured by difficulty (novice green, easy blue, intermediate red,
  advanced white, freeride yellow; snow park magenta; connection grey), lifts
  orange. Names are drawn near the feature: piste names as-is (mostly 1-2 digit
  numbers), lift names shortened to three characters. A blue dot marks the rider
  at the bottom centre.

## Packets

- BLE `S` (PWA to S3), 13 bytes: `'S'`, seq, `uint16 speed`,
  `int16 navAngle`, `uint16 average/MAX`, `uint16 remaining`, `uint16 total`,
  `uint8 frame`.
- BLE `G` (PWA to S3), 16 bytes: `'G'`, seq, `int32 lat*1e7`, `int32 lon*1e7`,
  `uint8 mode` (0 stats, 1 map), `int16 heading` (-1 unknown), `uint8 zoom`,
  `uint16 fisheye`.
- ESP-NOW `S` (S3 to receiver), 13 bytes: the same telemetry packet the PWA
  sends over BLE.
- ESP-NOW `L` (S3 to receiver), 16 bytes: `'L'`, seq, `uint8 mode`,
  `uint8 zoom`, `int32 lat*1e7`, `int32 lon*1e7`, `int16 heading`,
  `uint16 fisheye`.

## Piste direction report

`tools/map-report.txt` lists the elevation check per piste
(`ok` / `flipped` / `flat`) with start and end elevation. `flipped` means the
OSM way direction disagreed with the terrain and the stored bearing was rotated
by 180 degrees.
