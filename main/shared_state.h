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
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== Version Information ====================
#define CYGM_VERSION "1.3.6"
#define CYGM_STAGE "Beta"  // "Alpha", "Beta", or "Release"

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

// CardKB
#define CARDKB_I2C_NUM  I2C_NUM_1
#define CARDKB_SDA      21
#define CARDKB_SCL      22
#define CARDKB_ADDR     0x5F

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
#define BATTERY_VOLTAGE_DIVIDER 1.46
#define BATTERY_FULL_VOLTAGE 4.0
#define BATTERY_NOMINAL_VOLTAGE 3.7
#define BATTERY_LOW_VOLTAGE 3.2
#define BATTERY_CRITICAL_PERCENT 10

// Feature flags
#define ENABLE_WEB_SERVER 0

// ==================== Color Palette ====================
#define COLOR_BG            0x000000  // Pure black background
#define COLOR_CARD_BG       0x1a2332  // Dark blue-grey cards
#define COLOR_TEXT_WHITE    0xFFFFFF  // Pure white text
#define COLOR_TEXT_GRAY     0x9CA3AF  // Light gray for secondary
#define COLOR_TEXT_DIM      0x6B7280  // Dimmed gray
#define COLOR_ACCENT_BLUE   0x3B82F6  // Bright blue accent
#define COLOR_ACCENT_LIGHT  0x60A5FA  // Lighter blue
#define COLOR_GREEN         0x22C55E  // Green for good
#define COLOR_ORANGE        0xF59E0B  // Orange warning
#define COLOR_RED           0xEF4444  // Red alert
#define COLOR_BORDER        0xFFFFFF  // White border/outline

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
extern i2c_master_bus_handle_t cardkb_i2c_bus;
extern i2c_master_dev_handle_t cardkb_dev;

// Task handles
extern TaskHandle_t wifi_task_handle;
extern TaskHandle_t ui_task_handle;
extern TaskHandle_t weather_task_handle;
extern TaskHandle_t glucose_task_handle;

// ==================== LED State ====================

extern volatile uint8_t led_breathe_r;
extern volatile uint8_t led_breathe_g;
extern volatile uint8_t led_breathe_b;

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
extern lv_obj_t *screen_zipcode_entry;
extern lv_obj_t *screen_tone_picker;
extern lv_obj_t *screen_alarm_preview;
extern lv_obj_t *screen_alarm_detail_editor;

// ==================== Alarm Configuration ====================

extern cgm_alarms_t current_alarm_settings;
extern alarm_config_t *editing_alarm;
extern lv_obj_t *alarm_threshold_value_label;

// Alarm state
extern active_alarm_state_t current_alarm_state;
extern bool alarm_acknowledged;

// Visual alarm components
extern lv_obj_t *visual_alarm_overlay;
extern lv_obj_t *visual_alarm_slider;
extern lv_obj_t *visual_alarm_slider_label;
extern lv_obj_t *screen_alarm_snooze;
extern lv_timer_t *visual_alarm_timer;
extern bool visual_alarm_active;
extern uint8_t visual_alarm_pulse_state;
extern int64_t alarm_snooze_until;

// Audio alarm
extern lv_timer_t *audio_alarm_timer;
extern alarm_config_t *active_alarm_config;

// ==================== Battery Monitoring ====================

extern adc_oneshot_unit_handle_t adc_handle;
extern adc_cali_handle_t adc_cali_handle;
extern float battery_voltage;
extern int battery_percent;
extern bool battery_low_warning_shown;
extern lv_obj_t *battery_icon;
extern lv_obj_t *battery_canvas;
extern lv_timer_t *battery_flash_timer;

// ==================== Brightness Control ====================

extern uint8_t screen_brightness_percent;  // 0-100%
extern uint8_t saved_brightness_percent;   // Saved before alarm override
extern lv_obj_t *brightness_icon;
extern lv_obj_t *brightness_overlay;
extern lv_obj_t *brightness_slider;
extern lv_obj_t *brightness_value_label;

// ==================== Boot Log ====================

extern lv_obj_t *boot_log_label;
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

// ==================== Glucose Data ====================

extern int current_glucose;
extern dexcom_trend_t current_trend;
extern time_t glucose_timestamp;
extern bool glucose_data_valid;
extern bool glucose_data_fresh;
extern dexcom_status_t glucose_status;
extern bool first_glucose_received;

// ==================== Home Screen Widgets ====================

extern lv_obj_t *label_clock;
extern lv_obj_t *label_date;
extern lv_obj_t *label_ampm;
extern lv_obj_t *label_glucose;
extern lv_obj_t *trend_canvas;
extern lv_obj_t *label_time_ago;
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
extern bool use_postal_code_entry;  // true = postal code, false = city dropdown

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

// ==================== Global LVGL Styles ====================

extern lv_style_t style_bg;
extern lv_style_t style_card;

#ifdef __cplusplus
}
#endif

#endif // SHARED_STATE_H
