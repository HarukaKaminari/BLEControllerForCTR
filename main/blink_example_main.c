/* Blink Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "ble_ota.h"

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
static const char *TAG = "example";
#define BLINK_GPIO CONFIG_BLINK_GPIO
static uint8_t s_led_state = 0;

#ifdef CONFIG_BLINK_LED_STRIP
static led_strip_handle_t led_strip;

/*
 * blink_led()
 *
 * Apply the current LED state to the hardware.
 *
 * When the LED is on, the RGB value is set to a neutral white level.
 * When the LED is off, all pixel channels are cleared.
 */
static void blink_led(void)
{
    if (s_led_state) {
        led_strip_set_pixel(led_strip, 0, 16, 16, 16);
        led_strip_refresh(led_strip);
    } else {
        led_strip_clear(led_strip);
    }
}

/*
 * configure_led()
 *
 * Initialize the selected LED backend.
 *
 * The project can be configured to use either:
 *   - a normal GPIO LED; or
 *   - a single-addressable LED strip.
 *
 * This helper ensures that the correct peripheral is opened and that the LED
 * starts from a known off-state.
 */
static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED!");
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
#else
#error "unsupported LED strip backend"
#endif
    led_strip_clear(led_strip);
}

#elif CONFIG_BLINK_LED_GPIO

static void blink_led(void)
{
    gpio_set_level(BLINK_GPIO, s_led_state);
}

static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to blink GPIO LED!");
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
}

#else
#error "unsupported LED type"
#endif

/*
 * app_main()
 *
 * This is the user-application entry point.
 *
 * The ordering of startup work here is important:
 *   1. initialize NVS;
 *   2. spin up the BLE OTA service module;
 *   3. configure the LED peripheral;
 *   4. enter the normal blink loop.
 */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ble_ota_init();
    configure_led();

    while (1) {
        ESP_LOGI(TAG, "Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
        blink_led();
        s_led_state = !s_led_state;
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}
