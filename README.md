# ESP32 Mouse Jiggler

USB mouse jiggler for the [LilyGO T-Display-S3 Pro](https://wiki.lilygo.cc/products/t-display-series/t-display-s3-pro/). The board enumerates as a HID mouse, moves the pointer a few pixels and back, and shows what it is doing on the 2.33 inch panel.

Based on [rithwikmakesthings/esp32s3-Mouse-Jiggler](https://github.com/rithwikmakesthings/esp32s3-Mouse-Jiggler/).

## What it does

- **Jiggle** eases 10–20 px out and back on one axis every 30–90 seconds, for up to 8 hours.
- **Nudge** draws a 40 px square around the pointer and returns to the start.
- The status line shows the countdown, HID reports sent, finished jiggles, and the current nudge side.
- The red-circle touch key toggles the backlight. Jiggling keeps running while the panel is dark.
- **USB** disconnects and reconnects the mouse. The host sees a Dell Laser Mouse MS3220.

## Build and flash

[PlatformIO](https://platformio.org/) with the pioarduino ESP32 Arduino 3.x platform. LovyanGFX is fetched automatically. The CST226 touch driver is the SensorLib copy in `lib/SensorLib`.

```bash
pio run -e jiggler -t upload
```

| Environment | USB | Upload |
| --- | --- | --- |
| `jiggler` (default) | HID mouse, no serial port | Hold **BOOT**, tap **RESET**, release **BOOT**, then upload |
| `jiggler-debug` | HID mouse plus serial at 115200 | Upload can auto-reset. **USB** disconnect also drops that serial port |

`jiggler-debug`:

```bash
pio run -e jiggler-debug -t upload
```

Open the serial monitor at 115200 on the debug build. The sketch itself does not print a log; the port is there so the host can reset the board.

## Hardware

LilyGO T-Display-S3 Pro: ESP32-S3R8, 16 MB flash, 8 MB PSRAM, ST7796 222×480.

| Function | Pin |
| --- | --- |
| TFT SPI | SCK 18, MOSI 17, MISO 8, CS 39, DC 9, RST 47 |
| Backlight | 48 (V1.1 is a 16-step pulse pin) |
| Touch and SY6970 | SDA 5, SCL 6, touch reset 13, IRQ 21 |
| Buttons | BOOT 0, 12, 16 |

The panel is wired in the sketch. Do not substitute the non-Pro variant pin macros. USB HID needs USB-OTG (`ARDUINO_USB_MODE=0`). The board manifest sets Hardware CDC; `platformio.ini` removes that flag.

## Tuning

Distances, pauses, and the 8 hour limit are the `kJiggle*` and `kHoursToRun` constants at the top of `src/esp32-Mouse-Jiggler.ino`.
