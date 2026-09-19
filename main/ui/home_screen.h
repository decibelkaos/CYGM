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

// The trend arrow's canvas, in both card states. Both files that draw the
// arrow need these: main.c owns the renderer, home_screen.c owns the layout,
// and when the collapsed size lived in only one of them the two drifted apart
// and the arrow spent a release drawn 4 px off its own centre.
#define HOME_TREND_SIZE           68   // collapsed: the band between unit and delta
#define HOME_TREND_SIZE_EXPANDED  80   // expanded: matches the large digits

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
/**
 * Re-measure the city name against its slot and start, restart or stop the
 * scroll. Call after changing home_location_label's text.
 */
/**
 * Show a short tag in the home screen's status corner, or hide it when the
 * text is NULL or empty. Set over serial with "label <text>"; the device
 * cannot work out its own COM port, so the host has to say.
 */
void home_set_device_label(const char *text);

void home_location_scroll_sync(void);

void show_login_success_overlay_ui(void);
void show_login_error_overlay_ui(const char *reason);

// Legal disclaimer overlay (shown on boot until user accepts)
void show_disclaimer_overlay(void);

// Welcome overlay (shown once after first ToS acceptance)
void show_welcome_overlay(void);

// First-time setup walkthrough. Polled from the 1Hz home timer; it puts up one
// card per step still outstanding (WiFi, location, CGM) and does nothing once
// the walkthrough is finished or skipped.
void cygm_setup_guide_maybe_show(void);

// Bench hook for the "setup" serial command: restart the walkthrough at step 1
// and replay every card regardless of what is already configured.
void cygm_setup_guide_force(void);

// Update zone border color on home screen cards
void update_ambient_tint(void);

// True while the red-on-black night face owns the home screen. The trend
// renderer fills its canvas to match: black at night, card colour by day.
bool home_night_face_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // UI_HOME_SCREEN_H
