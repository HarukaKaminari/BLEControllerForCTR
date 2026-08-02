/*
 * ota_trigger.c
 *
 * Startup-only hardware gate for BLE OTA mode. The input is active-low and is
 * sampled continuously rather than read only once, preventing a short glitch
 * or accidental button press from selecting OTA mode.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "ota_trigger.h"

/* Log tag and timing policy for the startup decision. */
static const char *TAG = "ota_trigger";

#define OTA_TRIGGER_GPIO CONFIG_OTA_TRIGGER_GPIO
#define OTA_TRIGGER_HOLD_MS 2000
#define OTA_TRIGGER_SAMPLE_MS 10

bool ota_trigger_is_held(void)
{
    /* Configure the trigger as a pulled-up input. An external switch or host
     * signal selects OTA by pulling this pin to ground. */
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << OTA_TRIGGER_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(TAG, "Checking GPIO %d for a %d ms active-low hold",
             OTA_TRIGGER_GPIO, OTA_TRIGGER_HOLD_MS);

    /* Convert the human-readable debounce/hold policy to FreeRTOS ticks. */
    const TickType_t sample_period = pdMS_TO_TICKS(OTA_TRIGGER_SAMPLE_MS);
    const int sample_count = OTA_TRIGGER_HOLD_MS / OTA_TRIGGER_SAMPLE_MS;
    /* Every sample must remain low. One high sample cancels the request. */
    for (int sample = 0; sample < sample_count; ++sample) {
        if (gpio_get_level(OTA_TRIGGER_GPIO) != 0) {
            ESP_LOGI(TAG, "OTA trigger not held; starting normal application");
            return false;
        }
        vTaskDelay(sample_period);
    }

    ESP_LOGI(TAG, "OTA trigger held; starting BLE OTA mode");
    return true;
}