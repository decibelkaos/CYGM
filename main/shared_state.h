/*
 * shared_state.h
 *
 * Global variable declarations and shared constants for CYGM modules
 * All global variables are defined in main.c and declared extern here
 */

#ifndef SHARED_STATE_H
#define SHARED_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst820.h"
#include "lvgl.h"
#include "nvs_config.h"
#include "dexcom_api.h"
#include "cgm_types.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== Version Information ====================
// Version format: major.minor.patch.YYYY-MM-DD.stage
#define CYGM_VERSION_MAJOR 0
#define CYGM_VERSION_MINOR 15
#define CYGM_VERSION_PATCH 1
#define CYGM_VERSION_BUILD 0
#define CYGM_VERSION_DATE "2026-08-08"
#define CYGM_VERSION_STAGE "Beta"  // "Alpha", "Beta", or "Release"

// Full version string
#define CYGM_VERSION_STRING "0.15.1.2026-08-08.Beta"

// ==================== Hardware Configuration ====================

// LCD
#define LCD_MOSI    13
#define LCD_SCLK    14
#define LCD_CS      15
#define LCD_DC      2
#define LCD_BL      27
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

// Touch
#define TOUCH_I2C_NUM   I2C_NUM_0
#define TOUCH_SDA       33
#define TOUCH_SCL       32
#define TOUCH_INT       GPIO_NUM_NC
#define TOUCH_RST       25

// LED
#define LED_RED         4
#define LED_GREEN       16
#define LED_BLUE        17
#define LED_RED_CHANNEL     LEDC_CHANNEL_1
#define LED_GREEN_CHANNEL   LEDC_CHANNEL_2
#define LED_BLUE_CHANNEL    LEDC_CHANNEL_3
#define LED_TIMER           LEDC_TIMER_1

// Buzzer
#define BUZZER_GPIO         26
#define BUZZER_CHANNEL      LEDC_CHANNEL_0
#define BUZZER_TIMER        LEDC_TIMER_0

// Battery
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define BATTERY_ADC_GPIO    39
#define BATTERY_VOLTAGE_DIVIDER 1.50
#define BATTERY_FULL_VOLTAGE 4.0
#define BATTERY_NOMINAL_VOLTAGE 3.7
#define BATTERY_LOW_VOLTAGE 3.2
#define BATTERY_CRITICAL_PERCENT 10

// ==================== Color Palette ====================
#define COLOR_BG            0x000000  // Pure black background
#define COLOR_CARD_BG       0x1a2332  // Dark blue-grey cards
#define COLOR_TEXT_WHITE    0xFFFFFF  // Pure white text
#define COLOR_TEXT_GRAY     0x9CA3AF  // Light gray for secondary
#define COLOR_TEXT_DIM      0x6B7280  // Dimmed gray
#define COLOR_ACCENT_BLUE   0x3B82F6  // Bright blue accent
#define COLOR_ACCENT_LIGHT  0x60A5FA  // Lighter blue
#define COLOR_GREEN         0x22C55E  // Green for good
#define COLOR_YELLOW        0xFACC15  // Yellow warning
#define COLOR_ORANGE        0xF59E0B  // Orange warning
#define COLOR_RED           0xEF4444  // Red alert
#define COLOR_BORDER        0xFFFFFF  // White border/outline

// Extended palette — modal, interaction, and chart colors
#define COLOR_MODAL_BG      0x0F1419  // Dark modal card background
#define COLOR_MODAL_BORDER  0x2A3A4A  // Subtle modal border
#define COLOR_DIVIDER       0x263040  // Section divider lines
#define COLOR_PRESSED       0x2a3545  // Generic button pressed state
#define COLOR_PRESSED_GREEN 0x1a3a2a  // Green button pressed state
#define COLOR_PRESSED_RED   0x3a1a1a  // Red button pressed state
#define COLOR_PRESSED_BLUE  0x1a2a45  // Blue button pressed state
#define COLOR_INPUT_BG      0x253040  // Text input field background
#define COLOR_CHART_BG      0x0D1117  // Chart background (very dark)
#define COLOR_CHART_GRID    0x1B2433  // Chart grid lines

// ==================== Type Definitions ====================

