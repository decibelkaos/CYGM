/*
 * led.h
 *
 * RGB LED control module
 * Active-low PWM control for RGB LED
 */

#ifndef HARDWARE_LED_H
#define HARDWARE_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize LED PWM channels
 */
void led_init(void);

/**
 * Set LED color
 * @param r Red brightness (0-127)
 * @param g Green brightness (0-127)
 * @param b Blue brightness (0-127)
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * Start WiFi boot blink pattern (blue, 1s on/off)
 */
void led_start_wifi_boot_blink(void);

/**
 * Stop WiFi boot blink pattern
 */
void led_stop_wifi_boot_blink(void);

/**
 * Blink blue once (for Dexcom polling indication)
 */
void led_blink_blue(void);

/**
 * Show success (steady green for 3 seconds, then fade off)
 */
void led_show_success(void);

/**
 * Start error blink pattern (red, 500ms on/off)
 */
void led_start_error_blink(void);

/**
 * Stop error blink pattern
 */
void led_stop_error_blink(void);

/**
 * Start alarm flash pattern (bright red, fast)
 */
void led_start_alarm_flash(void);

/**
 * Stop alarm flash pattern
 */
void led_stop_alarm_flash(void);

/**
 * Turn off LED
 */
void led_off(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_LED_H
