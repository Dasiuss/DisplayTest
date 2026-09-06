# ESP32-S3 HUD firmware

## Arduino CLI

Install the ESP32 core and libraries once:

```text
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library"
```

Compile the sketch for the Waveshare board after checking the exact FQBN reported by `arduino-cli board list`:

```text
arduino-cli compile --fqbn esp32:esp32:waveshare_esp32_s3_zero firmware/hud_display
```

The sketch logs its firmware version at boot over native USB CDC at 115200 baud. It uses GPIO1 for SDA, GPIO2 for SCL, and OLED address `0x3C`.
