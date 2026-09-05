/*
 * home_screen.h - main home screen: time, weather, and glucose display.
 */

#ifndef UI_HOME_SCREEN_H
#define UI_HOME_SCREEN_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void create_home_screen(void);

// Draw the battery icon onto `canvas`; `percent` is the 0-100 fill level.
void draw_battery_icon(lv_obj_t *canvas, uint32_t color, int percent);

// Draw the no-battery plug icon onto `canvas` (same 30x14 canvas as the gauge).
void draw_plug_icon(lv_obj_t *canvas, uint32_t color);

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

// True while the red-on-black night face owns the home screen. The trend
// renderer fills its canvas to match: black at night, card colour by day.
bool home_night_face_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // UI_HOME_SCREEN_H
