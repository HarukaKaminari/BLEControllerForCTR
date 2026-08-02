#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "ble_ota.h"
#include "status_led.h"

/*
 * ble_ota.c
 *
 * This file implements a minimal BLE-based OTA service for ESP-IDF.
 *
 * Design intent:
 *   1. Separate transport (BLE GATT) and firmware update logic (OTA API)
 *      into a distinct module.
 *   2. Keep the OTA protocol lightweight so it can be reused by a mobile
 *      or PC-side host later.
 *   3. Maintain a clear state machine:
 *        idle -> upload-started -> data-streaming -> commit -> reboot
 *
 * Protocol summary:
 *   - A custom BLE service with UUID 0x00FF is exposed by the device.
 *   - Characteristic 0xFF01 is a write-only command channel.
 *   - Characteristic 0xFF02 is a write-only data channel.
 *   - Characteristic 0xFF03 is a read/notify status channel.
 *
 * The command channel expects a 5-byte packet:
 *   byte 0: command code (see OTA_CMD_* macros)
 *   bytes 1..4: image length in big-endian format
 *
 * After a START command, the device begins writing incoming firmware chunks
 * into the alternative OTA partition. After all bytes are received, the
 * COMMIT command finalizes the update and switches the boot partition.
 */

static const char *TAG = "ble_ota";

/*
 * APP_ID is the application identifier that the GATT server registers.
 * All BLE service registrations in this module share the same app id.
 */
#define APP_ID 0

/*
 * UUID definitions for the custom OTA service.
 *
 * On ESP-IDF, BLE service and characteristic UUIDs are usually 16-bit values
 * here because this example is intentionally minimal and self-contained.
 */
#define OTA_SERVICE_UUID 0x00FF
#define OTA_CMD_CHAR_UUID 0xFF01
#define OTA_DATA_CHAR_UUID 0xFF02
#define OTA_STATUS_CHAR_UUID 0xFF03

/*
 * Command codes used on the command characteristic.
 */
#define OTA_CMD_START 0x01
#define OTA_CMD_COMMIT 0x02
#define OTA_CMD_ABORT 0x03

/*
 * GATT handles for the custom service and its characteristics.
 *
 * These values are populated by the GATT event callbacks after service and
 * characteristic creation completes. The module stores them so that on each
 * incoming write event it knows which characteristic the client targeted.
 */
static uint16_t s_service_handle = 0;
static uint16_t s_cmd_char_handle = 0;
static uint16_t s_data_char_handle = 0;
static uint16_t s_status_char_handle = 0;

/*
 * Current BLE GATT interface instance used for notifications or indications.
 * We keep a global handle because status notifications are sent from the OTA
 * state machine and from the callback layer, not from the main application.
 */
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;

/*
 * OTA state variables.
 *
 * These form the core state of the firmware upload procedure.
 * - s_ota_active indicates whether a valid OTA transaction is currently open;
 * - s_ota_handle is the OTA operation handle returned by esp_ota_begin();
 * - s_target_partition is the partition that will receive the new image;
 * - s_image_size is the expected length of the firmware image;
 * - s_image_bytes_received tracks how many bytes have already been written.
 */
static bool s_ota_active = false;
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_target_partition = NULL;
static size_t s_image_size = 0;
static size_t s_image_bytes_received = 0;

/*
 * s_ble_ota_initialized
 *
 * Marks whether the BLE OTA module has completed its initialization path.
 *
 * This flag is intentionally separate from the OTA upload state. It exists so
 * that ble_ota_deinit() can safely decide whether the full BLE stack was ever
 * successfully started, which prevents calling teardown APIs in an invalid
 * half-initialized state.
 */
static bool s_ble_ota_initialized = false;

/*
 * Connected peer address that is remembered so the deinitialization path can
 * actively tear down an existing BLE link before the stack is disabled.
 *
 * The Bluetooth controller and Bluedroid stack can only be shut down cleanly
 * after the active connection is disconnected (or at least no longer active).
 */
static esp_bd_addr_t s_connected_bda = {0};

