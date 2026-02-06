/*
 * display.h
 *
 * Display and input initialization module
 * LCD, touch, and LVGL setup
 */

#ifndef HARDWARE_DISPLAY_H
#define HARDWARE_DISPLAY_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize LCD display (SPI, backlight, ST7789 panel)
 * @return ESP_OK on success
 */
esp_err_t display_init(void);

/**
 * Initialize touch controller on Core 1
 * CRITICAL: Must run on Core 1 to avoid I2C conflicts with WiFi
 * @return ESP_OK on success
 */
esp_err_t touch_init_core1(void);

/**
 * Initialize LVGL framework
 * Must be called after display_init() and touch_init_core1()
 * @return ESP_OK on success
 */
esp_err_t lvgl_init(void);

/**
 * Set LCD backlight brightness using PWM
 * @param brightness_percent Brightness level (0-100%)
 * @return ESP_OK on success
 */
esp_err_t display_set_brightness(uint8_t brightness_percent);

/**
 * Initialize LCD backlight PWM channel
 * @return ESP_OK on success
 */
esp_err_t display_backlight_init(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_DISPLAY_H
