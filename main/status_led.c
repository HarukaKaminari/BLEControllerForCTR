/*
 * status_led.c
 *
 * Hardware owner for the board's single addressable RGB LED. Other modules
 * request an indication through status_led_set() or status_led_set_color();
 * they never access led_strip directly. This keeps LED timing and peripheral
 * ownership in one place and prevents BLE callbacks from competing with an
 * application blink loop.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "status_led.h"

/* Log tag for LED initialization messages. */
static const char *TAG = "status_led";

/* Hardware and default visual parameters. */
#define STATUS_LED_GPIO CONFIG_BLINK_GPIO
#define STATUS_LED_DEFAULT_BLINK_HZ 5.0f

/* Shared LED driver handle and requested output state. */
static led_strip_handle_t s_strip;
static volatile uint32_t s_rgb;
static volatile float s_frequency_hz;
static bool s_initialized;

/*
 * Render one frame to the physical LED.
 *
 * Colors use 0x00RRGGBB. When on is false, all channels are written as zero,
 * which is the off value for an addressable LED. led_strip_refresh() is
 * required after changing the pixel buffer to transmit the frame.
 */
static void render(uint32_t rgb, bool on)
{
    led_strip_set_pixel(s_strip, 0,
                        on ? ((rgb >> 16) & 0xFF) : 0,
                        on ? ((rgb >> 8) & 0xFF) : 0,
                        on ? (rgb & 0xFF) : 0);
    led_strip_refresh(s_strip);
}

/*
 * Background renderer.
 *
 * A separate task is used so callers only update the desired state and return
 * immediately. For a positive frequency, 500/frequency gives the half-period
 * in milliseconds, because one complete cycle contains an on and an off half.
 * A non-positive frequency never toggles the output: zero means solid when a
 * nonzero color is supplied, and a negative value means explicitly off.
 */
static void status_led_task(void *arg)
{
    (void)arg;
    uint32_t rendered_rgb = 0;
    bool rendered_on = false;

    while (true) {
        uint32_t requested_rgb = s_rgb;
        float requested_frequency = s_frequency_hz;
        bool blinking = requested_frequency > 0.0f && requested_rgb != 0;
        bool next_on = blinking ? !rendered_on : requested_rgb != 0 &&
                                                   requested_frequency >= 0.0f;

        if (requested_rgb != rendered_rgb || next_on != rendered_on) {
            render(requested_rgb, next_on);
            rendered_rgb = requested_rgb;
            rendered_on = next_on;
        }

        if (blinking) {
            uint32_t half_period_ms = (uint32_t)(500.0f / requested_frequency);
            vTaskDelay(pdMS_TO_TICKS(half_period_ms > 0 ? half_period_ms : 1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/*
 * Convert a semantic OTA/application state into a generic color/frequency
 * request. Keeping this mapping here means BLE modules do not need to know the
 * RGB encoding or timing implementation.
 */
void status_led_set(status_led_state_t state)
{
    static const uint32_t colors[] = {
        [STATUS_LED_OFF] = 0x00000000,
        [STATUS_LED_BLUE] = 0x000000FF,
        [STATUS_LED_YELLOW] = 0x00FFFF00,
        [STATUS_LED_DATA] = 0x000000FF,
        [STATUS_LED_GREEN] = 0x0000FF00,
        [STATUS_LED_UPDATE] = 0x00FF0000,
    };

    if (state > STATUS_LED_UPDATE) {
        status_led_set_color(0, -1.0f);
        return;
    }

    status_led_set_color(colors[state],
                         state == STATUS_LED_DATA || state == STATUS_LED_UPDATE
                             ? STATUS_LED_DEFAULT_BLINK_HZ
                             : 0.0f);
}

/*
 * Set an arbitrary color and blink rate.
 *
 * Initialization is lazy because the public API may be called from either the
 * startup path or a Bluetooth callback. The first call creates the RMT-backed
 * LED strip and its renderer task; later calls only replace the volatile
 * request consumed by that task.
 */
void status_led_set_color(uint32_t rgb, float frequency_hz)
{
    if (!s_initialized) {
        led_strip_config_t strip_config = {
            .strip_gpio_num = STATUS_LED_GPIO,
            .max_leds = 1,
        };
        led_strip_rmt_config_t rmt_config = {
            .resolution_hz = 10 * 1000 * 1000,
            .flags.with_dma = false,
        };
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config,
                                                 &s_strip));
        s_initialized = true;
        ESP_LOGI(TAG, "RGB status LED initialized on GPIO %d", STATUS_LED_GPIO);
        xTaskCreate(status_led_task, "status_led", 2048, NULL, 5, NULL);
    }

    s_rgb = rgb;
    s_frequency_hz = frequency_hz;
}