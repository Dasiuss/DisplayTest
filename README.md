# DisplayTest

Live HUD editor for an ESP32-S3-Zero and a 128x64 SSD1306 OLED display.

## Hardware

Connect the OLED module as follows:

| OLED | Waveshare ESP32-S3-Zero |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | IO1 / GPIO1 |
| SCL | IO2 / GPIO2 |

The firmware uses I2C address `0x3C` and explicitly initializes I2C with GPIO1 as SDA and GPIO2 as SCL.

## PWA

```text
cd pwa
npm install
npm run dev
```

The production build is created in `pwa/dist`. GitHub Pages deployment is defined in `.github/workflows/deploy-pages.yml`.

Web Bluetooth requires a Chromium-based browser and HTTPS in production. `localhost` is allowed during development.

## Firmware

The firmware is in `firmware/hud_display`. It uses the Arduino ESP32 core and the `Adafruit SSD1306` and `Adafruit GFX Library` libraries.

The current firmware version is printed at boot over USB CDC at 115200 baud. The current PWA version is visible in its header and connection panel.

The Waveshare board uses native USB. If the board is not detected for upload, hold BOOT, press RESET, release RESET, then release BOOT.