// Alarm state tracking
typedef enum {
    ALARM_STATE_NONE = 0,
    ALARM_STATE_HIGH_ALARM,
    ALARM_STATE_HIGH_WARNING,
    ALARM_STATE_LOW_WARNING,
    ALARM_STATE_LOW_ALARM
} active_alarm_state_t;

// ==================== Global Hardware Handles ====================

// LVGL
extern lv_disp_t *lvgl_display;
extern lv_indev_t *lvgl_touch_indev;

// LCD & Touch
extern esp_lcd_panel_handle_t lcd_panel;
extern esp_lcd_panel_io_handle_t lcd_io_handle;
extern esp_lcd_touch_handle_t touch_handle;
extern i2c_master_bus_handle_t touch_i2c_bus;

// Task handles
extern TaskHandle_t wifi_task_handle;
extern TaskHandle_t ui_task_handle;
extern TaskHandle_t weather_task_handle;
extern TaskHandle_t glucose_task_handle;
extern TaskHandle_t time_task_handle;
extern TaskHandle_t battery_task_handle;

// ==================== Background Task Control ====================

extern bool home_screen_active;       // True when home screen is displayed
extern bool pause_background_tasks;   // Set to true to pause Dexcom/Weather fetching
extern SemaphoreHandle_t network_mutex;  // Mutex to prevent simultaneous network operations

// ==================== UI Screens ====================

extern lv_obj_t *screen_splash;
extern lv_obj_t *screen_home;
extern lv_obj_t *screen_menu;
extern lv_obj_t *screen_wifi_list;
extern lv_obj_t *screen_wifi_password;
extern lv_obj_t *screen_zipcode_entry;
extern lv_obj_t *screen_cgm_menu;
extern lv_obj_t *screen_dexcom_auth;
extern lv_obj_t *screen_dexcom_login;
extern lv_obj_t *screen_dexcom_username_entry;
extern lv_obj_t *screen_dexcom_password_entry;
extern lv_obj_t *screen_dexcom_status;
extern lv_obj_t *screen_alarm_settings;
extern lv_obj_t *screen_time_weather_settings;
extern lv_obj_t *screen_tone_picker;
extern lv_obj_t *screen_alarm_preview;
extern lv_obj_t *screen_alarm_detail_editor;

// ==================== Alarm Configuration ====================

extern cgm_alarms_t current_alarm_settings;
extern alarm_config_t *editing_alarm;
extern lv_obj_t *alarm_threshold_value_label;

// Alarm-engine extension settings (quiet hours, escalation, data-gap watchdog,
// predictive alerts). Frozen layout and loader contract live in cgm_types.h.
// Persisted separately from cgm_alarms_t so growing it can never reset the
// user's thresholds, tones and volumes.
extern cygm_alarm_ext_t alarm_ext_settings;

// Alarm state (volatile: written on Core 0, read on Core 1 and vice versa)
extern volatile active_alarm_state_t current_alarm_state;
extern bool alarm_acknowledged;

// Visual alarm components
extern lv_obj_t *visual_alarm_overlay;
extern lv_obj_t *visual_alarm_disarm_btn;
extern lv_obj_t *visual_alarm_disarm_progress;
extern lv_obj_t *visual_alarm_disarm_label;
extern lv_timer_t *visual_alarm_timer;
extern volatile bool visual_alarm_active;
extern uint8_t visual_alarm_pulse_state;
extern volatile int64_t alarm_snooze_until;

// ==================== Battery Monitoring ====================

extern adc_oneshot_unit_handle_t adc_handle;
extern adc_cali_handle_t adc_cali_handle;
extern float battery_voltage;
extern int battery_percent;
extern bool battery_low_warning_shown;
extern lv_obj_t *battery_icon;
extern lv_obj_t *battery_canvas;
extern lv_obj_t *battery_percent_label;
extern lv_timer_t *battery_flash_timer;
extern lv_timer_t *battery_charging_fade_timer;

// ==================== Brightness Control ====================

extern uint8_t screen_brightness_percent;  // 0-100%
extern uint8_t saved_brightness_percent;   // Saved before alarm override
extern bool dim_while_charging;            // Idle dim even on the charger (default off)
extern lv_obj_t *brightness_icon;
extern lv_obj_t *brightness_overlay;
extern lv_obj_t *brightness_slider;
extern lv_obj_t *brightness_value_label;

