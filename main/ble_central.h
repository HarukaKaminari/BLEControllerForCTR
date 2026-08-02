#ifndef BLE_CENTRAL_H
#define BLE_CENTRAL_H

/*
 * Public interface for the normal-mode BLE central role.
 *
 * The implementation scans for a standard HID service and attempts one GATT
 * connection. The function returns after scheduling the asynchronous scan;
 * connection results are handled internally by the module.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Start scanning for a standard BLE HID controller. */
void ble_central_init(void);

#ifdef __cplusplus
}
#endif

#endif