/*
 * notify_status()
 *
 * A simple helper that sends back a textual status string to the connected
 * client through the status characteristic.
 *
 * Note:
 *   This uses an indication (not a notification) because the code currently
 *   requests an acknowledge-style response for status updates.
 */
static void notify_status(const char *message)
{
    if (s_status_char_handle == 0 || s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }

    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_status_char_handle,
                                strlen(message), (uint8_t *)message,
                                false);
}

/*
 * reset_ota_ctx()
 *
 * Clear the OTA state and release the current OTA operation if one exists.
 *
 * This helper is intentionally conservative:
 * - if an OTA transaction is currently open, abort it;
 * - then clear all counters and handles so the next OTA session starts clean.
 */
static void reset_ota_ctx(void)
{
    if (s_ota_active) {
        esp_ota_abort(s_ota_handle);
    }

    s_ota_active = false;
    s_ota_handle = 0;
    s_target_partition = NULL;
    s_image_size = 0;
    s_image_bytes_received = 0;
}

/*
 * start_ota_upload()
 *
 * Begin an OTA session by selecting the alternate application partition.
 *
 * The ESP-IDF OTA API requires a target partition and an expected image size.
 * This function inspects the currently running partition then chooses the
 * opposite slot (OTA_0 or OTA_1). It then calls esp_ota_begin() to switch the
 * update state machine into a writable mode.
 */
static void start_ota_upload(size_t image_size)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        notify_status("STA:ERR run part");
        return;
    }

    const esp_partition_t *target = NULL;
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                           ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                           NULL);
    } else {
        target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                           ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                           NULL);
    }

    if (target == NULL) {
        notify_status("STA:ERR NO OTA");
        return;
    }

    reset_ota_ctx();
    esp_err_t err = esp_ota_begin(target, image_size, &s_ota_handle);
    if (err != ESP_OK) {
        notify_status("STA:ERR BEGIN");
        return;
    }

    s_ota_active = true;
    s_target_partition = target;
    s_image_size = image_size;
    s_image_bytes_received = 0;

    char status[64];
    snprintf(status, sizeof(status), "STA:OK %u", (unsigned int)image_size);
    notify_status(status);
}

/*
 * process_command()
 *
 * Decode and execute a command packet sent by the host over the command
 * characteristic.
 *
 * The first byte is the command code. The next four bytes are the expected
 * firmware image size in big-endian order. The command handler then routes the
 * request to either:
 *   - START: open OTA begin session and prepare the target partition;
 *   - COMMIT: verify that all bytes were received, finalize the image, set the
 *     boot partition, then restart;
 *   - ABORT: reset the OTA context.
 */
static void process_command(const uint8_t *data, uint16_t length)
{
    if (length < 5) {
        notify_status("CMD:ERR LEN");
        return;
    }

    uint8_t cmd = data[0];
    uint32_t image_size = ((uint32_t)data[1] << 24) |
                          ((uint32_t)data[2] << 16) |
                          ((uint32_t)data[3] << 8) |
                          (uint32_t)data[4];

    switch (cmd) {
    case OTA_CMD_START:
        start_ota_upload(image_size);
        break;
    case OTA_CMD_COMMIT:
        if (!s_ota_active) {
            notify_status("COM:ERR NO OTA");
            return;
        }
        if (s_image_bytes_received != s_image_size) {
            notify_status("COM:ERR SIZE");
            reset_ota_ctx();
            return;
        }
        status_led_set(STATUS_LED_UPDATE);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (esp_ota_end(s_ota_handle) != ESP_OK) {
            status_led_set(STATUS_LED_YELLOW);
            notify_status("COM:ERR END");
            reset_ota_ctx();
            return;
        }
        if (esp_ota_set_boot_partition(s_target_partition) != ESP_OK) {
            status_led_set(STATUS_LED_YELLOW);
            notify_status("COM:ERR BOOT");
            reset_ota_ctx();
            return;
        }
        notify_status("COM:OK REBOOT");
        status_led_set(STATUS_LED_BLUE);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        break;
    case OTA_CMD_ABORT:
        reset_ota_ctx();
        notify_status("ABT:OK");
        break;
    default:
        notify_status("CMD:ERR UNSUP");
        break;
    }
}