// ==================== Boot Log ====================

extern lv_obj_t *boot_log_label;
extern lv_obj_t *boot_progress_bar;  // Animated progress bar on splash screen
extern char boot_log_buffer[512];

// ==================== Dexcom Login UI ====================

extern lv_obj_t *dexcom_username_label;
extern lv_obj_t *dexcom_password_label;
extern char dexcom_username_buf[64];
extern char dexcom_password_buf[64];

// Entry screens
extern lv_obj_t *dexcom_username_entry_label;
extern lv_obj_t *dexcom_username_entry_ta;
extern lv_obj_t *dexcom_username_entry_keyboard;
extern lv_obj_t *dexcom_password_entry_label;
extern lv_obj_t *dexcom_password_entry_ta;
extern lv_obj_t *dexcom_password_entry_keyboard;

// ==================== Libre Login UI ====================

extern lv_obj_t *screen_libre_auth;
extern lv_obj_t *screen_libre_login;
extern lv_obj_t *screen_libre_email_entry;
extern lv_obj_t *screen_libre_password_entry;
extern lv_obj_t *screen_libre_status;

extern char libre_email_buf[64];
extern char libre_password_buf[64];

// ==================== Nightscout Login UI ====================

extern lv_obj_t *screen_nightscout_auth;
extern lv_obj_t *screen_nightscout_login;
extern lv_obj_t *screen_nightscout_url_entry;
extern lv_obj_t *screen_nightscout_token_entry;
extern lv_obj_t *screen_nightscout_status;

extern char nightscout_url_buf[128];
extern char nightscout_token_buf[64];

// ==================== Glucose Data ====================

extern int current_glucose;
extern dexcom_trend_t current_trend;
extern time_t glucose_timestamp;
extern bool glucose_data_valid;
extern bool glucose_data_fresh;
extern dexcom_status_t glucose_status;
extern bool first_glucose_received;
extern bool sensor_change_mode;        // User confirmed CGM sensor change in progress

// ==================== Home Screen Widgets ====================

extern lv_obj_t *label_clock_h;
extern lv_obj_t *label_clock_colon;
extern lv_obj_t *label_clock_m;
extern lv_obj_t *label_date;
extern lv_obj_t *label_ampm;
extern lv_obj_t *label_glucose;
extern lv_obj_t *trend_canvas;
extern lv_obj_t *label_time_ago;
extern lv_obj_t *label_unit;              // "mg/dL" unit label (repositioned in expanded mode)
extern lv_obj_t *glucose_freshness_arc;    // Arc counting down to the next CGM pull
extern bool glucose_fetch_active;          // True while a glucose fetch is in progress
extern lv_timer_t *glucose_timer_update;    // Timer to update the progress bar
extern int64_t last_glucose_fetch_time;     // Timestamp of last glucose fetch (milliseconds)
extern bool glucose_fetch_in_progress;      // True when actively fetching glucose
extern bool glucose_fetch_failed;           // True if last fetch failed
extern bool glucose_force_fetch_requested;  // True when user requested manual fetch
extern volatile int64_t glucose_next_fetch_ms;   // esp_timer ms deadline of the next pull
extern volatile int     glucose_fetch_period_s;  // interval the deadline was armed with
extern lv_timer_t *glucose_fetch_animation_timer;  // Animation timer for bouncing indicator
extern lv_obj_t *label_wifi_status;

// ==================== WiFi State ====================

extern bool wifi_connected;
extern wifi_ap_record_t wifi_networks[10];
extern uint16_t wifi_network_count;
extern char wifi_password[64];
extern uint8_t wifi_password_len;
extern char selected_ssid[33];

// WiFi password screen widgets
extern lv_obj_t *password_label;
extern lv_obj_t *password_field;
extern lv_obj_t *keyboard;
extern bool caps_lock_enabled;
extern uint32_t last_shift_press_time;
extern bool shift_waiting_for_char;

// ==================== Time/Weather Settings ====================

extern char user_timezone[64];
extern bool user_dst_enabled;
extern bool user_24hr_format;
extern char user_zipcode[11];
extern char geocoded_zipcode[11];  // Geocoded zipcode tracker
extern bool user_temp_celsius;
extern uint8_t user_weather_interval_min;
extern char user_location[64];
extern bool user_glucose_mmol;   // false = mg/dL (US), true = mmol/L (rest of world)
extern bool user_date_dmy;       // false = US month-day, true = day-month

