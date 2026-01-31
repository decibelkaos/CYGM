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
 * Set breathing color (for future use - breathing task disabled)
 * @param r Red brightness (0-127)
 * @param g Green brightness (0-127)
 * @param b Blue brightness (0-127)
 */
void led_set_breathing_color(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_LED_H
