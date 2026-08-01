#ifndef BLE_OTA_H
#define BLE_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ble_ota.h
 *
 * This header declares the public interface of the BLE OTA module.
 *
 * The idea is to keep the OTA-related networking and firmware-update logic
 * completely separate from the normal application logic. In other words:
 *
 * - the application entry file is only responsible for the product's basic
 *   runtime behavior (for example, blinking an LED);
 * - the BLE OTA module is responsible for all operations that relate to
 *   wireless OTA firmware transport and partition switching.
 *
 * The BLE OTA service exposes a tiny command/data protocol over GATT:
 *   - command writes arrive on the command characteristic;
 *   - firmware chunks are written to the data characteristic;
 *   - the device reports status updates back through the status notify
 *     characteristic.
 */

/*
 * Initialize the BLE controller, bluedroid stack, GAP/GATT callbacks,
 * advertising, and the OTA service definition.
 *
 * This function should be called once during startup before the application
 * enters its main loop.
 */
void ble_ota_init(void);

/*
 * Release the OTA context and stop advertising.
 *
 * This is a lightweight cleanup hook. In a production product this may be
 * expanded to unregister services or deactivate resources cleanly.
 */
void ble_ota_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