/*
 * process_data()
 *
 * Write a chunk of raw firmware bytes into the OTA handle.
 *
 * This function is the low-level data-path of the OTA protocol. The caller
 * may send many small BLE packets; the device simply writes each chunk into
 * the OTA partition in order. The code prevents overflow by checking the
 * running byte count against the expected total image size.
 */
static void process_data(const uint8_t *data, uint16_t length)
{
    if (!s_ota_active) {
        notify_status("DAT:ERR OFF");
        return;
    }

    if ((s_image_bytes_received + length) > s_image_size) {
        notify_status("DAT:ERR OVF");
        reset_ota_ctx();
        return;
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, length);
    if (err != ESP_OK) {
        notify_status("DAT:ERR WR");
        reset_ota_ctx();
        return;
    }

    s_image_bytes_received += length;
    if (s_image_bytes_received == s_image_size) {
        status_led_set(STATUS_LED_GREEN);
    } else {
        status_led_set(STATUS_LED_DATA);
    }
    char status[48];
    snprintf(status, sizeof(status), "DAT:OK %u", (unsigned int)s_image_bytes_received);
    notify_status(status);
}

/*
 * gap_event_handler()
 *
 * GAP layer callback for BLE advertising events.
 *
 * This callback is intentionally small: after the advertising data is ready,
 * the code starts advertising and then logs any failure in the start-complete
 * event.
 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
            .adv_int_min = 0x20,
            .adv_int_max = 0x40,
            .adv_type = ADV_TYPE_IND,
            .channel_map = ADV_CHNL_ALL,
            .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .peer_addr = {0},
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        });
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    default:
        break;
    }
}

/*
 * gatts_event_handler()
 *
 * GATT server callback that handles the OTA service lifecycle.
 *
 * This callback is the core glue between the BLE stack and the OTA logic:
 *   - REG_EVT: register the application and create the OTA service;
 *   - CREATE_EVT: add the command/data/status characteristics;
 *   - ADD_CHAR_EVT: save the attribute handles needed for later writes;
 *   - WRITE_EVT: route writes to the command or data parsing functions.
 */
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        {
            esp_gatt_srvc_id_t service_id = {
                .id = {
                    .uuid = {
                        .len = ESP_UUID_LEN_16,
                        .uuid.uuid16 = OTA_SERVICE_UUID,
                    },
                    .inst_id = 0,
                },
                .is_primary = true,
            };
            esp_ble_gatts_create_service(gatts_if, &service_id, 7);
        }
        break;
    case ESP_GATTS_CREATE_EVT:
        s_service_handle = param->create.service_handle;
        {
            esp_bt_uuid_t char_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid.uuid16 = OTA_CMD_CHAR_UUID,
            };
            esp_ble_gatts_add_char(s_service_handle, &char_uuid,
                                   ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   NULL, NULL);

            char_uuid.uuid.uuid16 = OTA_DATA_CHAR_UUID;
            esp_ble_gatts_add_char(s_service_handle, &char_uuid,
                                   ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   NULL, NULL);

            char_uuid.uuid.uuid16 = OTA_STATUS_CHAR_UUID;
            esp_ble_gatts_add_char(s_service_handle, &char_uuid,
                                   ESP_GATT_PERM_READ,
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                   NULL, NULL);
        }
        esp_ble_gatts_start_service(s_service_handle);
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == OTA_CMD_CHAR_UUID) {
            s_cmd_char_handle = param->add_char.attr_handle;
        } else if (param->add_char.char_uuid.uuid.uuid16 == OTA_DATA_CHAR_UUID) {
            s_data_char_handle = param->add_char.attr_handle;
        } else if (param->add_char.char_uuid.uuid.uuid16 == OTA_STATUS_CHAR_UUID) {
            s_status_char_handle = param->add_char.attr_handle;
        }
        break;
    case ESP_GATTS_CONNECT_EVT:
        status_led_set(STATUS_LED_YELLOW);
        s_conn_id = param->connect.conn_id;
        memcpy(s_connected_bda, param->connect.remote_bda, sizeof(s_connected_bda));
        ESP_LOGI(TAG, "BLE connected");
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        status_led_set(STATUS_LED_BLUE);
        s_conn_id = 0;
        memset(s_connected_bda, 0, sizeof(s_connected_bda));
        ESP_LOGI(TAG, "BLE disconnected");
        break;
    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_cmd_char_handle) {
            process_command(param->write.value, param->write.len);
        } else if (param->write.handle == s_data_char_handle) {
            process_data(param->write.value, param->write.len);
        }
        break;
    default:
        break;
    }
}

