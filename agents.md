# DisplayTest: context for agents

## Project purpose

DisplayTest is a live HUD editor for a Waveshare ESP32-S3-Zero and a 128x64
SSD1306 OLED display. The PWA edits telemetry values and two bitmap layers,
then sends them over BLE to the ESP32. The ESP32 renders the final HUD locally
on the OLED.

The current product direction is a compact monochrome instrument display with
the following priorities:

- Keep the left telemetry area readable and separated from the map.
- Use the complete OLED height for the bottom values.
- Keep the map and route visible at the same time.
- Use several synchronized moving gaps so the route animation is visible over
  its full length without waiting for one gap to travel from bottom to top.

## Current state

- Firmware version (sender): `v0.7.0`.
- Firmware version (receiver): `v0.1.0`.
- PWA package version: `0.7.0`.
- Board FQBN (sender): `esp32:esp32:waveshare_esp32_s3_zero`.
- Board FQBN (receiver): `esp8266:esp8266:nodemcuv2:eesz=4M1M`.
- Upload port (sender): `COM6`.
- Receiver OTA address: `192.168.100.107` (DHCP; it can change — discover it
  with `arduino-cli board list` or `hud-receiver.local`).
- OLED address: `0x3C`.
- ESP-NOW fallback channel: `ESPNOW_FALLBACK_CHANNEL` (default `1`).
- PWA deployment URL: `https://dasiuss.github.io/DisplayTest/`.
- Firmware source (sender): `firmware/hud_display/hud_display.ino`.
- Firmware source (receiver): `firmware/hud_receiver/hud_receiver.ino`.
- PWA entry point: `pwa/src/main.js`.
- Local serial monitor: `monitor_COM6.cmd`.

The local `monitor_COM6.cmd` file is intentionally untracked. Do not add it to
commits unless the user explicitly asks for that.

The latest `TOTAL` alignment was requested as a firmware-only adjustment. The
firmware currently places `TOTAL` at `x=50`, while the PWA preview still has it
at `x=62`. Do not silently modify the PWA when a user asks for a firmware-only
display adjustment.

## Hardware

| OLED pin | ESP32-S3-Zero |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO1 |
| SCL | GPIO2 |

The firmware initializes I2C explicitly with SDA `GPIO1`, SCL `GPIO2`, and a
400 kHz bus speed. The display is initialized as an SSD1306 using address
`0x3C`.

## Repository layout

- `README.md`: project overview and hardware pinout.
- `agents.md`: this file, containing implementation context and rules for
  future agents.
- `firmware/hud_display/hud_display.ino`: ESP32-S3 sender. BLE server, packet
  handling, OLED rendering, bitmap drawing, route animation, ESP-NOW sender.
- `firmware/hud_receiver/hud_receiver.ino`: ESP8266 receiver ("duży ekran" /
  "rękaw"). ESP-NOW listener, ST7789 rendering, map drawing, OTA/web server.
- `firmware/hud_receiver/map_data.h`: generated Sölden map data.
- `firmware/secrets.example.h`: template for the untracked `secrets.h`.
- `tools/build-map.mjs`: one-off generator for `hud_receiver/map_data.h`.
- `firmware/README.md`: firmware-specific Arduino CLI instructions.
- `pwa/index.html`: PWA markup and visible version labels.
- `pwa/src/main.js`: PWA state, canvas preview, BLE transport, bitmap editor,
  and route animation preview.
- `pwa/src/style.css`: PWA visual design and responsive layout.
- `pwa/package.json`: PWA scripts and version.
- `pwa/package-lock.json`: locked npm dependency versions.
- `pwa/public/sw.js`: service worker cache name and cache handling.
- `.github/workflows/deploy-pages.yml`: GitHub Pages build and deployment.

## OLED layout: "zoptymalizowana z mapa"

The physical display is 128x64 pixels. Coordinates below use the OLED origin:
`x=0` at the left edge and `y=0` at the top edge. The last valid row is
`y=63`.

The display is divided into a telemetry panel and a map panel:

- Telemetry area: `x=0..86`.
- Separator: vertical line at `x=87`.
- Map area: `x=88..127`, exactly 40 pixels wide.
- Map height: all rows `y=0..63`, exactly 64 pixels.

The current firmware layout is the reference physical layout:

