
# Main documentation page for T-Display S3 Pro

https://wiki.lilygo.cc/products/t-display-series/t-display-s3-pro/


# T-Display S3 Pro References (quick start)

## Required Libraries

Copy the project `lib/` directory to your Arduino libraries folder, or install the libraries below:

| Library | Version | Source |
| :-----: | :-----: | :----: |
| LovyanGFX | Latest | [GitHub](https://github.com/lovyan03/LovyanGFX) |
| LilyGo-display-library | Latest | [Xinyuan-LilyGO/LilyGo-display-library](https://github.com/Xinyuan-LilyGO/LilyGo-display-library) |
| LVGL | 8.x | [GitHub](https://github.com/lvgl/lvgl) |
| XPowersLib | Latest | [GitHub](https://github.com/lewisxhe/XPowersLib) |
| SensorLib | Latest | [GitHub](https://github.com/lewisxhe/SensorLib) |
| TouchLib | Latest | [GitHub](https://github.com/mmMicky/TouchLib) |
| JPEGDEC | Latest | [GitHub](https://github.com/bitbank2/JPEGDEC) |

---

## Arduino

### PlatformIO (Recommended)

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension
2. Clone the repository:
   ```bash
   git clone https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro.git
   ```
3. Open `platformio.ini` and enable one target example
4. Build, connect via USB-C, then upload

### Arduino IDE

| Setting | Value |
| :-----: | :---: |
| Board | **ESP32S3 Dev Module** |
| Upload Speed | 921600 |
| USB Mode | **Hardware CDC and JTAG** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | **240 MHz (WiFi)** |
| Flash Mode | **QIO 80 MHz** |
| Flash Size | **16 MB (128Mb)** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** |

> Set **USB CDC On Boot** to **Disabled** when running from battery only.