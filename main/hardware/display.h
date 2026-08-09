/*
 * display.h
 *
 * LCD, capacitive touch and LVGL initialization.
 */

#ifndef HARDWARE_DISPLAY_H
#define HARDWARE_DISPLAY_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the SPI bus, backlight and ST7789 panel. */
esp_err_t display_init(void);

/** Initialize the touch controller. MUST run on Core 1 — I2C conflicts with WiFi otherwise. */
esp_err_t touch_init_core1(void);

/** Initialize LVGL. Call after display_init() and touch_init_core1(). */
esp_err_t lvgl_init(void);

/** Set backlight brightness (0-100%) via PWM. */
esp_err_t display_set_brightness(uint8_t brightness_percent);

/** Initialize the backlight PWM channel. */
esp_err_t display_backlight_init(void);

/* CST820 hardware gesture codes (register 0x01), per the panel vendor's driver.
 * Only these two are latched — slide/single-tap codes duplicate what LVGL
 * already derives from the coordinate stream. */
#define CST820_GESTURE_NONE        0x00
#define CST820_GESTURE_DOUBLE_TAP  0x0B
#define CST820_GESTURE_LONG_PRESS  0x0C

/** Return and clear the last latched gesture. Safe from any task. */
uint8_t cst820_take_gesture(void);

/**
 * Inject a synthetic tap at landscape coordinates (0-319, 0-239) through the
 * normal touch pipeline, held for duration_ms then released naturally. The
 * duration is rounded up to whole indev polls and clamped to [2..400] polls.
 * Safe from any task.
 */
void display_inject_tap(uint16_t screen_x, uint16_t screen_y, uint16_t duration_ms);

/**
 * Inject a synthetic drag in landscape coordinates, interpolated across
 * duration_ms then released. LVGL only reads it as a gesture when the travel
 * exceeds ~60px in ~150-250ms; slower drags scroll instead. Safe from any task.
 */
void display_inject_swipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_DISPLAY_H