| Element | Firmware position | Font/size | Notes |
| --- | --- | --- | --- |
| `SPD` | `x=1, y=0` | Adafruit default, size 1 | Speed label. |
| Speed value | `x=1, y=9` | Adafruit default, size 3 | Uses `%3u`; moved down one pixel. |
| `REM` | `x=1, y=37` | Adafruit default, size 1 | Moved one pixel toward its value. |
| Remaining value | `x=1, y=48` | Adafruit default, size 2 | Occupies the final 16-pixel block and reaches row `y=63`. |
| Navigation arrow | center `x=69, y=10` | line drawing | Angle `0` points up; supports `-359..359`. |
| `MAX` | `x=62, y=27` | Adafruit default, size 1 | This label currently displays `hudState.average`. |
| MAX value | `x=62, y=35` | Adafruit default, size 1 | Uses `%3u`. |
| `TOTAL` | `x=50, y=48` | Adafruit default, size 1 | Five letters, shifted two character cells left of `MAX`; right edge aligns with `MAX`. |
| Total value | `x=62, y=56` | Adafruit default, size 1 | Uses `%3u` and reaches row `y=63`. |
| Separator | `x=87, y=0..63` | one-pixel line | Keeps text out of the map. |
| Map | `x=88, y=0` | 40x64 bitmap | `mapBitmap` and `routeBitmap` are rendered here. |

The default Adafruit GFX font advances six pixels per character at size 1.
`MAX` starts at `x=62` and occupies three character cells. `TOTAL` has five
characters, so its aligned start is `62 - (5 - 3) * 6 = 50`. It must not be
returned to `x=56` or `x=62`, because it would extend farther right than
`MAX` and approach or overlap the map separator.

The bottom values are intentionally lower than their labels:

- `REM` label starts at `y=37`; its size-2 value starts at `y=48` and uses
  rows through `y=63`.
- `TOTAL` label starts at `y=48`; its size-1 value starts at `y=56` and uses
  rows through `y=63`.

### PWA preview coordinates

The PWA uses an HTML canvas, where `fillText` coordinates are text baselines,
not Adafruit GFX cursor origins. It mirrors most of the physical layout, but
the PWA was deliberately not changed during the last firmware-only alignment
request. Current relevant PWA coordinates are:

- `SPD` baseline `y=6`; speed baseline `y=30`.
- `REM` baseline `y=39`; remaining baseline `y=63`.
- `MAX` baseline `y=32`; its value baseline `y=42`.
- `TOTAL` baseline `x=62, y=55`; its value baseline `x=62, y=63`.
- Preview separator at `x=87.5`.
- Preview map starts at `x=88` and is 40x64.

The PWA currently calls the data field `average` and sends it as the average
value in the BLE packet, even though the physical label is `MAX`. Renaming
the data field or the control label is a separate semantic change and must
not be assumed from the visual label change alone.

## Route and map layers

The bitmap dimensions are shared by the PWA and firmware:

- Width: 40 pixels.
- Height: 64 pixels.
- Row bytes: 5.
- Total bitmap bytes: 320.
- PWA chunk size: 16 bytes.
- Total chunks per complete bitmap: 20.

There are two independent layers:

- `MAPA` / `mapBitmap`: static minimap background.
- `TRASA` / `routeBitmap`: route pixels that receive the animation.

The PWA editor switches between the two layers. The firmware draws the map
first and the route second. Both are white on the physical monochrome OLED.
The PWA uses separate colors in its editor and preview to make the layers
easier to distinguish.

## Route animation

### Ordered route pixels

The current route order is deliberately simple and shared by the PWA and
firmware:

1. Start at the bottom row, `y=63`.
2. Scan `x=0..39` from left to right.
3. Continue with rows `y=62` down to `y=0`.
4. Add only pixels set in `routeBitmap`.

The resulting ordered list is called `routeOrder` in the PWA and is represented
by the same scan in `drawAnimatedRoute()` in the firmware. It is an ordered
bottom-to-top sweep, not a graph traversal of connected neighboring pixels.
For that reason, do not describe it as true path following. Replacing it with
neighbor traversal would be a separate design change requiring a policy for
branches, gaps, loops, and disconnected route segments.

### Number and length of gaps

The animation uses a fixed gap length of five route pixels. The number of
simultaneous gaps depends on the number of set route pixels, `N`:

