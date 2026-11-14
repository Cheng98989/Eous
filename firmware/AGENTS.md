# Repository Guidance

## Project

- This is an ESP32-S3 Arduino firmware project managed by PlatformIO.
- The active environment is `esp32s3` in `platformio.ini`; it uses `esp32-s3-devkitc-1` and NimBLE-Arduino 2.5.2.
- `src/ble_test.cpp` contains the BLE server, BOOT-button handling, and main loop.
- `src/firefox_launcher.cpp` and `src/firefox_launcher.h` implement USB HID keyboard commands for the connected host.
- PlatformIO tests belong in `test/`; the directory currently contains only its README.

## Build and Verification

- Build firmware: `pio run -e esp32s3`
- Upload to a connected board: `pio run -e esp32s3 -t upload`
- Open the serial monitor: `pio device monitor -b 115200`
- Run PlatformIO tests when tests exist: `pio test -e esp32s3`

## Change Guidance

- Keep board-specific settings and dependencies in `platformio.ini`.
- Preserve the existing BLE service and characteristic UUIDs unless the change explicitly requires a protocol update.
- Keep launcher declarations in `firefox_launcher.h` consistent with their definitions and call sites.
- Avoid changing hardware pin assignments or USB keyboard behavior without an explicit requirement.