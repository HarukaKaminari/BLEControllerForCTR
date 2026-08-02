/*
 * ble_central.c
 *
 * Normal-operation BLE central role. The module scans for standard BLE HID
 * devices, connects to the first matching advertisement, and reports the
 * connection result through the shared status LED module.
 *
 * Important scope boundary:
 *   - this module owns central-mode Bluetooth startup and shutdown;
 *   - it currently establishes the GATT link only;
 *   - HID report discovery and controller-specific input decoding are left for
 *     the product layer because different controllers expose different report
 *     descriptors.
 *
 * OTA mode does not use this module. The startup trigger selects exactly one
 * Bluetooth role, so the central and OTA implementations do not run together.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_sleep.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "status_led.h"
#include "ble_central.h"

/* Log tag used to identify central-role messages on the serial console. */
static const char *TAG = "ble_central";

/*
 * GATT application identifier. OTA uses a different application id (0), so
 * keeping this value at 1 avoids accidental registration collisions if both
 * roles are ever used in the same firmware image.
 */
#define CENTRAL_APP_ID 1

/* Application-level deadline for scanning plus connection establishment. */
#define CENTRAL_TIMEOUT_MS 30000

/* Standard Bluetooth SIG Human Interface Device service UUID. */
#define HID_SERVICE_UUID 0x1812

/* Event-group bits used to communicate callback results to central_task(). */
#define CENTRAL_CONNECTED BIT0
#define CENTRAL_STOP BIT1

/*
 * Module state. These variables are accessed by the Bluetooth callbacks and
 * by central_task(), therefore they represent asynchronous role state rather
 * than ordinary local function data.
 */
static EventGroupHandle_t s_events;
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static esp_bd_addr_t s_peer_addr;
static esp_ble_addr_type_t s_peer_addr_type;
static bool s_stack_started;
static bool s_scan_started;

/*
 * Check whether a scan advertisement contains the standard HID service UUID.
 *
 * BLE advertising data is a sequence of length/type/value fields. ESP-IDF's
 * resolver extracts the 16-bit service-list field for us. UUIDs in BLE
 * advertising packets are little-endian, so the two bytes are reconstructed
 * as (high_byte << 8) | low_byte.
 *
 * Both complete and incomplete 16-bit service lists are accepted. A device
 * may advertise only part of its service list when the packet has limited
 * space, so requiring only the complete-list AD type would reject valid HID
 * controllers.
 */
static bool has_hid_service(const uint8_t *adv_data, uint8_t adv_len)
{
    uint8_t service_len = 0;
    uint8_t *services = esp_ble_resolve_adv_data((uint8_t *)adv_data,
                                                  ESP_BLE_AD_TYPE_16SRV_CMPL,
                                                  &service_len);
    if (services == NULL) {
        services = esp_ble_resolve_adv_data((uint8_t *)adv_data,
                                             ESP_BLE_AD_TYPE_16SRV_PART,
                                             &service_len);
    }
    if (services == NULL || service_len < 2) {
        return false;
    }

    for (uint8_t i = 0; i + 1 < service_len; i += 2) {
        if (((uint16_t)services[i + 1] << 8 | services[i]) == HID_SERVICE_UUID) {
            return true;
        }
    }
    /* The resolver determines the field length; adv_len is kept for API
     * readability and future validation of the complete advertisement. */
    (void)adv_len;
    return false;
}

/*
 * GAP callback for the central role.
 *
 * A scan result callback can arrive for both an individual result and the
 * scan-complete notification. Only individual inquiry results are candidates
 * for connection. Once a HID advertisement is found, scanning is stopped
 * before opening the GATT connection because the controller must transition
 * from discovery to connection without continuing to consume scan resources.
 */
static void gap_callback(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        return;
    }

    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT ||
        !has_hid_service(param->scan_rst.ble_adv, param->scan_rst.adv_data_len) ||
        s_scan_started == false) {
        return;
    }

    /* Preserve both the address bytes and the address type reported by the
     * advertiser. The type is required by esp_ble_gattc_open(). */
    memcpy(s_peer_addr, param->scan_rst.bda, sizeof(s_peer_addr));
    s_peer_addr_type = param->scan_rst.ble_addr_type;
    s_scan_started = false;
    esp_ble_gap_stop_scanning();
    ESP_LOGI(TAG, "BLE HID controller found; connecting");
    ESP_ERROR_CHECK(esp_ble_gattc_open(s_gattc_if, s_peer_addr,
                                       s_peer_addr_type, true));
}