| Route pixels `N` | Gap count `K` |
| --- | --- |
| `N <= 15` | 1 |
| `16 <= N <= 30` | 2 |
| `31 <= N <= 45` | 3 |
| `N > 45` | 4 |

The five-pixel length is intentionally fixed, including for short routes. If
a route has fewer than five pixels, a five-pixel cyclic gap can cover every
route pixel. This is an unavoidable consequence of the fixed-length rule and
should only be changed if the user explicitly requests adaptive gap length.

### Synchronization and spacing

There is one shared animation cursor, not one independent timer per gap. Let:

- `N` be the number of route pixels.
- `K` be the selected gap count.
- `C` be the shared cursor, normalized to `0..N-1`.
- `i` be a gap index from `0` to `K-1`.

The start position for gap `i` is:

```text
gapStart(i) = (C + floor(i * N / K)) mod N
```

Each gap blacks out the five ordered route pixels beginning at its own
`gapStart`:

```text
routeOrder[(gapStart(i) + offset) mod N]
for offset = 0..4
```

This means the gaps are evenly distributed by progress along the ordered
route, not by screen `y` quarters. If one screen quarter contains more route
pixels than another, no independent animation can drift because every gap is
derived from the same cursor. When `N` is not divisible by `K`, the floor
rounding makes the spacing differ by at most one ordered route position.

### Timing

- PWA timer: `ROUTE_ANIMATION_INTERVAL = 28` ms.
- Firmware timer: `ROUTE_STEP_MS = 28` ms.
- Every step increments the shared cursor by one.
- Firmware route bitmap updates reset `routeCursor` to zero.
- Firmware renders only when `screenDirty` is true and at least 10 ms passed
  since the previous render.
- The complete route is drawn first; the five-pixel gaps are then drawn in
  black over it.

The PWA must use the same gap count, fixed gap length, route ordering, and gap
start formula as the firmware so its preview represents the physical display.

## BLE protocol

The BLE service UUID is:

```text
5f8a0001-4e56-4e46-9a7c-000000000001
```

The writable characteristic UUID is:

```text
5f8a0001-4e56-4e46-9a7c-000000000002
```

### State packet

The PWA sends a 13-byte state packet:

| Byte(s) | Meaning | Encoding |
| --- | --- | --- |
| `0` | Packet type | ASCII `S` / `0x53` |
| `1` | Sequence | unsigned 8-bit |
| `2..3` | Speed | unsigned 16-bit little-endian |
| `4..5` | Navigation angle | signed 16-bit little-endian |
| `6..7` | Average/MAX data field | unsigned 16-bit little-endian |
| `8..9` | Remaining | unsigned 16-bit little-endian |
| `10..11` | Total | unsigned 16-bit little-endian |
| `12` | Protocol marker | currently `1` |

Firmware clamps the navigation angle to `-359..359`. Numeric HUD values are
stored as unsigned 16-bit values.

### Bitmap packets

Bitmap packets have a four-byte header followed by up to 16 data bytes:

| Byte | Meaning |
| --- | --- |
| `0` | `M` for map or `R` for route |
| `1` | Bitmap sequence |
| `2` | Chunk index |
| `3` | Total chunk count |
| `4..19` | Packed bitmap data |

The firmware collects all chunks for one sequence before replacing the active
bitmap. A complete route transfer resets the animation cursor.

### Geo packet

The PWA sends a 16-byte geo packet:

| Byte(s) | Meaning | Encoding |
| --- | --- | --- |
| `0` | Packet type | ASCII `G` / `0x47` |
| `1` | Sequence | unsigned 8-bit |
| `2..5` | Latitude | signed 32-bit little-endian, degrees * 1e7 |
| `6..9` | Longitude | signed 32-bit little-endian, degrees * 1e7 |
| `10` | Display mode | `0` stats, `1` map |
| `11..12` | Heading | signed 16-bit little-endian, `-1` unknown |
| `13` | Zoom index | unsigned 8-bit |
| `14..15` | Fisheye | unsigned 16-bit little-endian |

The receiver clamps fisheye to `100..500`.

## ESP-NOW transport

Two boards talk over ESP-NOW:

- `firmware/hud_display/hud_display.ino` (ESP32-S3): broadcasts telemetry
  (`S`) and the rider position (`L`).
- `firmware/hud_receiver/hud_receiver.ino` (ESP8266, the "duży ekran" /
  "rękaw"): receives them and renders stats or the map.

