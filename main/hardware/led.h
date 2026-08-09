/*
 * led.h
 *
 * RGB status LED — active-low PWM control.
 */

#ifndef HARDWARE_LED_H
#define HARDWARE_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the LED PWM channels. */
void led_init(void);

/** Set the LED color; each channel is 0-127. */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/** Start the WiFi boot blink (blue, 1s on/off). */
void led_start_wifi_boot_blink(void);

/** Stop the WiFi boot blink. */
void led_stop_wifi_boot_blink(void);

/** Blink blue once to indicate a CGM poll. */
void led_blink_blue(void);

/** Steady green for 3 seconds, then fade off. */
void led_show_success(void);

/** Start the error blink (red, 500ms on/off). */
void led_start_error_blink(void);

/** Stop the error blink. */
void led_stop_error_blink(void);

/** Start the alarm flash (bright red, fast). */
void led_start_alarm_flash(void);

/** Stop the alarm flash. */
void led_stop_alarm_flash(void);

/** Turn the LED off. */
void led_off(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_LED_H
