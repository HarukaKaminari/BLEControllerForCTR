# BLEControllerForCTR

This repository provides a small ESP-IDF-based BLE OTA controller demo that keeps the wireless firmware update path separated from the normal application runtime.

The design is intentionally modular:

- [main/blink_example_main.c](main/blink_example_main.c) keeps the application entrypoint and LED runtime logic lightweight.
- [main/ble_ota.c](main/ble_ota.c) implements the BLE transport, GATT service, OTA command/data parsing, and the partition commit path.
- [main/ble_ota.h](main/ble_ota.h) exposes the public OTA module interface.

## Project Goal

The firmware is structured to support a BLE-based remote firmware update flow on ESP32-class devices, with the OTA implementation isolated in a dedicated module rather than embedded directly inside the entrypoint.

## Repository Layout

- [CMakeLists.txt](CMakeLists.txt) — top-level ESP-IDF project configuration
- [partitions.csv](partitions.csv) — OTA-capable partition table
- [main/CMakeLists.txt](main/CMakeLists.txt) — main component source registration
- [main/ble_ota.c](main/ble_ota.c) — BLE OTA service implementation
- [main/ble_ota.h](main/ble_ota.h) — OTA module public API
- [main/blink_example_main.c](main/blink_example_main.c) — LED runtime entrypoint
- [sdkconfig.defaults](sdkconfig.defaults) — default project configuration

## Hardware Notes

This project targets ESP-IDF-based development boards with a BLE-capable controller and a visible LED output. The OTA transport is implemented using a custom GATT service, so a compatible BLE host is needed to send OTA command/data packets to the device.

## Build

Use the standard ESP-IDF workflow:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

If you are working in a clean shell, make sure the ESP-IDF environment has been exported first.

## OTA Protocol Summary

A custom BLE service is exposed with the following logical layout:

- Command characteristic: start / commit / abort OTA transaction
- Data characteristic: receive firmware image chunks
- Status characteristic: report operation progress back to the host

## Notes

This repository is oriented toward a maintainable firmware structure, not a monolithic single-file entrypoint implementation.

The build output and local IDE cache should remain outside the Git working tree, which is why the repository ignores generated ESP-IDF artifacts and editor workspace metadata.
