# BLEControllerForCTR

This repository is a single-target ESP-IDF firmware project for the ESP32-S3 platform. The application keeps BLE OTA transport logic in a dedicated module instead of embedding it in the main entrypoint.

## Project structure

- [main/main.c](main/main.c) — lightweight application entrypoint and startup orchestration.
- [main/ble_ota.c](main/ble_ota.c) — BLE GATT OTA service implementation, protocol handling, and OTA commit path.
- [main/ble_ota.h](main/ble_ota.h) — public OTA API exposed to the entrypoint.
- [main/ble_central.c](main/ble_central.c) — normal-mode BLE HID controller scan and connection logic.
- [main/ble_central.h](main/ble_central.h) — public central-mode startup API.
- [main/status_led.c](main/status_led.c) — independent RGB status indicator implementation.
- [main/status_led.h](main/status_led.h) — single public status-setting interface.
- [partitions.csv](partitions.csv) — two-slot OTA partition layout used by the firmware update flow.
- [sdkconfig.defaults](sdkconfig.defaults) — base project defaults.
- [sdkconfig.defaults.esp32s3](sdkconfig.defaults.esp32s3) — ESP32-S3-specific board defaults.

## Scope

This repository is intentionally limited to the ESP32-S3 target only. It is not designed to be a cross-platform firmware tree or a generic multi-chip demo.

## Build

Use the standard ESP-IDF workflow for this board target:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## OTA protocol summary

The firmware exposes a custom BLE service with:

- a command characteristic for `START`, `COMMIT`, and `ABORT`
- a data characteristic for incoming firmware chunks
- a status characteristic for transaction feedback

## RGB status indication

The addressable RGB LED is driven on `CONFIG_BLINK_GPIO` (GPIO 38 by default)
and is exclusively owned by the status LED module:

- blue, solid: powered on and waiting;
- yellow, solid: an Android BLE client is connected;
- blue, fast blinking: firmware data is being received;
- green, solid: the complete image has been received;
- red, fast blinking: firmware update is being finalized;
- blue, solid: update completed successfully, immediately before reboot.

The module exposes `status_led_set()` for predefined states and
`status_led_set_color()` for custom RGB values. Custom colors use the
`0x00RRGGBB` format; `0xFFFFFFFF` is also interpreted as white, while `0` is
off. A frequency of `0` means solid, a positive value specifies complete
blink cycles per second, and a negative value turns the LED off. The module
initializes the LED lazily and performs blinking internally.

## Normal-mode controller connection

When the OTA trigger is not held during startup, the firmware starts a BLE
central role and scans for devices advertising the standard HID Service UUID
`0x1812`. During scanning and connection establishment, the LED slowly blinks
blue at `0.5 Hz`. After a successful GATT connection, it remains solid green.

The total scan/connection deadline is 30 seconds. If no controller connection
is established before the deadline, the firmware turns the LED off, shuts down
the BLE stack, and enters deep sleep. No wake source is configured, so the
device remains asleep until the next reset or power cycle. OTA mode remains
available by holding the configured OTA trigger GPIO during startup.

The current implementation establishes the BLE GATT connection but does not
yet decode a controller-specific HID report protocol. HID report discovery and
decoding depend on the exact controller model and its report descriptor.

## Notes

- The OTA logic is intentionally isolated in a separate source module for maintainability.
- Generated build output and editor-local metadata are kept outside the committed source tree.