// Locale glucose formatting helpers (defined in main.c). Glucose is stored
// canonical mg/dL; these convert at the display boundary only.
const char *cygm_glucose_unit(void);                       // "mg/dL" or "mmol/L"
void cygm_format_glucose(int mgdl, char *buf, size_t len); // value only
void cygm_format_threshold(int mgdl, char *buf, size_t len); // value + unit
int mgdl_to_mmol_tenths(int mgdl);                         // rounded tenths of mmol/L (for delta math)
// ==================== Weather Data ====================

extern int current_temp_f;
extern int high_temp_f;
extern int low_temp_f;
extern const char *current_condition;
extern time_t sunrise_time;
extern time_t sunset_time;
extern int current_weather_code;
extern float user_latitude;
extern float user_longitude;

// Weather widgets
extern lv_obj_t *home_temp_label;
extern lv_obj_t *home_hilo_label;
extern lv_obj_t *home_weather_canvas;
extern lv_obj_t *home_condition_label;
extern lv_obj_t *home_location_label;
extern lv_obj_t *home_sunrise_canvas;
extern lv_obj_t *home_sunrise_label;
extern lv_obj_t *home_sunset_canvas;
extern lv_obj_t *home_sunset_label;

// Weather icon animation
extern lv_timer_t *weather_icon_anim_timer;
extern uint8_t weather_icon_anim_frame;

// ==================== CGM Expanded View ====================

extern bool cgm_expanded;               // True when CGM card is expanded
extern lv_obj_t *left_card;             // Left card (time/weather)
extern lv_obj_t *right_card;            // Right card (CGM data)
extern lv_obj_t *expanded_time_h;       // Hour label in expanded CGM top bar
extern lv_obj_t *expanded_time_colon;   // Colon label in expanded CGM top bar (blinks)
extern lv_obj_t *expanded_time_m;       // Minute label in expanded CGM top bar
extern lv_obj_t *expanded_temp_label;   // Temp label shown in expanded CGM top bar

// ==================== SD Card Logging ====================

extern bool sd_glucose_logging_enabled;  // Toggle glucose CSV logging to SD card

// ==================== Global LVGL Styles ====================

extern lv_style_t style_bg;
extern lv_style_t style_card;

// ==================== Shared Interaction Contract ====================
// Canonical ghost-button look + touch target, applied by every UI module
// instead of hand-rolling the same inline property list.
//
// style_btn_ghost   — transparent fill, hairline accent border, canonical
//                     radius, shadows explicitly off.
// style_btn_pressed — LV_STATE_PRESSED feedback: background fill and border
//                     opacity only. Deliberately carries NO LV_STYLE_TRANSITION
//                     (a style transition is an animation, which freezes this
//                     hardware) and no shadow/recolor. It sets no border COLOR
//                     either, so a green/red ghost keeps its own accent.
//
// LVGL v8 precedence: a local style beats an added style of the same state, and
// any PRESSED-state style beats a default-state local one — so callers may
// apply the helper and then override properties in any order.
extern lv_style_t style_btn_ghost;
extern lv_style_t style_btn_pressed;

#define CYGM_BTN_RADIUS      8    // Canonical ghost-button corner radius
#define CYGM_BTN_BORDER_W    1    // Hairline border
#define CYGM_BTN_H          36    // Canonical ghost-button height
#define CYGM_BTN_W_TEXT    120    // Canonical width for a text button
#define CYGM_BTN_W_ICON     40    // Canonical width for an icon-only button
#define CYGM_HIT_TARGET_MIN 50    // Minimum effective touch target (px, per axis)
#define CYGM_BTN_EXT_CLICK   8    // Default touch-box growth per side

// Apply the canonical ghost look + pressed feedback and grow the touch box
// (not the drawn box) toward CYGM_HIT_TARGET_MIN. Safe on lv_btn and on a
// plain lv_obj. Call AFTER lv_obj_set_size() so the size can be measured;
// when the size is not resolved yet it falls back to CYGM_BTN_EXT_CLICK.
void cygm_apply_ghost_btn(lv_obj_t *btn);

#ifdef __cplusplus
}
#endif

#endif // SHARED_STATE_H