/*
 * GATT client callback.
 *
 * Registration is the first asynchronous milestone. Only after receiving
 * ESP_GATTC_REG_EVT can the module safely configure scanning and use the
 * returned GATT client interface. The physical connection event is treated
 * as the successful connection point for the current requirement.
 */
static void gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATT client registration failed: %d", param->reg.status);
            xEventGroupSetBits(s_events, CENTRAL_STOP);
            return;
        }
        s_gattc_if = gattc_if;
        /* Active scanning requests scan responses as well as advertisements,
         * which is useful when a controller places its service data there. */
        ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&(esp_ble_scan_params_t){
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x50,
            .scan_window = 0x30,
            .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        }));
        break;
    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "BLE HID controller connected");
        status_led_set_color(0x0000FF00, 0.0f);
        xEventGroupSetBits(s_events, CENTRAL_CONNECTED);
        break;
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "BLE HID controller connection failed: %d",
                     param->open.status);
        }
        break;
    default:
        break;
    }
}

/*
 * Stop the central role in dependency order and enter indefinite deep sleep.
 *
 * Bluetooth resources are released before deep sleep is requested. This is
 * important because esp_deep_sleep_start() powers down the application but is
 * not a substitute for orderly shutdown of an active host/controller stack.
 * No wake source is configured here; reset or power cycling is required to
 * execute app_main() again.
 */
static void stop_bluetooth_and_sleep(void)
{
    if (s_scan_started) {
        esp_ble_gap_stop_scanning();
        s_scan_started = false;
    }
    if (s_gattc_if != ESP_GATT_IF_NONE) {
        /* conn_id is zero in the current single-connection design. */
        esp_ble_gattc_close(s_gattc_if, 0);
        esp_ble_gattc_app_unregister(s_gattc_if);
        s_gattc_if = ESP_GATT_IF_NONE;
    }
    if (s_stack_started) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_stack_started = false;
    }
    status_led_set_color(0, -1.0f);
    ESP_LOGI(TAG, "No BLE HID controller connected in 30 seconds; sleeping");
    esp_deep_sleep_start();
}

/*
 * Supervisory task for the application-level 30-second deadline.
 *
 * Bluetooth callbacks must remain short and should not perform a complete
 * stack teardown directly. This task waits for the connected bit; if it is
 * not set before the deadline, it performs the teardown from task context.
 */
static void central_task(void *arg)
{
    (void)arg;
    EventBits_t bits = xEventGroupWaitBits(s_events, CENTRAL_CONNECTED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CENTRAL_TIMEOUT_MS));
    if ((bits & CENTRAL_CONNECTED) == 0) {
        stop_bluetooth_and_sleep();
    }
    vTaskDelete(NULL);
}

/*
 * Initialize the BLE central role and start the controller search.
 *
 * Initialization is intentionally asynchronous: this function starts the
 * Bluetooth stack, registers callbacks, requests scan parameters, sets the
 * slow-blue LED indication, and returns. The callback/task pair handles all
 * later events while the application entrypoint remains simple.
 */
void ble_central_init(void)
{
    /* Event bits are created before registering callbacks because registration
     * may immediately generate an event on a fast target. */
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_bt_controller_init(&(esp_bt_controller_config_t)
                                           BT_CONTROLLER_INIT_CONFIG_DEFAULT()));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    s_stack_started = true;
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_callback));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(CENTRAL_APP_ID));
    status_led_set_color(0x000000FF, 0.5f);
    /* The API duration is in seconds; the task independently enforces the
     * same deadline in milliseconds so connection setup is covered too. */
    s_scan_started = true;
    ESP_ERROR_CHECK(esp_ble_gap_start_scanning(CENTRAL_TIMEOUT_MS / 1000));
    xTaskCreate(central_task, "ble_central", 4096, NULL, 5, NULL);
}