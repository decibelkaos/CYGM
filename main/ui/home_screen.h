/*
 * home_screen.h
 *
 * Main home screen with time, weather, and glucose display
 */

#ifndef UI_HOME_SCREEN_H
#define UI_HOME_SCREEN_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create home screen with time, date, weather, and glucose widgets
 */
void create_home_screen(void);

/**
 * Draw custom battery icon with specified color and fill level
 * @param canvas LVGL canvas object
 * @param color Battery color (green, orange, red)
 * @param percent Fill level 0-100
 */
void draw_battery_icon(lv_obj_t *canvas, uint32_t color, int percent);

// No-data / sensor change overlay
void show_nodata_overlay(void);
void dismiss_nodata_overlay(void);
void reset_nodata_overlay_cycle(void);

// WiFi disconnected overlay
void show_wifi_disconnected_overlay(void);
void dismiss_wifi_disconnected_overlay(void);

// Login success overlay (one-time after CGM login)
void show_login_success_overlay_ui(void);

// Legal disclaimer overlay (shown on boot until user accepts)
void show_disclaimer_overlay(void);

// Welcome overlay (shown once after first ToS acceptance)
void show_welcome_overlay(void);

// Update zone border color on home screen cards
void update_ambient_tint(void);

#ifdef __cplusplus
}
#endif

#endif // UI_HOME_SCREEN_H