/*
 * ble_ota_init()
 *
 * High-level initialization entry point for the BLE OTA module.
 *
 * The sequence is:
 *   1. initialize the Bluetooth controller;
 *   2. enable BLE mode;
 *   3. initialize and enable the Bluedroid stack;
 *   4. register GAP and GATT callbacks;
 *   5. register the OTA GATT application;
 *   6. publish a device name and advertising payload containing the OTA UUID.
 *
 * After this call returns, the device is ready to be discovered by a BLE
 * client and can accept the OTA command/data transaction.
 */
void ble_ota_init(void)
{
    if (s_ble_ota_initialized) {
        return;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(APP_ID));
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name("ESP32-BLINK-OTA"));

    uint8_t adv_service_uuid[] = {
        (uint8_t)(OTA_SERVICE_UUID & 0xFF),
        (uint8_t)(OTA_SERVICE_UUID >> 8),
    };

    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&(esp_ble_adv_data_t){
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = false,
        .min_interval = 0x20,
        .max_interval = 0x40,
        .appearance = 0x00,
        .flag = 0x06,
        .service_uuid_len = sizeof(adv_service_uuid),
        .p_service_uuid = adv_service_uuid,
    }));

    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));
    s_ble_ota_initialized = true;
}

/*
 * ble_ota_deinit()
 *
 * Reverse-close the BLE OTA subsystem.
 *
 * This function is intended to restore the local BLE device and all stack-level
 * resources to the state they were in before ble_ota_init() was called.
 *
 * Cleanup sequence:
 *   1. abort any ongoing OTA write transaction;
 *   2. disconnect any currently connected peer, if one exists;
 *   3. stop and delete the OTA GATT service;
 *   4. unregister the OTA GATT application;
 *   5. stop advertising and clear the local GATT interface state;
 *   6. disable and deinitialize the Bluedroid stack;
 *   7. disable and deinitialize the Bluetooth controller.
 *
 * The final order matters because the stack tear-down should happen from the
 * highest-level application registration down to the low-level controller.
 */
void ble_ota_deinit(void)
{
    if (!s_ble_ota_initialized) {
        return;
    }

    reset_ota_ctx();

    if (s_connected_bda[0] != 0 || s_connected_bda[1] != 0 ||
        s_connected_bda[2] != 0 || s_connected_bda[3] != 0 ||
        s_connected_bda[4] != 0 || s_connected_bda[5] != 0) {
        esp_ble_gap_disconnect(s_connected_bda);
        memset(s_connected_bda, 0, sizeof(s_connected_bda));
    }

    if (s_service_handle != 0) {
        esp_ble_gatts_stop_service(s_service_handle);
        esp_ble_gatts_delete_service(s_service_handle);
        s_service_handle = 0;
    }

    if (s_gatts_if != ESP_GATT_IF_NONE) {
        esp_ble_gatts_app_unregister(s_gatts_if);
        s_gatts_if = ESP_GATT_IF_NONE;
    }

    esp_ble_gap_stop_advertising();
    s_cmd_char_handle = 0;
    s_data_char_handle = 0;
    s_status_char_handle = 0;
    s_conn_id = 0;

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_ble_ota_initialized = false;
}
