#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_BLUE,
    STATUS_LED_YELLOW,
    STATUS_LED_DATA,
    STATUS_LED_GREEN,
    STATUS_LED_UPDATE,
} status_led_state_t;

/*
 * Set a predefined status indication. This compatibility helper is convenient
 * for the firmware's BLE OTA state machine; the generic color API below can be
 * used when arbitrary colors or blink frequencies are required.
 */
void status_led_set(status_led_state_t state);

/*
 * Set an arbitrary RGB color and blink frequency.
 *
 * The color is encoded as 0x00RRGGBB. The upper byte is ignored, therefore
 * 0xFFFFFFFF also represents white. A frequency of zero means continuously
 * on. A negative frequency means off, regardless of the color value. A
 * positive frequency is the number of complete on/off cycles per second.
 * Passing color 0 also turns the LED off.
 */
void status_led_set_color(uint32_t rgb, float frequency_hz);

#ifdef __cplusplus
}
#endif

#endif