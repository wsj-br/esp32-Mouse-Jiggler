# Development

Firmware for the LilyGO T-Display-S3 Pro. Board reference: [T-Display-S3 Pro](https://wiki.lilygo.cc/products/t-display-series/t-display-s3-pro/).

Build and flash from Cursor with the PlatformIO extension. This project is not set up for the Arduino IDE. USB HID needs USB-OTG (`ARDUINO_USB_MODE=0`), which `platformio.ini` already selects.

## Cursor

1. Install [Cursor](https://cursor.com/).
2. **File → Open Folder** and open this repository. Open the folder that contains `platformio.ini`, not a parent directory.
3. When Cursor offers workspace recommendations, install them. They are listed in `.vscode/extensions.json`:
   - **PlatformIO IDE** (`platformio.platformio-ide`)
   - **clangd** (`llvm-vs-code-extensions.vscode-clangd`)
4. Skip **pioarduino IDE** and the Microsoft **C/C++ Extension Pack**. This workspace marks both as unwanted. The ESP32 platform package is pioarduino; the editor extension is the official PlatformIO IDE.
5. Wait until PlatformIO finishes its first-time setup. The first build also downloads the ESP32 platform and needs a network connection. The status bar shows a PlatformIO home icon once the extension is ready.

Reload the window if the status-bar icons do not appear after the extension installs.

On Linux, your user must be allowed to open the serial device:

```bash
sudo usermod -a -G dialout $USER
```

Log out and back in so the new group applies.

## Serial port

`platformio.ini` writes the firmware to the port in `upload_port`. The serial monitor uses `monitor_port`. Both are set to `/dev/ttyACM1` in the repo; change them to the port on this machine before uploading.

List what the kernel sees:

```bash
pio device list
```

Or watch the kernel log, plug in the board, and note the new `ttyACM` name:

```bash
dmesg -W
```

Put that name in both settings:

```ini
monitor_port = /dev/ttyACM0
upload_port = /dev/ttyACM0
```

Which device appears depends on the environment.

**`jiggler` (default).** The running firmware is a HID mouse with no serial port, so nothing shows up as `ttyACM` while it is running. Hold **BOOT**, tap **RESET**, then release **BOOT**. The ROM bootloader enumerates a serial device. Set `upload_port` to that device and start the upload while the board is still in the bootloader.

**`jiggler-debug`.** The firmware is a HID mouse plus a serial port at 115200, so a `ttyACM` device is present while the sketch is running. PlatformIO can reset the board for the upload. The on-screen **USB** button disconnects that port until you connect USB again.

An RTS reset leaves this board sitting in the ROM bootloader. `platformio.ini` uses a watchdog reset after upload so the new firmware actually starts.

## Build and flash

The status bar is the PlatformIO toolbar. The active environment is shown there and defaults to `jiggler`. Click the environment name to switch to `jiggler-debug`.

| Status-bar action | What it does |
| --- | --- |
| Build (checkmark) | Compile the active environment |
| Upload (arrow) | Build, then write the firmware to `upload_port` |
| Serial Monitor (plug) | Open `monitor_port` at 115200 |

The same actions are under **PlatformIO → Project Tasks → *env*** in the sidebar, and in the command palette as **PlatformIO: Build** and **PlatformIO: Upload**.

For the default firmware:

1. Set `upload_port` as above.
2. Leave the status-bar environment on `jiggler`.
3. Put the board in the bootloader (hold **BOOT**, tap **RESET**, release **BOOT**).
4. Click **Upload**.

For the debug firmware, switch the environment to `jiggler-debug`, then click **Upload**. After it boots, **Serial Monitor** opens the CDC port. The sketch does not print a log; the port exists so upload can reset the board.

LovyanGFX is downloaded on the first build. The touch driver is the SensorLib tree already in `lib/SensorLib`.
