/*
 * main.h
 *
 * Public functions from main.c that are accessible to other modules
 */

#ifndef MAIN_H
#define MAIN_H

#include "lvgl.h"
#include "dexcom_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== Boot Log ====================

/**
 * Add message to boot log (splash screen)
 * @param msg Message to add to log
 */
void add_boot_log(const char *msg);

// ==================== LVGL Styles ====================

/**
 * Initialize global LVGL styles (style_bg, style_card)
 * Called once during UI initialization
 */
void init_styles(void);

// ==================== Screen Creation ====================

/**
 * Create splash screen (boot animation)
 */
void create_splash_screen(void);

// ==================== Drawing Functions ====================

/**
 * Draw weather icon on canvas based on weather code
 * @param canvas LVGL canvas object
 * @param weather_code Weather condition code (0-7)
 */
void draw_weather_icon(lv_obj_t *canvas, int weather_code);

/**
 * Draw sunrise icon on canvas
 * @param canvas LVGL canvas object
 */
void draw_sunrise_icon(lv_obj_t *canvas);

/**
 * Draw sunset icon on canvas
 * @param canvas LVGL canvas object
 */
void draw_sunset_icon(lv_obj_t *canvas);

/**
 * Draw trend arrow on canvas based on glucose trend
 * @param canvas LVGL canvas object
 * @param trend Dexcom trend direction
 */
void draw_trend_arrow(lv_obj_t *canvas, dexcom_trend_t trend);

// ==================== Glucose Functions ====================

/**
 * Check glucose value against alarm thresholds and trigger alarms if needed
 * @param glucose_mg_dl Glucose value in mg/dL
 */
void check_glucose_alarms(int glucose_mg_dl);

/**
 * Update glucose display widgets (value, trend, time ago)
 */
void update_glucose_display(void);

/**
 * Glucose update background task
 * @param pvParameters Task parameter (unused)
 */
void glucose_update_task(void *pvParameters);

// ==================== Event Callbacks ====================

/**
 * Back button event callback (navigates to previous screen)
 */
void back_btn_event_cb(lv_event_t *e);

/**
 * Menu button event callback (navigates to main menu)
 */
void menu_btn_event_cb(lv_event_t *e);

/**
 * WiFi icon click event callback (navigates to WiFi setup)
 */
void wifi_icon_event_cb(lv_event_t *e);

/**
 * Clock label tap event callback (navigates to time settings)
 */
void clock_label_event_cb(lv_event_t *e);

/**
 * Temperature display toggle event callback (C/F switch)
 */
void temp_toggle_event_cb(lv_event_t *e);

/**
 * Menu item button event callback (handles menu navigation)
 */
void menu_item_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif // MAIN_H