ESP-NOW has no association step: it only requires both radios to be on the
same WiFi channel. The firmware uses a hybrid scheme so the link works both at
home (with a router) and away (without one):

- While a network from the shared `secrets.h` list is reachable, both boards
  connect to the first reachable network and use that AP's channel.
- When no network is reachable, both boards pin the radio to
  `ESPNOW_FALLBACK_CHANNEL` (default `1`) via `esp_wifi_set_channel()` on the
  ESP32 and `wifi_set_channel()` on the ESP8266, then retry the network list
  every 30 s (`WIFI_SEARCH_INTERVAL_MS` / `NET_SEARCH_INTERVAL_MS`).
- The sender transmits whenever `espNowReady` is true; it is **not** gated on
  `wifiConnected`. The receiver always listens.
- `WiFi.setAutoReconnect(false)` is set on both boards so the background
  reconnect does not move the radio to another channel.

The receiver's SoftAP (`HUD-Receiver` / `hudreceiver`, 192.168.4.1) is only
for programming/OTA and is not used by ESP-NOW. In `WIFI_AP_STA` the ESP8266
SoftAP follows the STA channel, so it is on the router channel when connected
and on the fallback channel otherwise.

Transitions between the router channel and the fallback channel can briefly
desynchronize the two radios until both settle on the same channel.

### ESP-NOW packets

- `S` (S3 to receiver), 13 bytes: the same telemetry packet the PWA sends over
  BLE (`S`, seq, speed, nav angle, average/MAX, remaining, total, frame).
- `L` (S3 to receiver), 16 bytes: `'L'`, seq, `uint8 mode` (0 stats, 1 map),
  `uint8 zoom`, `int32 lat*1e7`, `int32 lon*1e7`, `int16 heading`,
  `uint16 fisheye`.

## Development and upload commands

Run PWA commands from `pwa`:

```text
npm install
npm run dev
npm run build
node --check src/main.js
```

Compile firmware from the repository root:

```text
arduino-cli compile --fqbn esp32:esp32:waveshare_esp32_s3_zero firmware/hud_display
```

Upload firmware to the connected board:

```text
arduino-cli compile --fqbn esp32:esp32:waveshare_esp32_s3_zero --upload --port COM6 firmware/hud_display
```

Compile and OTA-upload the receiver (ESP8266):

```text
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M firmware/hud_receiver
arduino-cli upload -p 192.168.100.107 --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M --upload-field password=<OTA_PASSWORD> firmware/hud_receiver
```

If `arduino-cli upload` reports `No response from device`, build with
`--output-dir build` and POST the `.bin` to `http://<ip>/update` with basic auth
`admin` / `OTA_PASSWORD`.

The serial monitor uses 115200 baud and native USB CDC:

```text
monitor_COM6.cmd
```

Close the monitor before uploading because it may hold `COM6`. Start it again
after a successful upload. Do not commit the local monitor script unless
explicitly requested.

## Rules for future changes

- Inspect the existing source before changing coordinates or behavior.
- Keep PWA and firmware rendering synchronized for any shared behavior.
- If the user says "only firmware", modify only
  `firmware/hud_display/hud_display.ino`; do not change the PWA, version
  labels, package files, or service worker.
- Do not bump the version for a firmware-only adjustment unless the user asks
  for a version change.
- Do not commit or push a firmware-only adjustment unless the user asks for
  that. The physical upload can be performed with a local source change.
- Do not stage `monitor_COM6.cmd`.
- Preserve unrelated user changes in the worktree.
- Use `apply_patch` for manual edits.
- Run `git diff --check` after edits.
- For display changes, compile the firmware before upload and verify the
  expected FQBN and port.
- When uploading, stop the serial monitor first and restart it after upload.
- For route animation changes, test the threshold cases `15`, `16`, `30`,
  `31`, `45`, and `46` route pixels.
- Avoid changing route order to a connected-path algorithm without discussing
  branch and disconnected-pixel behavior first.
- Keep the ESP-NOW sender and receiver on the same WiFi channel. The sender
  must transmit whenever ESP-NOW is ready; do not gate sending on
  `wifiConnected`.
- Do not change `ESPNOW_FALLBACK_CHANNEL` on only one board; both must match.
- The receiver is flashed over OTA (`/update` or `arduino-cli upload -p <ip>`),
  not over a serial port.
