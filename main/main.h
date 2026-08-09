/*
 * main.h
 *
 * Public functions from main.c that are accessible to other modules.
 */

#ifndef MAIN_H
#define MAIN_H

#include "lvgl.h"
#include "dexcom_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== Boot Log ====================

/** Append a message to the splash-screen boot log. */
void add_boot_log(const char *msg);

// ==================== LVGL Styles ====================

/** Initialize the global LVGL styles (style_bg, style_card). Call once. */
void init_styles(void);

// ==================== Screen Creation ====================

/** Create the splash screen. */
void create_splash_screen(void);

// ==================== Drawing Functions ====================

/** Draw a weather icon (code 0-7) on a canvas. */
void draw_weather_icon(lv_obj_t *canvas, int weather_code);

/** Draw one animation frame (0-255) of a weather icon on a canvas. */
void draw_weather_icon_animated(lv_obj_t *canvas, int weather_code, uint8_t frame);

/** LVGL timer callback driving the weather icon animation. */
void weather_icon_animation_timer_cb(lv_timer_t *timer);

/** Draw a sunrise icon on a canvas. */
void draw_sunrise_icon(lv_obj_t *canvas);

/** Draw a sunset icon on a canvas. */
void draw_sunset_icon(lv_obj_t *canvas);

/** Draw the glucose trend arrow on a canvas. */
void draw_trend_arrow(lv_obj_t *canvas, dexcom_trend_t trend);
void draw_trend_arrow_sized(lv_obj_t *canvas, dexcom_trend_t trend, int sz);

/**
 * Start/stop the trend-demo ticker to match cygm_demo_trend (hardware/screenshot.h).
 * Takes the LVGL lock itself, so it is safe to call from the serial command task.
 */
void cygm_demo_trend_sync(void);

// ==================== Glucose Functions ====================

/** Check a reading against the alarm thresholds and fire alarms if needed. */
void check_glucose_alarms(int glucose_mg_dl);

/** Stop the visual alarm (overlay, container, timer, LED, brightness restore). */
void stop_visual_alarm(void);

/** Stop the audio alarm (timer, buzzer). */
void stop_audio_alarm(void);

// ==================== Alarm Engine ====================

/**
 * True while the configured quiet-hours window is active; non-urgent alerts are
 * downgraded to visual + LED only. Always false when the clock is unsynced — a
 * wrong clock must not mute alarms.
 */
bool cygm_quiet_hours_active(void);

/**
 * Effective urgent-low threshold in mg/dL: the user's low threshold when that
 * tier is enabled, otherwise the non-disableable safety floor (default 55).
 */
int cygm_urgent_low_threshold(void);

/**
 * Data-gap watchdog: alerts when no fresh reading has arrived for the configured
 * number of minutes, warning before the 30-minute stale cutoff. Never changes
 * glucose_data_valid. Safe to call at any point of the glucose cycle, WiFi up or down.
 */
void check_data_gap_alert(void);

/**
 * Predictive-low and rate-of-change check. Call only after a fresh reading has
 * been stored in the history ring buffer.
 */
void check_predictive_alerts(void);

/** Refresh the glucose display widgets (value, trend, time ago). */
void update_glucose_display(void);

/** Glucose polling background task. */
void glucose_update_task(void *pvParameters);

// ==================== Task Lifecycle Helpers ====================

/** Delete weather/time/battery tasks to prevent mutex contention during TLS. */
bool delete_background_tasks_for_ssl(const char *reason);

/** Recreate weather/time/battery tasks after TLS operation (static stacks). */
void recreate_background_tasks(void);

/**
 * Create every background task that is not already running, with its canonical
 * core pinning, priority and stack size. Idempotent. The single creation point,
 * so no call site can drift on core/priority.
 */
void ensure_tasks_running(void);

// Task stack sizes (bytes) — shared by main.c and background_tasks.c.
// Heap-allocated so task deletion frees contiguous memory for TLS reconnection.
#define WEATHER_STACK_SIZE 6144   // Covers geocoding (HTTP+JSON) plus esp_wifi_set_country_code
#define TIME_STACK_SIZE    2048   // Headroom for localtime_r, night fade and logging
#define BATTERY_STACK_SIZE 2560   // 2048 left only 48 bytes free at high-water mark

// ==================== Event Callbacks ====================

/** Back button: navigate to the previous screen. */
void back_btn_event_cb(lv_event_t *e);

/** Menu button: navigate to the main menu. */
void menu_btn_event_cb(lv_event_t *e);

/** WiFi icon tap: navigate to WiFi setup. */
void wifi_icon_event_cb(lv_event_t *e);

/** Clock label tap: navigate to time settings. */
void clock_label_event_cb(lv_event_t *e);

/** Temperature unit toggle (C/F). */
void temp_toggle_event_cb(lv_event_t *e);

/** Glucose unit toggle (mg/dL <-> mmol/L). */
void glucose_unit_toggle_event_cb(lv_event_t *e);

/** Date format toggle (month-day <-> day-month). */
void date_label_event_cb(lv_event_t *e);

/** Main menu item tap handler. */
void menu_item_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif // MAIN_H
