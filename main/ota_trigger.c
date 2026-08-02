#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "ota_trigger.h"

static const char *TAG = "ota_trigger";

#define OTA_TRIGGER_GPIO CONFIG_OTA_TRIGGER_GPIO
#define OTA_TRIGGER_HOLD_MS 2000
#define OTA_TRIGGER_SAMPLE_MS 10

bool ota_trigger_is_held(void)
{
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

    const TickType_t sample_period = pdMS_TO_TICKS(OTA_TRIGGER_SAMPLE_MS);
    const int sample_count = OTA_TRIGGER_HOLD_MS / OTA_TRIGGER_SAMPLE_MS;
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