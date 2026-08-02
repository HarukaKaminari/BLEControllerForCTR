/* Blink Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ble_ota.h"
#include "ota_trigger.h"
#include "status_led.h"

/*
 * This file is intentionally kept lean.
 *
 * Its responsibility is not to host OTA protocol details, but to serve as the
 * application's entrypoint and demonstration runtime.
 *
 * The BLE OTA functionality has been moved to the separate module in
 * ble_ota.c/ble_ota.h so that the entrypoint remains focused on:
 *   - basic hardware setup;
 *   - LED control;
 *   - startup of the OTA subsystem.
 */
/*
 * app_main()
 *
 * This is the user-application entry point.
 *
 * The ordering of startup work here is important:
 *   1. initialize NVS;
 *   2. sample the OTA trigger GPIO;
 *   3. start BLE OTA only when the trigger is held low for two seconds;
 *   4. keep the blue power/status indication active when OTA mode was not
 *      requested. Normal business logic can be added independently here.
 */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    status_led_set(STATUS_LED_BLUE);

    if (ota_trigger_is_held()) {
        ble_ota_init();

        /* BLE OTA owns startup until the client finishes the update. */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
