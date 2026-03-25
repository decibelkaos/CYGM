/*
 * CYGM - Continuous Glucose Monitor Display
 * ESP32-IDF with LVGL
 *
 * Core 0: WiFi Management
 * Core 1: UI/Touch (LVGL)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

// LCD & LVGL
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst820.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

// Project modules
#include "shared_state.h"
#include "main.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "nightscout_api.h"
#include "cgm_types.h"
#include "hardware/led.h"
#include "hardware/buzzer.h"
#include "hardware/battery.h"
#include "hardware/display.h"
#include "hardware/screenshot.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "features/glucose_history.h"
#include "ui/menu_screen.h"
#include "ui/home_screen.h"
#include "ui/wifi_screens.h"
#include "ui/cgm_screens.h"
#include "ui/alarm_screens.h"
#include "ui/time_weather_screens.h"
#include "tasks/background_tasks.h"
#include "sd_logger.h"
#include "features/update_checker.h"
#include "features/heartbeat.h"

static const char *TAG = "CYGM";

// ==================== Hardware Configuration ====================

#define LCD_MOSI    13
#define LCD_SCLK    14
#define LCD_CS      15
#define LCD_DC      2
#define LCD_BL      27
#define LCD_WIDTH   320  // Landscape mode
#define LCD_HEIGHT  240

#define TOUCH_I2C_NUM   I2C_NUM_0
#define TOUCH_SDA       33
#define TOUCH_SCL       32
#define TOUCH_INT       GPIO_NUM_NC  // Use polling mode (WiFi conflict fix)
#define TOUCH_RST       25

#define LED_RED         4
#define LED_GREEN       16
#define LED_BLUE        17

// LED PWM channels
#define LED_RED_CHANNEL     LEDC_CHANNEL_1
#define LED_GREEN_CHANNEL   LEDC_CHANNEL_2
#define LED_BLUE_CHANNEL    LEDC_CHANNEL_3
#define LED_TIMER           LEDC_TIMER_1

// Speaker/Buzzer PWM (8002A amplifier on GPIO 26)
#define BUZZER_GPIO         26
#define BUZZER_CHANNEL      LEDC_CHANNEL_0
#define BUZZER_TIMER        LEDC_TIMER_0

// Battery Monitoring (3.7V LiPo)
// GPIO 39 confirmed - calibrated with unplugged battery at 4.10V
// Percentage based on under-load voltage: 4.0V=100%, 3.7V=50%, 3.2V=0% (shutdown to prevent damage)
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3   // GPIO 39 (ADC1_CH3)
#define BATTERY_ADC_GPIO    39
#define BATTERY_VOLTAGE_DIVIDER 1.50        // Calibrated: shows under-load voltage
#define BATTERY_FULL_VOLTAGE 4.0            // Fully charged under load (100%)
#define BATTERY_NOMINAL_VOLTAGE 3.7         // Nominal voltage (50%)
#define BATTERY_LOW_VOLTAGE 3.2             // Safe shutdown threshold (prevents damage)
#define BATTERY_CRITICAL_PERCENT 10         // Warning threshold (10%)

// SD Card Logging v2: Mount-on-write pattern (~2.5KB permanent vs old 13KB)
// Only mounts FAT during flush (~200ms), then unmounts immediately
// v1 was removed 2026-01-30 because 13KB constant RAM broke SSL

// ==================== Global Handles ====================

// LVGL
lv_disp_t *lvgl_display = NULL;
lv_indev_t *lvgl_touch_indev = NULL;

// Hardware
esp_lcd_panel_handle_t lcd_panel = NULL;
esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
esp_lcd_touch_handle_t touch_handle = NULL;
i2c_master_bus_handle_t touch_i2c_bus = NULL;

// Task handles
TaskHandle_t wifi_task_handle = NULL;
TaskHandle_t ui_task_handle = NULL;
TaskHandle_t weather_task_handle = NULL;
TaskHandle_t glucose_task_handle = NULL;
TaskHandle_t time_task_handle = NULL;
TaskHandle_t battery_task_handle = NULL;

// Task stacks are heap-allocated (not static) so that deleting tasks
// frees contiguous heap blocks — critical for TLS reconnection on ESP32.
// Tasks are created AFTER SSL auth so stacks interleave with the SSL
// region — freeing both on reconnect merges into one large block.

// LED breathing (disabled to save power - variables kept for future use)
volatile uint8_t led_breathe_r = 127, led_breathe_g = 0, led_breathe_b = 0;

// Background Task Control
bool home_screen_active = false;       // True when home screen is displayed
bool pause_background_tasks = false;   // Pause Dexcom/Weather when not on home screen
SemaphoreHandle_t network_mutex = NULL;  // Mutex to prevent simultaneous network operations

// UI Screens
lv_obj_t *screen_splash = NULL;
lv_obj_t *screen_home = NULL;
lv_obj_t *screen_menu = NULL;
lv_obj_t *screen_wifi_list = NULL;
lv_obj_t *screen_wifi_password = NULL;
lv_obj_t *screen_zipcode_entry = NULL;
lv_obj_t *screen_cgm_menu = NULL;
lv_obj_t *screen_dexcom_auth = NULL;
lv_obj_t *screen_dexcom_login = NULL;
lv_obj_t *screen_dexcom_username_entry = NULL;
lv_obj_t *screen_dexcom_password_entry = NULL;
lv_obj_t *screen_dexcom_status = NULL;
lv_obj_t *screen_alarm_settings = NULL;
lv_obj_t *screen_time_weather_settings = NULL;
lv_obj_t *screen_tone_picker = NULL;
lv_obj_t *screen_alarm_preview = NULL;
lv_obj_t *screen_alarm_detail_editor = NULL;

// Global alarm configuration
cgm_alarms_t current_alarm_settings;
alarm_config_t *editing_alarm = NULL;  // Pointer to alarm being edited
lv_obj_t *alarm_threshold_value_label = NULL;  // Reference to threshold value label for color updates

// Alarm state tracking (enum defined in shared_state.h)
active_alarm_state_t current_alarm_state = ALARM_STATE_NONE;
bool alarm_acknowledged = false;

// Visual alarm components
lv_obj_t *visual_alarm_overlay = NULL;      // Flash overlay for visual alarm
lv_obj_t *visual_alarm_container = NULL;    // Glass panel for alarm dismiss UI
lv_obj_t *visual_alarm_disarm_btn = NULL;      // Hold-to-disarm button
lv_obj_t *visual_alarm_disarm_progress = NULL; // Progress bar inside disarm button
lv_obj_t *visual_alarm_disarm_label = NULL;    // Label on disarm button
static int64_t disarm_press_start_ms = 0;      // Timestamp when button was pressed
#define DISARM_HOLD_MS 1500                     // Hold duration to disarm (1.5s)
lv_obj_t *screen_alarm_snooze = NULL;       // Snooze/dismiss screen
lv_timer_t *visual_alarm_timer = NULL;      // Timer for pulsing effect
bool visual_alarm_active = false;
uint8_t visual_alarm_pulse_state = 0;       // 0 = dim, 1 = bright flash
static uint32_t visual_alarm_flash_color = COLOR_RED;  // Current alarm flash color
static lv_obj_t *visual_alarm_glucose_label = NULL;    // Glucose value label in alarm card (for live updates)
int64_t alarm_snooze_until = 0;             // Unix timestamp when snooze expires

// Audio alarm repeat
static lv_timer_t *audio_alarm_timer = NULL;       // Timer for repeating audio
static alarm_config_t *active_alarm_config = NULL; // Current alarm being played

// Battery monitoring
adc_oneshot_unit_handle_t adc_handle = NULL;
adc_cali_handle_t adc_cali_handle = NULL;
float battery_voltage = 0.0f;
int battery_percent = 100;
bool battery_low_warning_shown = false;  // Track if low battery warning played
lv_obj_t *battery_icon = NULL;           // Battery icon on home screen
lv_obj_t *battery_canvas = NULL;         // Battery canvas for custom drawn icon
lv_obj_t *battery_percent_label = NULL;  // Battery percentage text label
lv_timer_t *battery_flash_timer = NULL;  // Timer for flashing red at low battery
lv_timer_t *battery_charging_fade_timer = NULL;  // Timer for fading charging icon

// Brightness control
uint8_t screen_brightness_percent = 80;  // Default 80%
uint8_t saved_brightness_percent = 80;   // Saved before alarm override
lv_obj_t *brightness_icon = NULL;
lv_obj_t *brightness_overlay = NULL;
lv_obj_t *brightness_slider = NULL;
lv_obj_t *brightness_value_label = NULL;


// Splash screen boot log
lv_obj_t *boot_log_label = NULL;
lv_obj_t *boot_progress_bar = NULL;  // Animated progress bar
char boot_log_buffer[512] = {0};  // Buffer for boot messages

// Dexcom login UI elements
lv_obj_t *dexcom_username_label = NULL;
lv_obj_t *dexcom_password_label = NULL;
char dexcom_username_buf[64] = {0};
char dexcom_password_buf[64] = {0};

// Libre UI screens and credential buffers
lv_obj_t *screen_libre_auth = NULL;
lv_obj_t *screen_libre_login = NULL;
lv_obj_t *screen_libre_email_entry = NULL;
lv_obj_t *screen_libre_password_entry = NULL;
lv_obj_t *screen_libre_status = NULL;
char libre_email_buf[64] = {0};
char libre_password_buf[64] = {0};

// Nightscout UI screens and credential buffers
lv_obj_t *screen_nightscout_auth = NULL;
lv_obj_t *screen_nightscout_login = NULL;
lv_obj_t *screen_nightscout_url_entry = NULL;
lv_obj_t *screen_nightscout_token_entry = NULL;
lv_obj_t *screen_nightscout_status = NULL;
char nightscout_url_buf[128] = {0};
char nightscout_token_buf[64] = {0};

// Glucose data - "Last Known Good" approach
// We ALWAYS display the last valid glucose reading, even if current fetch fails
// This prevents user anxiety from seeing "---" during temporary network/API issues
int current_glucose = 0;
dexcom_trend_t current_trend = TREND_NONE;
time_t glucose_timestamp = 0;
bool glucose_data_valid = false;      // True if we've EVER received valid data
bool glucose_data_fresh = false;       // True only if last fetch succeeded
dexcom_status_t glucose_status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
bool first_glucose_received = false;  // Track if we've received first reading
bool sensor_change_mode = false;      // User confirmed CGM sensor change in progress
bool sd_glucose_logging_enabled = true;  // SD card glucose CSV logging (NVS-persisted)

// Entry screen UI elements
lv_obj_t *dexcom_username_entry_label = NULL;
lv_obj_t *dexcom_username_entry_ta = NULL;
lv_obj_t *dexcom_username_entry_keyboard = NULL;
lv_obj_t *dexcom_password_entry_label = NULL;
lv_obj_t *dexcom_password_entry_ta = NULL;
lv_obj_t *dexcom_password_entry_keyboard = NULL;

// Home screen widgets
lv_obj_t *label_clock = NULL;
lv_obj_t *label_date = NULL;
lv_obj_t *label_ampm = NULL;  // AM/PM indicator (12hr mode only)
lv_obj_t *label_glucose = NULL;
lv_obj_t *trend_canvas = NULL;  // Canvas for custom-drawn trend arrow
lv_obj_t *label_time_ago = NULL;
lv_obj_t *label_unit = NULL;              // "mg/dL" unit label
lv_obj_t *glucose_fetch_timer_bar = NULL;  // Progress bar showing time until next glucose fetch
lv_timer_t *glucose_timer_update = NULL;    // Timer to update the progress bar
int64_t last_glucose_fetch_time = 0;        // Timestamp of last glucose fetch (milliseconds)
bool glucose_fetch_in_progress = false;     // True when actively fetching glucose
bool glucose_fetch_failed = false;          // True if last fetch failed
bool glucose_force_fetch_requested = false; // True when user requested manual fetch
lv_timer_t *glucose_fetch_animation_timer = NULL;  // Animation timer for bouncing indicator
lv_obj_t *label_wifi_status = NULL;

// WiFi state
bool wifi_connected = false;
wifi_ap_record_t wifi_networks[10];
uint16_t wifi_network_count = 0;
char wifi_password[64] = {0};
uint8_t wifi_password_len = 0;
char selected_ssid[33] = {0};

// WiFi password screen widgets
lv_obj_t *password_label = NULL;
lv_obj_t *password_field = NULL;
lv_obj_t *keyboard = NULL;
bool caps_lock_enabled = false;
uint32_t last_shift_press_time = 0;
bool shift_waiting_for_char = false;  // True when shift pressed but no char typed yet

// Time/Date/Weather settings
char user_timezone[64] = "America/New_York";  // Default timezone
bool user_dst_enabled = true;  // Daylight Saving Time enabled
bool user_24hr_format = false;  // 12-hour (AM/PM) vs 24-hour format
char user_zipcode[11] = "";  // US zipcode (5 or 9 digits)
bool user_temp_celsius = false;  // false = Fahrenheit, true = Celsius
uint8_t user_weather_interval_min = 5;  // Weather update interval (5-90 minutes)
char user_location[64] = "";  // City, State from geocoding

// Weather display data
int current_temp_f = 72;  // Current temperature in Fahrenheit
int high_temp_f = 78;      // High temperature
int low_temp_f = 65;       // Low temperature
const char *current_condition = "Partly Cloudy";  // Current weather condition
time_t sunrise_time = 0;   // Sunrise time (Unix timestamp)
time_t sunset_time = 0;    // Sunset time (Unix timestamp)
int current_weather_code = 1;  // 0=Clear, 1=Partly Cloudy, 2=Cloudy, 3=Rain, 4=Snow, 5=Storm, 6=Fog, 7=Ice
float user_latitude = 0.0f;    // User's latitude from zipcode
float user_longitude = 0.0f;   // User's longitude from zipcode

// Weather display widgets (for updating)
lv_obj_t *home_temp_label = NULL;
lv_obj_t *home_hilo_label = NULL;
lv_obj_t *home_weather_canvas = NULL;
lv_obj_t *home_condition_label = NULL;
lv_obj_t *home_location_label = NULL;
lv_obj_t *home_sunrise_canvas = NULL;
lv_obj_t *home_sunrise_label = NULL;
lv_obj_t *home_sunset_canvas = NULL;
lv_obj_t *home_sunset_label = NULL;

// Weather icon animation
lv_timer_t *weather_icon_anim_timer = NULL;
uint8_t weather_icon_anim_frame = 0;

// CGM Expanded View
bool cgm_expanded = false;
lv_obj_t *left_card = NULL;
lv_obj_t *right_card = NULL;
lv_obj_t *expanded_time_label = NULL;
lv_obj_t *expanded_temp_label = NULL;

// ===== HELPER FUNCTIONS =====

// Weather icon animation timer callback
void weather_icon_animation_timer_cb(lv_timer_t *timer) {
    if (!home_screen_active || home_weather_canvas == NULL) {
        return;
    }

    // Increment animation frame
    weather_icon_anim_frame++;

    // Redraw weather icon with new frame
    if (lvgl_port_lock(0)) {
        draw_weather_icon(home_weather_canvas, current_weather_code);
        lvgl_port_unlock();
    }
}

// Draw weather icon on a canvas
void draw_weather_icon(lv_obj_t *canvas, int weather_code) {
    draw_weather_icon_animated(canvas, weather_code, weather_icon_anim_frame);
}

// Helper: draw a solid fluffy cloud shape centered at (cx, cy)
static void draw_solid_cloud(lv_obj_t *canvas, int cx, int cy, uint32_t color, uint8_t opa) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.bg_opa = opa;

    // Flat bottom base
    r.radius = 5;
    lv_canvas_draw_rect(canvas, cx - 18, cy - 3, 36, 11, &r);

    // Rounded bumps on top
    r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, cx - 17, cy - 12, 15, 15, &r);   // Left bump
    lv_canvas_draw_rect(canvas, cx - 6, cy - 17, 17, 17, &r);    // Center bump (tallest)
    lv_canvas_draw_rect(canvas, cx + 5, cy - 10, 13, 13, &r);    // Right bump
}

// Helper: draw a smaller cloud (for partly cloudy overlay)
static void draw_small_cloud(lv_obj_t *canvas, int cx, int cy, uint32_t color, uint8_t opa) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.bg_opa = opa;

    // Flat bottom
    r.radius = 4;
    lv_canvas_draw_rect(canvas, cx - 14, cy - 2, 28, 9, &r);

    // Bumps
    r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, cx - 13, cy - 9, 12, 12, &r);    // Left
    lv_canvas_draw_rect(canvas, cx - 4, cy - 13, 13, 13, &r);    // Center
    lv_canvas_draw_rect(canvas, cx + 4, cy - 7, 10, 10, &r);     // Right
}

void draw_weather_icon_animated(lv_obj_t *canvas, int weather_code, uint8_t frame) {
    if (canvas == NULL) return;

    // Clear canvas
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);

    switch (weather_code) {
        case 0: { // CLEAR SKY - Sun (day) / Moon + stars (night)
            if (is_night_time()) {
                // Soft moon glow
                rect_dsc.bg_color = lv_color_hex(0xF0E68C);
                rect_dsc.bg_opa = 30;
                rect_dsc.radius = LV_RADIUS_CIRCLE;
                lv_canvas_draw_rect(canvas, 15, 7, 32, 32, &rect_dsc);

                // Solid moon body
                rect_dsc.bg_opa = LV_OPA_COVER;
                lv_canvas_draw_rect(canvas, 19, 11, 24, 24, &rect_dsc);

                // Crescent cutout (overlap right side with bg color)
                rect_dsc.bg_color = lv_color_hex(COLOR_CARD_BG);
                lv_canvas_draw_rect(canvas, 29, 9, 18, 18, &rect_dsc);

                // Twinkling stars (proven algorithm - keeps existing logic)
                for (uint32_t i = 0; i < 12; i++) {
                    uint32_t star_lifecycle = 200 + (i * 47) % 150;
                    uint32_t life_position = (frame + (i * 79)) % star_lifecycle;
                    uint32_t epoch = (frame + (i * 79)) / star_lifecycle;
                    uint32_t seed = (epoch * 1103515245U + i * 12345U);

                    int star_x = ((seed >> 16) % 58);
                    seed = seed * 1103515245U + 12345U;
                    int star_y = ((seed >> 16) % 48);
                    seed = seed * 1103515245U + 12345U;
                    int star_size = ((seed >> 8) % 10 < 2) ? 2 : 1;

                    uint32_t vis_start = star_lifecycle / 5;
                    uint32_t vis_dur = 50 + ((seed >> 12) % 60);
                    uint16_t brightness = 0;

                    if (life_position >= vis_start && life_position < vis_start + vis_dur) {
                        uint32_t vp = life_position - vis_start;
                        uint32_t fi = vis_dur / 4;
                        uint32_t fo = vis_dur / 4;
                        if (vp < fi) brightness = (vp * 200) / fi;
                        else if (vp > vis_dur - fo) brightness = ((vis_dur - vp) * 200) / fo;
                        else brightness = 180 + ((seed >> 4) % 40);
                    }

                    if (brightness > 50) {
                        lv_draw_rect_dsc_t s;
                        lv_draw_rect_dsc_init(&s);
                        s.bg_color = lv_color_hex(0xFFFFFF);
                        s.bg_opa = (brightness > 255) ? 255 : (uint8_t)brightness;
                        s.radius = 0;
                        lv_canvas_draw_rect(canvas, star_x, star_y, star_size, star_size, &s);
                    }
                }
            } else {
                // Warm outer glow
                rect_dsc.bg_color = lv_color_hex(0xFFCC00);
                rect_dsc.bg_opa = 35;
                rect_dsc.radius = LV_RADIUS_CIRCLE;
                lv_canvas_draw_rect(canvas, 13, 8, 34, 34, &rect_dsc);

                // Solid sun body
                rect_dsc.bg_color = lv_color_hex(0xFFAA00);
                rect_dsc.bg_opa = LV_OPA_COVER;
                lv_canvas_draw_rect(canvas, 18, 13, 24, 24, &rect_dsc);

                // Bright inner highlight
                rect_dsc.bg_color = lv_color_hex(0xFFDD44);
                rect_dsc.bg_opa = 160;
                lv_canvas_draw_rect(canvas, 23, 18, 14, 14, &rect_dsc);

                // 8 primary rotating rays (thick, pulsing length)
                line_dsc.color = lv_color_hex(0xFFAA00);
                line_dsc.width = 2;
                lv_point_t ray[2];
                float rot = frame * 3.14159 / 128.0;
                int pulse = 22 + (int)(2.0 * sin(frame * 0.15));

                for (int i = 0; i < 8; i++) {
                    float a = (i * 45.0 * 3.14159 / 180.0) + rot;
                    ray[0].x = 30 + (int)(16 * cos(a));
                    ray[0].y = 25 + (int)(16 * sin(a));
                    ray[1].x = 30 + (int)(pulse * cos(a));
                    ray[1].y = 25 + (int)(pulse * sin(a));
                    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);
                }

                // 8 secondary rays (thin, offset 22.5 degrees)
                line_dsc.width = 1;
                line_dsc.color = lv_color_hex(0xFFCC44);
                int pulse2 = 18 + (int)(1.5 * sin(frame * 0.15 + 1.0));

                for (int i = 0; i < 8; i++) {
                    float a = ((i * 45.0 + 22.5) * 3.14159 / 180.0) + rot;
                    ray[0].x = 30 + (int)(15 * cos(a));
                    ray[0].y = 25 + (int)(15 * sin(a));
                    ray[1].x = 30 + (int)(pulse2 * cos(a));
                    ray[1].y = 25 + (int)(pulse2 * sin(a));
                    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);
                }
            }
            break;
        }

        case 1: { // PARTLY CLOUDY - Sun/Moon peeking + fluffy cloud
            if (is_night_time()) {
                // Small moon glow (upper-left)
                rect_dsc.bg_color = lv_color_hex(0xF0E68C);
                rect_dsc.bg_opa = 25;
                rect_dsc.radius = LV_RADIUS_CIRCLE;
                lv_canvas_draw_rect(canvas, 4, 1, 24, 24, &rect_dsc);

                // Moon body
                rect_dsc.bg_opa = LV_OPA_COVER;
                lv_canvas_draw_rect(canvas, 7, 4, 18, 18, &rect_dsc);

                // Crescent cutout
                rect_dsc.bg_color = lv_color_hex(COLOR_CARD_BG);
                lv_canvas_draw_rect(canvas, 15, 2, 14, 14, &rect_dsc);
            } else {
                // Small sun glow (upper-left)
                rect_dsc.bg_color = lv_color_hex(0xFFCC00);
                rect_dsc.bg_opa = 30;
                rect_dsc.radius = LV_RADIUS_CIRCLE;
                lv_canvas_draw_rect(canvas, 3, 0, 26, 26, &rect_dsc);

                // Sun body
                rect_dsc.bg_color = lv_color_hex(0xFFAA00);
                rect_dsc.bg_opa = LV_OPA_COVER;
                lv_canvas_draw_rect(canvas, 7, 4, 18, 18, &rect_dsc);

                // Inner highlight
                rect_dsc.bg_color = lv_color_hex(0xFFDD44);
                rect_dsc.bg_opa = 140;
                lv_canvas_draw_rect(canvas, 10, 7, 12, 12, &rect_dsc);

                // A few visible rays (upper-left quadrant)
                line_dsc.color = lv_color_hex(0xFFAA00);
                line_dsc.width = 2;
                lv_point_t ray[2];
                for (int i = 2; i < 6; i++) {
                    float a = i * 45.0 * 3.14159 / 180.0;
                    ray[0].x = 16 + (int)(12 * cos(a));
                    ray[0].y = 13 + (int)(12 * sin(a));
                    ray[1].x = 16 + (int)(17 * cos(a));
                    ray[1].y = 13 + (int)(17 * sin(a));
                    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);
                }
            }

            // Fluffy cloud lower-right with gentle sine drift
            int drift = (int)(3.0 * sin(frame * 0.04));
            draw_small_cloud(canvas, 36 + drift, 34, 0x666666, LV_OPA_COVER);
            draw_small_cloud(canvas, 34 + drift, 33, COLOR_TEXT_GRAY, LV_OPA_COVER);
            break;
        }

        case 2: { // OVERCAST - Layered clouds for depth
            int drift = (int)(4.0 * sin(frame * 0.03));

            // Back cloud (darker, offset for depth)
            draw_solid_cloud(canvas, 34 + drift / 2, 30, 0x555555, LV_OPA_COVER);

            // Front cloud (lighter, main)
            draw_solid_cloud(canvas, 28 + drift, 25, COLOR_TEXT_GRAY, LV_OPA_COVER);

            // Subtle highlight on front cloud top
            rect_dsc.bg_color = lv_color_hex(0xBBBBBB);
            rect_dsc.bg_opa = 50;
            rect_dsc.radius = LV_RADIUS_CIRCLE;
            lv_canvas_draw_rect(canvas, 21 + drift, 9, 14, 12, &rect_dsc);
            break;
        }

        case 3: { // RAIN - Solid cloud + diagonal rain streaks
            // Solid cloud
            draw_solid_cloud(canvas, 30, 18, COLOR_TEXT_GRAY, LV_OPA_COVER);

            // 5 rain drops falling at slight diagonal (wind effect)
            line_dsc.color = lv_color_hex(COLOR_ACCENT_BLUE);
            line_dsc.width = 2;
            lv_point_t drop[2];

            static const int drop_x[] = {12, 21, 30, 39, 48};
            static const int drop_speed[] = {4, 3, 5, 3, 4};
            static const int drop_phase[] = {0, 5, 2, 8, 4};

            for (int i = 0; i < 5; i++) {
                int y = 28 + ((frame / 2 + drop_phase[i] * 3) * drop_speed[i] / 3) % 22;

                if (y >= 26 && y < 48) {
                    // Slight diagonal slant
                    drop[0].x = drop_x[i];
                    drop[0].y = y;
                    drop[1].x = drop_x[i] - 1;
                    drop[1].y = y + 5;
                    lv_canvas_draw_line(canvas, drop, 2, &line_dsc);
                }
            }

            // Secondary thinner drops (staggered)
            line_dsc.width = 1;
            line_dsc.color = lv_color_hex(0x4488CC);
            static const int drop2_x[] = {17, 35, 44};
            static const int drop2_phase[] = {3, 6, 1};

            for (int i = 0; i < 3; i++) {
                int y = 30 + ((frame / 2 + drop2_phase[i] * 4) * 4 / 3) % 18;
                if (y >= 28 && y < 47) {
                    drop[0].x = drop2_x[i];
                    drop[0].y = y;
                    drop[1].x = drop2_x[i] - 1;
                    drop[1].y = y + 4;
                    lv_canvas_draw_line(canvas, drop, 2, &line_dsc);
                }
            }
            break;
        }

        case 4: { // SNOW - Solid cloud + floating snowflakes
            // Solid cloud
            draw_solid_cloud(canvas, 30, 16, COLOR_TEXT_GRAY, LV_OPA_COVER);

            // 5 snowflakes as asterisk patterns (* = 3 crossing lines)
            line_dsc.color = lv_color_hex(0xFFFFFF);
            line_dsc.width = 1;
            lv_point_t sf[2];

            static const int sf_x[] = {10, 22, 34, 46, 16};
            static const int sf_phase[] = {0, 7, 3, 10, 5};

            for (int i = 0; i < 5; i++) {
                int fall = ((frame / 4 + sf_phase[i] * 5) % 24);
                int drift = (int)(3.0 * sin((frame + i * 40) * 0.08));
                int sx = sf_x[i] + drift;
                int sy = 26 + fall;

                if (sy >= 24 && sy < 48) {
                    int arm = 3;
                    // Vertical arm
                    sf[0].x = sx; sf[0].y = sy - arm;
                    sf[1].x = sx; sf[1].y = sy + arm;
                    lv_canvas_draw_line(canvas, sf, 2, &line_dsc);
                    // Diagonal arm 1
                    sf[0].x = sx - arm; sf[0].y = sy - arm + 1;
                    sf[1].x = sx + arm; sf[1].y = sy + arm - 1;
                    lv_canvas_draw_line(canvas, sf, 2, &line_dsc);
                    // Diagonal arm 2
                    sf[0].x = sx + arm; sf[0].y = sy - arm + 1;
                    sf[1].x = sx - arm; sf[1].y = sy + arm - 1;
                    lv_canvas_draw_line(canvas, sf, 2, &line_dsc);
                }
            }
            break;
        }

        case 5: { // STORM - Dark cloud + lightning + rain
            // Dark cloud with purple tint
            draw_solid_cloud(canvas, 30, 16, 0x444455, LV_OPA_COVER);

            // Storm rain (thinner, blue-gray)
            line_dsc.color = lv_color_hex(0x5588CC);
            line_dsc.width = 1;
            lv_point_t drop[2];
            for (int i = 0; i < 4; i++) {
                int dx = 14 + i * 10;
                int dy = 24 + ((frame * 3 + i * 7) % 22);
                if (dy >= 24 && dy < 48) {
                    drop[0].x = dx; drop[0].y = dy;
                    drop[1].x = dx - 1; drop[1].y = dy + 4;
                    lv_canvas_draw_line(canvas, drop, 2, &line_dsc);
                }
            }

            // Lightning flash (periodic)
            bool flash = (frame < 8) || (frame >= 70 && frame < 78) || (frame >= 180 && frame < 186);
            if (flash) {
                // Glow behind bolt
                rect_dsc.bg_color = lv_color_hex(0xFFFF88);
                rect_dsc.bg_opa = 40;
                rect_dsc.radius = LV_RADIUS_CIRCLE;
                lv_canvas_draw_rect(canvas, 22, 26, 18, 18, &rect_dsc);

                // Zigzag lightning bolt
                line_dsc.color = lv_color_hex(0xFFFF00);
                line_dsc.width = 3;
                lv_point_t bolt[2];

                bolt[0].x = 32; bolt[0].y = 24;
                bolt[1].x = 28; bolt[1].y = 32;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

                bolt[0].x = 28; bolt[0].y = 32;
                bolt[1].x = 34; bolt[1].y = 33;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

                bolt[0].x = 34; bolt[0].y = 33;
                bolt[1].x = 28; bolt[1].y = 44;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

                // Bright bolt core (thinner white line for intensity)
                line_dsc.color = lv_color_hex(0xFFFFCC);
                line_dsc.width = 1;

                bolt[0].x = 32; bolt[0].y = 24;
                bolt[1].x = 28; bolt[1].y = 32;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

                bolt[0].x = 28; bolt[0].y = 32;
                bolt[1].x = 34; bolt[1].y = 33;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);

                bolt[0].x = 34; bolt[0].y = 33;
                bolt[1].x = 28; bolt[1].y = 44;
                lv_canvas_draw_line(canvas, bolt, 2, &line_dsc);
            }
            break;
        }

        case 6: { // FOG - Soft drifting horizontal bars at varying opacity
            static const int fog_y[] = {10, 19, 28, 36, 44};
            static const int fog_w[] = {30, 40, 44, 36, 28};
            static const uint8_t fog_opa[] = {90, 130, 170, 130, 80};

            rect_dsc.radius = 3;
            rect_dsc.bg_color = lv_color_hex(COLOR_TEXT_GRAY);

            for (int i = 0; i < 5; i++) {
                int drift = (int)(6.0 * sin(frame * (0.025 + i * 0.008) + i * 1.5));
                rect_dsc.bg_opa = fog_opa[i];
                int x = (60 - fog_w[i]) / 2 + drift;
                lv_canvas_draw_rect(canvas, x, fog_y[i], fog_w[i], 5, &rect_dsc);
            }

            // Secondary translucent layer for depth
            rect_dsc.bg_color = lv_color_hex(0xAAAAAA);
            rect_dsc.bg_opa = 40;
            rect_dsc.radius = 2;
            for (int i = 0; i < 3; i++) {
                int drift = (int)(4.0 * sin(frame * (0.04 + i * 0.01) + i * 2.0));
                int y = 15 + i * 12;
                lv_canvas_draw_rect(canvas, 8 + drift, y, 36, 4, &rect_dsc);
            }
            break;
        }

        case 7: { // ICE - Cloud + falling ice crystals
            // Cloud at top
            draw_solid_cloud(canvas, 30, 16, COLOR_TEXT_GRAY, LV_OPA_COVER);

            // Falling 6-arm ice crystals
            line_dsc.color = lv_color_hex(0xAADDFF);
            line_dsc.width = 2;
            lv_point_t ice[2];

            static const int ice_x[] = {14, 30, 46};
            static const int ice_phase[] = {0, 5, 2};

            for (int i = 0; i < 3; i++) {
                int fall = ((frame / 3 + ice_phase[i] * 6) % 24);
                int cx = ice_x[i];
                int cy = 26 + fall;

                if (cy >= 24 && cy < 48) {
                    int s = 4;
                    // Vertical arm
                    ice[0].x = cx; ice[0].y = cy - s;
                    ice[1].x = cx; ice[1].y = cy + s;
                    lv_canvas_draw_line(canvas, ice, 2, &line_dsc);
                    // Diagonal arm 1
                    ice[0].x = cx - s; ice[0].y = cy - s / 2;
                    ice[1].x = cx + s; ice[1].y = cy + s / 2;
                    lv_canvas_draw_line(canvas, ice, 2, &line_dsc);
                    // Diagonal arm 2
                    ice[0].x = cx + s; ice[0].y = cy - s / 2;
                    ice[1].x = cx - s; ice[1].y = cy + s / 2;
                    lv_canvas_draw_line(canvas, ice, 2, &line_dsc);

                    // Small branch tips (makes it look more crystalline)
                    line_dsc.width = 1;
                    ice[0].x = cx; ice[0].y = cy - s;
                    ice[1].x = cx - 2; ice[1].y = cy - s + 2;
                    lv_canvas_draw_line(canvas, ice, 2, &line_dsc);
                    ice[0].x = cx; ice[0].y = cy - s;
                    ice[1].x = cx + 2; ice[1].y = cy - s + 2;
                    lv_canvas_draw_line(canvas, ice, 2, &line_dsc);
                    line_dsc.width = 2;
                }
            }
            break;
        }

        default:
            break;
    }
}

// Draw trend arrow on canvas (60x60 pixels)
// Style: Solid severity-colored glow (wide) + solid blue core (narrow) on top.
// ALL glow segments drawn first, then ALL core segments — proper z-layering
// so the blue core is always cleanly on top at junctions.
// Green glow = stable (flat, 45°), Orange = warning (single), Red = alarm (double)
void draw_trend_arrow_sized(lv_obj_t *canvas, dexcom_trend_t trend, int sz) {
    if (canvas == NULL) return;

    // Scale factor: all coordinates are designed for 60px canvas
    // S(v) maps a 60-unit coordinate to the target size
    #define S(v) ((v) * sz / 60)

    // Glow (outer) — severity color, full opacity, wide
    lv_draw_line_dsc_t glow;
    lv_draw_line_dsc_init(&glow);
    glow.round_start = 1;
    glow.round_end = 1;
    glow.width = S(12);

    // Core (inner) — blue, full opacity, narrower
    lv_draw_line_dsc_t core;
    lv_draw_line_dsc_init(&core);
    core.round_start = 1;
    core.round_end = 1;
    core.color = lv_color_hex(0x3B82F6);
    core.width = S(8);

    // Set glow color based on severity
    switch (trend) {
        case TREND_FLAT:
        case TREND_FORTY_FIVE_UP:
        case TREND_FORTY_FIVE_DOWN:
            glow.color = lv_color_hex(0x10B981);  // Green — stable
            break;
        case TREND_SINGLE_UP:
        case TREND_SINGLE_DOWN:
            glow.color = lv_color_hex(0xF59E0B);  // Orange — warning
            break;
        case TREND_DOUBLE_UP:
        case TREND_DOUBLE_DOWN:
            glow.color = lv_color_hex(0xEF4444);  // Red — alarm
            break;
        default:
            return;
    }

    // Helper: draw a single line segment on canvas (coordinates auto-scaled)
    #define DRAW_SEG(dsc, x1, y1, x2, y2) do { \
        lv_point_t _p[2] = {{S(x1), S(y1)}, {S(x2), S(y2)}}; \
        lv_canvas_draw_line(canvas, _p, 2, &dsc); \
    } while(0)

    // For double arrows, use slightly narrower lines to avoid overlap
    lv_draw_line_dsc_t glow_d = glow;
    lv_draw_line_dsc_t core_d = core;
    glow_d.width = S(11);
    core_d.width = S(7);

    switch (trend) {
        case TREND_FLAT:
            // Horizontal arrow pointing right, centered at (30,30)
            DRAW_SEG(glow, 6, 30, 50, 30);       // shaft
            DRAW_SEG(glow, 50, 30, 41, 21);      // upper winglet
            DRAW_SEG(glow, 50, 30, 41, 39);      // lower winglet
            DRAW_SEG(core, 6, 30, 50, 30);
            DRAW_SEG(core, 50, 30, 41, 21);
            DRAW_SEG(core, 50, 30, 41, 39);
            break;

        case TREND_FORTY_FIVE_UP:
            DRAW_SEG(glow, 10, 50, 47, 13);
            DRAW_SEG(glow, 47, 13, 35, 12);
            DRAW_SEG(glow, 47, 13, 48, 25);
            DRAW_SEG(core, 10, 50, 47, 13);
            DRAW_SEG(core, 47, 13, 35, 12);
            DRAW_SEG(core, 47, 13, 48, 25);
            break;

        case TREND_FORTY_FIVE_DOWN:
            DRAW_SEG(glow, 10, 10, 47, 47);
            DRAW_SEG(glow, 47, 47, 35, 48);
            DRAW_SEG(glow, 47, 47, 48, 35);
            DRAW_SEG(core, 10, 10, 47, 47);
            DRAW_SEG(core, 47, 47, 35, 48);
            DRAW_SEG(core, 47, 47, 48, 35);
            break;

        case TREND_SINGLE_UP:
            DRAW_SEG(glow, 30, 54, 30, 13);
            DRAW_SEG(glow, 30, 13, 21, 22);
            DRAW_SEG(glow, 30, 13, 39, 22);
            DRAW_SEG(core, 30, 54, 30, 13);
            DRAW_SEG(core, 30, 13, 21, 22);
            DRAW_SEG(core, 30, 13, 39, 22);
            break;

        case TREND_SINGLE_DOWN:
            DRAW_SEG(glow, 30, 6, 30, 47);
            DRAW_SEG(glow, 30, 47, 21, 38);
            DRAW_SEG(glow, 30, 47, 39, 38);
            DRAW_SEG(core, 30, 6, 30, 47);
            DRAW_SEG(core, 30, 47, 21, 38);
            DRAW_SEG(core, 30, 47, 39, 38);
            break;

        case TREND_DOUBLE_UP:
            DRAW_SEG(glow_d, 19, 52, 19, 16);
            DRAW_SEG(glow_d, 19, 16, 11, 23);
            DRAW_SEG(glow_d, 19, 16, 26, 23);
            DRAW_SEG(glow_d, 41, 52, 41, 16);
            DRAW_SEG(glow_d, 41, 16, 34, 23);
            DRAW_SEG(glow_d, 41, 16, 49, 23);
            DRAW_SEG(core_d, 19, 52, 19, 16);
            DRAW_SEG(core_d, 19, 16, 11, 23);
            DRAW_SEG(core_d, 19, 16, 26, 23);
            DRAW_SEG(core_d, 41, 52, 41, 16);
            DRAW_SEG(core_d, 41, 16, 34, 23);
            DRAW_SEG(core_d, 41, 16, 49, 23);
            break;

        case TREND_DOUBLE_DOWN:
            DRAW_SEG(glow_d, 19, 8, 19, 44);
            DRAW_SEG(glow_d, 19, 44, 11, 37);
            DRAW_SEG(glow_d, 19, 44, 26, 37);
            DRAW_SEG(glow_d, 41, 8, 41, 44);
            DRAW_SEG(glow_d, 41, 44, 34, 37);
            DRAW_SEG(glow_d, 41, 44, 49, 37);
            DRAW_SEG(core_d, 19, 8, 19, 44);
            DRAW_SEG(core_d, 19, 44, 11, 37);
            DRAW_SEG(core_d, 19, 44, 26, 37);
            DRAW_SEG(core_d, 41, 8, 41, 44);
            DRAW_SEG(core_d, 41, 44, 34, 37);
            DRAW_SEG(core_d, 41, 44, 49, 37);
            break;

        default:
            break;
    }

    #undef DRAW_SEG
    #undef S
}

void draw_trend_arrow(lv_obj_t *canvas, dexcom_trend_t trend) {
    draw_trend_arrow_sized(canvas, trend, 60);
}

// ==================== Time & SNTP Functions ====================
// Moved to features/time_system.c

// ==================== Weather Functions ====================
// Moved to features/weather_system.c

// ==================== Alarm Functions ====================

// Forward declarations for alarm helpers
static void start_visual_alarm(void);
static void stop_visual_alarm(void);
static void start_audio_alarm(alarm_config_t *alarm);
static void stop_audio_alarm(void);
static void trigger_alarm(active_alarm_state_t state);
static void visual_alarm_pulse_timer_cb(lv_timer_t *timer);
static void audio_alarm_repeat_timer_cb(lv_timer_t *timer);

static uint32_t get_glucose_alarm_color(int glucose_mg_dl);
void check_glucose_alarms(int glucose_mg_dl);
void update_glucose_display(void);
static void visual_alarm_disarm_event_cb(lv_event_t *e);

// Get alarm color based on glucose value and alarm thresholds
static uint32_t get_glucose_alarm_color(int glucose_mg_dl) {
    // Urgent alarms take priority over warnings
    if (current_alarm_settings.high_alarm.enabled &&
        glucose_mg_dl >= current_alarm_settings.high_alarm.threshold) {
        return current_alarm_settings.high_alarm.text_color;
    }

    if (current_alarm_settings.low_alarm.enabled &&
        glucose_mg_dl <= current_alarm_settings.low_alarm.threshold) {
        return current_alarm_settings.low_alarm.text_color;
    }

    // Check warnings
    if (current_alarm_settings.high_warning.enabled &&
        glucose_mg_dl >= current_alarm_settings.high_warning.threshold) {
        return current_alarm_settings.high_warning.text_color;
    }

    if (current_alarm_settings.low_warning.enabled &&
        glucose_mg_dl <= current_alarm_settings.low_warning.threshold) {
        return current_alarm_settings.low_warning.text_color;
    }

    // No alarm triggered - use white
    return COLOR_TEXT_WHITE;
}

// Trigger an alarm (audio + visual)
static void trigger_alarm(active_alarm_state_t state) {
    alarm_config_t *alarm = NULL;
    const char *alarm_name = "Unknown";

    switch (state) {
        case ALARM_STATE_HIGH_ALARM:
            alarm = &current_alarm_settings.high_alarm;
            alarm_name = "High Alarm";
            break;
        case ALARM_STATE_HIGH_WARNING:
            alarm = &current_alarm_settings.high_warning;
            alarm_name = "High Warning";
            break;
        case ALARM_STATE_LOW_WARNING:
            alarm = &current_alarm_settings.low_warning;
            alarm_name = "Low Warning";
            break;
        case ALARM_STATE_LOW_ALARM:
            alarm = &current_alarm_settings.low_alarm;
            alarm_name = "Low Alarm";
            break;
        default:
            return;
    }

    if (alarm == NULL) return;

    ESP_LOGW(TAG, "ALARM TRIGGERED: %s (threshold: %d mg/dL)",
             alarm_name, alarm->threshold);
    sd_log(TAG, "ALARM: %s triggered, glucose=%d threshold=%d audio=%d visual=%d",
           alarm_name, current_glucose, alarm->threshold,
           alarm->audio_enabled, alarm->visual_enabled);

    // Check if snooze is active
    time_t now;
    time(&now);
    if (alarm_snooze_until > 0 && now < alarm_snooze_until) {
        ESP_LOGI(TAG, "Alarm snoozed until %ld (current: %ld)", (long)alarm_snooze_until, (long)now);
        return;
    }

    // Audio alert (if enabled and not acknowledged)
    if (alarm->audio_enabled && !alarm_acknowledged) {
        start_audio_alarm(alarm);
    }

    // Visual alert (pulsing white screen)
    if (alarm->visual_enabled && !alarm_acknowledged) {
        start_visual_alarm();
    }
}

// Check glucose against alarm thresholds and trigger if needed
void check_glucose_alarms(int glucose_mg_dl) {
    if (!glucose_data_valid) {
        return;  // Don't check alarms without valid data
    }

    // Determine which alarm should trigger (urgent takes priority over warnings)
    active_alarm_state_t new_state = ALARM_STATE_NONE;

    if (current_alarm_settings.high_alarm.enabled &&
        glucose_mg_dl >= current_alarm_settings.high_alarm.threshold) {
        new_state = ALARM_STATE_HIGH_ALARM;
    } else if (current_alarm_settings.low_alarm.enabled &&
               glucose_mg_dl <= current_alarm_settings.low_alarm.threshold) {
        new_state = ALARM_STATE_LOW_ALARM;
    } else if (current_alarm_settings.high_warning.enabled &&
               glucose_mg_dl >= current_alarm_settings.high_warning.threshold) {
        new_state = ALARM_STATE_HIGH_WARNING;
    } else if (current_alarm_settings.low_warning.enabled &&
               glucose_mg_dl <= current_alarm_settings.low_warning.threshold) {
        new_state = ALARM_STATE_LOW_WARNING;
    }

    // Check snooze status
    if (alarm_snooze_until > 0) {
        time_t now;
        time(&now);

        if (now < alarm_snooze_until) {
            // Still snoozed - don't trigger alarm
            int minutes_remaining = (alarm_snooze_until - now) / 60;
            int seconds_remaining = (alarm_snooze_until - now) % 60;
            ESP_LOGI(TAG, "Alarm snoozed for %d min %d sec more (until %ld, current %ld)",
                     minutes_remaining, seconds_remaining, (long)alarm_snooze_until, (long)now);
            return;  // Exit early - alarm is snoozed
        } else if (new_state != ALARM_STATE_NONE) {
            // Snooze expired and alarm condition still exists - re-trigger
            ESP_LOGI(TAG, "Snooze expired! Re-triggering alarm (state: %d)", new_state);
            sd_log(TAG, "ALARM: snooze expired, re-trigger state=%d glucose=%d", new_state, glucose_mg_dl);
            alarm_snooze_until = 0;  // Clear snooze
            alarm_acknowledged = false;  // Reset acknowledgment
            current_alarm_state = new_state;  // Update state
            trigger_alarm(new_state);
            return;  // Don't process state change logic below
        } else {
            // Snooze expired but glucose returned to normal
            ESP_LOGI(TAG, "Snooze expired but glucose returned to normal");
            sd_log(TAG, "ALARM: snooze expired, glucose normal (%d)", glucose_mg_dl);
            alarm_snooze_until = 0;  // Clear snooze
            current_alarm_state = ALARM_STATE_NONE;
            return;
        }
    }

    // State changed?
    if (new_state != current_alarm_state) {
        ESP_LOGI(TAG, "Alarm state changed: %d -> %d", current_alarm_state, new_state);
        sd_log(TAG, "ALARM: state %d->%d, glucose=%d", current_alarm_state, new_state, glucose_mg_dl);
        current_alarm_state = new_state;
        alarm_acknowledged = false;  // Reset acknowledgment on state change

        if (new_state != ALARM_STATE_NONE) {
            trigger_alarm(new_state);
        } else {
            ESP_LOGI(TAG, "Glucose returned to normal range");
            sd_log(TAG, "ALARM: glucose returned to normal (%d)", glucose_mg_dl);
            stop_visual_alarm();
            stop_audio_alarm();
        }
    }
}

// Timer callback for pulsing alarm — flashes a bright border at the screen edges
static void visual_alarm_pulse_timer_cb(lv_timer_t *timer) {
    if (!visual_alarm_active || visual_alarm_overlay == NULL) {
        return;
    }

    // Auto-clear if glucose data has gone stale (e.g., Dexcom session expired overnight)
    if (glucose_data_valid && glucose_timestamp > 0) {
        time_t now;
        time(&now);
        int data_age_min = (int)difftime(now, glucose_timestamp) / 60;
        if (data_age_min > 15) {
            ESP_LOGI(TAG, "Alarm auto-clear: data stale (%d min old)", data_age_min);
            sd_log(TAG, "ALARM: auto-clear, data stale %d min", data_age_min);
            stop_visual_alarm();
            stop_audio_alarm();
            current_alarm_state = ALARM_STATE_NONE;
            return;
        }
    }

    if (lvgl_port_lock(0)) {
        if (visual_alarm_pulse_state == 0) {
            // Flash ON — thick alarm-color border at screen edges
            lv_obj_set_style_border_width(visual_alarm_overlay, 8, 0);
            lv_obj_set_style_border_color(visual_alarm_overlay, lv_color_hex(visual_alarm_flash_color), 0);
            lv_obj_set_style_border_opa(visual_alarm_overlay, LV_OPA_COVER, 0);
            visual_alarm_pulse_state = 1;
        } else {
            // Flash OFF — no border
            lv_obj_set_style_border_width(visual_alarm_overlay, 0, 0);
            visual_alarm_pulse_state = 0;
        }
        lv_obj_invalidate(visual_alarm_overlay);
        lvgl_port_unlock();
    }
}

// Timer callback for repeating audio alarm.
// Runs in LVGL task context (Core 1) — safe to call buzzer without cross-core
// LEDC spinlock contention.
static void audio_alarm_repeat_timer_cb(lv_timer_t *timer) {
    if (active_alarm_config != NULL && active_alarm_config->audio_enabled) {
        ESP_LOGI(TAG, "Playing alarm tone: %d at volume %d%%", active_alarm_config->tone, active_alarm_config->volume);
        play_alarm_tone(active_alarm_config->tone, active_alarm_config->volume);

        // If repeat is disabled, this was a one-shot — clean up
        if (!active_alarm_config->audio_repeat) {
            active_alarm_config = NULL;
            audio_alarm_timer = NULL;
            lv_timer_del(timer);  // Safe: already in LVGL context
        }
    }
}

// Start audio alarm (with optional repeat).
// IMPORTANT: Does NOT call play_alarm_tone() directly, because this function
// may be called from Core 0 (wifi_task).  Playing buzzer tones from Core 0
// causes cross-core LEDC spinlock contention with LED/backlight on Core 1,
// resulting in Interrupt WDT crashes.  Instead, an LVGL timer is created
// with lv_timer_ready() so the tone plays on Core 1's next LVGL cycle.
static void start_audio_alarm(alarm_config_t *alarm) {
    if (alarm == NULL || !alarm->audio_enabled) {
        return;
    }

    // Stop any existing audio alarm
    stop_audio_alarm();

    // Save reference to alarm config
    active_alarm_config = alarm;

    ESP_LOGI(TAG, "Scheduling alarm tone: %d at volume %d%% (via LVGL timer)", alarm->tone, alarm->volume);

    // Create LVGL timer to play tone from Core 1 (LVGL task context).
    // lv_timer_ready() makes it fire on the very next LVGL cycle.
    if (lvgl_port_lock(200)) {
        audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
        lv_timer_ready(audio_alarm_timer);  // Fire immediately
        lvgl_port_unlock();
    }
}

// Stop audio alarm
static void stop_audio_alarm(void) {
    // Clear config FIRST so timer callback won't start a new tone
    active_alarm_config = NULL;

    if (audio_alarm_timer != NULL) {
        ESP_LOGI(TAG, "Stopping audio alarm repeat timer");
        // Timeout must exceed longest alarm tone (Dixie Horn ~3.5s)
        if (lvgl_port_lock(5000)) {
            lv_timer_del(audio_alarm_timer);
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "LVGL lock timeout deleting alarm timer");
        }
        audio_alarm_timer = NULL;
    }
    buzzer_stop();  // Ensure buzzer is silenced
}

// Event handler for hold-to-disarm button
static void visual_alarm_disarm_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        disarm_press_start_ms = esp_timer_get_time() / 1000;
    } else if (code == LV_EVENT_PRESSING) {
        if (disarm_press_start_ms == 0) return;
        int64_t elapsed = (esp_timer_get_time() / 1000) - disarm_press_start_ms;

        // Update progress bar width (button is 260 wide, progress has 4px margin each side = max 252)
        int progress_w = (int)((elapsed * 252) / DISARM_HOLD_MS);
        if (progress_w > 252) progress_w = 252;
        if (visual_alarm_disarm_progress != NULL) {
            lv_obj_set_width(visual_alarm_disarm_progress, progress_w);
        }

        // Update label text to show countdown
        if (visual_alarm_disarm_label != NULL) {
            if (elapsed < DISARM_HOLD_MS) {
                char buf[32];
                int remaining_tenths = (int)((DISARM_HOLD_MS - elapsed) / 100);
                snprintf(buf, sizeof(buf), "Hold... %d.%d s", remaining_tenths / 10, remaining_tenths % 10);
                lv_label_set_text(visual_alarm_disarm_label, buf);
            }
        }

        if (elapsed >= DISARM_HOLD_MS) {
            ESP_LOGI(TAG, "Hold-to-disarm completed - showing snooze/dismiss overlay");
            disarm_press_start_ms = 0;
            stop_visual_alarm();
            stop_audio_alarm();

            if (lvgl_port_lock(0)) {
                if (screen_alarm_snooze != NULL) {
                    lv_obj_del(screen_alarm_snooze);
                    screen_alarm_snooze = NULL;
                }
                create_alarm_snooze_screen();
                lvgl_port_unlock();
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Released before hold completed — reset
        if (disarm_press_start_ms != 0) {
            disarm_press_start_ms = 0;
            if (visual_alarm_disarm_progress != NULL) {
                lv_obj_set_width(visual_alarm_disarm_progress, 0);
            }
            if (visual_alarm_disarm_label != NULL) {
                lv_label_set_text(visual_alarm_disarm_label, "Hold to Disarm");
            }
            ESP_LOGI(TAG, "Disarm button released early - reset");
        }
    }
}

// Start visual alarm (glass-themed panel with color-coded flash)
static void start_visual_alarm(void) {
    if (visual_alarm_active) {
        return;  // Already active
    }

    ESP_LOGI(TAG, "Starting visual alarm");
    visual_alarm_active = true;
    visual_alarm_pulse_state = 0;

    // Save current brightness and set to 100% for visual alarm
    saved_brightness_percent = screen_brightness_percent;
    screen_brightness_percent = 100;
    display_set_brightness(100);
    ESP_LOGI(TAG, "Brightness set to 100%% for visual alarm (saved: %d%%)", saved_brightness_percent);

    // Start fast red LED flash for alarm
    led_start_alarm_flash();

    // Determine alarm info
    const char *alarm_type = "ALARM";
    uint32_t alarm_color = COLOR_RED;
    if (current_alarm_state == ALARM_STATE_HIGH_ALARM) {
        alarm_type = "HIGH ALARM";
        alarm_color = current_alarm_settings.high_alarm.text_color;
    } else if (current_alarm_state == ALARM_STATE_HIGH_WARNING) {
        alarm_type = "HIGH WARNING";
        alarm_color = current_alarm_settings.high_warning.text_color;
    } else if (current_alarm_state == ALARM_STATE_LOW_WARNING) {
        alarm_type = "LOW WARNING";
        alarm_color = current_alarm_settings.low_warning.text_color;
    } else if (current_alarm_state == ALARM_STATE_LOW_ALARM) {
        alarm_type = "LOW ALARM";
        alarm_color = current_alarm_settings.low_alarm.text_color;
    }
    visual_alarm_flash_color = alarm_color;

    if (lvgl_port_lock(0)) {
        // ---- Dim backdrop with alarm-color border frame at screen edges ----
        visual_alarm_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(visual_alarm_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(visual_alarm_overlay, 0, 0);
        lv_obj_set_style_bg_color(visual_alarm_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(visual_alarm_overlay, LV_OPA_60, 0);
        lv_obj_set_style_border_width(visual_alarm_overlay, 0, 0);  // Starts off, pulse timer toggles
        lv_obj_set_style_border_side(visual_alarm_overlay, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(visual_alarm_overlay, 12, 0);  // Rounded inner corners on border
        lv_obj_set_style_pad_all(visual_alarm_overlay, 0, 0);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(visual_alarm_overlay);

        // ---- Glass panel (chart-style overlay) ----
        visual_alarm_container = lv_obj_create(lv_scr_act());
        lv_obj_set_size(visual_alarm_container, 290, 210);
        lv_obj_center(visual_alarm_container);
        lv_obj_set_style_bg_color(visual_alarm_container, lv_color_hex(0x0F1419), 0);
        lv_obj_set_style_bg_opa(visual_alarm_container, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(visual_alarm_container, 16, 0);
        lv_obj_set_style_border_width(visual_alarm_container, 1, 0);
        lv_obj_set_style_border_color(visual_alarm_container, lv_color_hex(0x2A3A4A), 0);
        lv_obj_set_style_border_opa(visual_alarm_container, LV_OPA_50, 0);
        lv_obj_set_style_shadow_width(visual_alarm_container, 25, 0);
        lv_obj_set_style_shadow_color(visual_alarm_container, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(visual_alarm_container, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(visual_alarm_container, 0, 0);
        lv_obj_clear_flag(visual_alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(visual_alarm_container);

        // ---- Title row: bell + alarm type ----
        lv_obj_t *bell = lv_label_create(visual_alarm_container);
        lv_label_set_text(bell, LV_SYMBOL_BELL);
        lv_obj_set_style_text_font(bell, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bell, lv_color_hex(alarm_color), 0);
        lv_obj_align(bell, LV_ALIGN_TOP_LEFT, 12, 12);

        lv_obj_t *type_label = lv_label_create(visual_alarm_container);
        lv_label_set_text(type_label, alarm_type);
        lv_obj_set_style_text_font(type_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(type_label, lv_color_hex(alarm_color), 0);
        lv_obj_align(type_label, LV_ALIGN_TOP_LEFT, 40, 14);

        // ---- Divider ----
        lv_obj_t *div = lv_obj_create(visual_alarm_container);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, 270, 1);
        lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

        // ---- Glucose value (centered, large) ----
        visual_alarm_glucose_label = lv_label_create(visual_alarm_container);
        char glucose_text[16];
        snprintf(glucose_text, sizeof(glucose_text), "%d", current_glucose);
        lv_label_set_text(visual_alarm_glucose_label, glucose_text);
        lv_obj_set_style_text_font(visual_alarm_glucose_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(visual_alarm_glucose_label, lv_color_hex(alarm_color), 0);
        lv_obj_align(visual_alarm_glucose_label, LV_ALIGN_CENTER, 0, -24);

        // ---- Unit label ----
        lv_obj_t *unit_label = lv_label_create(visual_alarm_container);
        lv_label_set_text(unit_label, "mg/dL");
        lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(unit_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align_to(unit_label, visual_alarm_glucose_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

        // ---- Hold-to-disarm button (hold 1.5s to dismiss) ----
        visual_alarm_disarm_btn = lv_obj_create(visual_alarm_container);
        lv_obj_remove_style_all(visual_alarm_disarm_btn);
        lv_obj_set_size(visual_alarm_disarm_btn, 260, 40);
        lv_obj_align(visual_alarm_disarm_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(visual_alarm_disarm_btn, lv_color_hex(0x1a2332), 0);
        lv_obj_set_style_bg_opa(visual_alarm_disarm_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(visual_alarm_disarm_btn, 10, 0);
        lv_obj_set_style_border_width(visual_alarm_disarm_btn, 1, 0);
        lv_obj_set_style_border_color(visual_alarm_disarm_btn, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_opa(visual_alarm_disarm_btn, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(visual_alarm_disarm_btn, 0, 0);
        lv_obj_clear_flag(visual_alarm_disarm_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(visual_alarm_disarm_btn, LV_OBJ_FLAG_CLICKABLE);

        // Progress fill bar (grows from left as user holds)
        visual_alarm_disarm_progress = lv_obj_create(visual_alarm_disarm_btn);
        lv_obj_remove_style_all(visual_alarm_disarm_progress);
        lv_obj_set_size(visual_alarm_disarm_progress, 0, 32);  // Starts at 0 width
        lv_obj_align(visual_alarm_disarm_progress, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_color(visual_alarm_disarm_progress, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(visual_alarm_disarm_progress, LV_OPA_30, 0);
        lv_obj_set_style_radius(visual_alarm_disarm_progress, 8, 0);
        lv_obj_clear_flag(visual_alarm_disarm_progress, LV_OBJ_FLAG_CLICKABLE);

        // Button label
        visual_alarm_disarm_label = lv_label_create(visual_alarm_disarm_btn);
        lv_label_set_text(visual_alarm_disarm_label, "Hold to Disarm");
        lv_obj_set_style_text_font(visual_alarm_disarm_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(visual_alarm_disarm_label, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(visual_alarm_disarm_label);

        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESS_LOST, NULL);

        lvgl_port_unlock();
    }

    // Pulse timer — fast for urgent alarms (200ms), moderate for warnings (350ms)
    uint32_t pulse_ms = (current_alarm_state == ALARM_STATE_HIGH_ALARM ||
                         current_alarm_state == ALARM_STATE_LOW_ALARM) ? 200 : 350;
    visual_alarm_timer = lv_timer_create(visual_alarm_pulse_timer_cb, pulse_ms, NULL);
}

// Stop visual alarm
static void stop_visual_alarm(void) {
    if (!visual_alarm_active) {
        return;
    }

    ESP_LOGI(TAG, "Stopping visual alarm");
    visual_alarm_active = false;

    // Restore saved brightness
    screen_brightness_percent = saved_brightness_percent;
    display_set_brightness(saved_brightness_percent);
    ESP_LOGI(TAG, "Brightness restored to %d%%", saved_brightness_percent);

    // Stop LED alarm flash
    led_stop_alarm_flash();

    if (visual_alarm_timer != NULL) {
        lv_timer_del(visual_alarm_timer);
        visual_alarm_timer = NULL;
    }

    if (lvgl_port_lock(0)) {
        if (visual_alarm_overlay != NULL) {
            lv_obj_del(visual_alarm_overlay);
            visual_alarm_overlay = NULL;
        }
        if (visual_alarm_container != NULL) {
            // Deleting container also deletes slider and label (they are children)
            lv_obj_del(visual_alarm_container);
            visual_alarm_container = NULL;
            visual_alarm_disarm_btn = NULL;
            visual_alarm_disarm_progress = NULL;
            visual_alarm_disarm_label = NULL;
            visual_alarm_glucose_label = NULL;
        }
        lvgl_port_unlock();
    }
}

// Update glucose value shown on the visual alarm card (keeps it current between fetches)
static void update_visual_alarm_glucose(int glucose_mg_dl) {
    if (!visual_alarm_active || visual_alarm_glucose_label == NULL) return;
    if (lvgl_port_lock(0)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", glucose_mg_dl);
        lv_label_set_text(visual_alarm_glucose_label, buf);
        lvgl_port_unlock();
    }
}

// Update glucose display on home screen
void update_glucose_display(void) {
    // Check if widgets exist (home screen is loaded)
    if (label_glucose == NULL || trend_canvas == NULL || label_time_ago == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        if (glucose_data_valid) {
            // Calculate data age first - this determines display behavior
            time_t now;
            time(&now);
            int minutes_ago = (int)difftime(now, glucose_timestamp) / 60;

            // SAFETY CRITICAL: If data is >15 minutes old, DO NOT show the number
            // Showing stale glucose can be life-threatening (e.g., showing 181 when actual is 52)
            if (minutes_ago > 15) {
                // Data is dangerously stale - hide the number to prevent false confidence
                lv_label_set_text(label_glucose, "---");
                lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Clear arrow
                lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_RED), 0);

                char stale_str[32];
                if (minutes_ago >= 60) {
                    snprintf(stale_str, sizeof(stale_str), "STALE %dh %dm ago", minutes_ago / 60, minutes_ago % 60);
                } else {
                    snprintf(stale_str, sizeof(stale_str), "STALE %d mins ago", minutes_ago);
                }
                lv_label_set_text(label_time_ago, stale_str);
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_RED), 0);

                // Throttle this warning to once per minute (avoid log spam every second)
                static int last_logged_stale_minute = -1;
                if (minutes_ago != last_logged_stale_minute) {
                    last_logged_stale_minute = minutes_ago;
                    ESP_LOGW(TAG, "Glucose data is %d minutes old - hiding value for safety", minutes_ago);
                }
            } else {
                // Data is recent enough to display
                char glucose_str[16];
                snprintf(glucose_str, sizeof(glucose_str), "%d", current_glucose);
                lv_label_set_text(label_glucose, glucose_str);

                // Realign unit label when expanded (stays below glucose center)
                if (cgm_expanded) {
                    if (label_unit != NULL) {
                        lv_obj_align_to(label_unit, label_glucose, LV_ALIGN_OUT_BOTTOM_RIGHT, 20, -4);
                    }
                }

                // Color code based on alarm thresholds (configured by user)
                uint32_t alarm_color = get_glucose_alarm_color(current_glucose);
                lv_obj_set_style_text_color(label_glucose, lv_color_hex(alarm_color), 0);

                // Update trend arrow canvas (80px when expanded, 60px normal)
                lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
                if (cgm_expanded) {
                    draw_trend_arrow_sized(trend_canvas, current_trend, 80);
                } else {
                    draw_trend_arrow(trend_canvas, current_trend);
                }

                char time_ago_str[32];
                if (minutes_ago < 1) {
                    snprintf(time_ago_str, sizeof(time_ago_str), "Just now");
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
                } else if (minutes_ago == 1) {
                    snprintf(time_ago_str, sizeof(time_ago_str), "1 min ago");
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
                } else if (minutes_ago >= 10) {
                    // Getting stale - show warning
                    snprintf(time_ago_str, sizeof(time_ago_str), "Last: %d mins ago", minutes_ago);
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_RED), 0);
                } else if (!glucose_data_fresh) {
                    // Data is not fresh (last fetch failed) - indicate staleness
                    snprintf(time_ago_str, sizeof(time_ago_str), "Last: %d mins ago", minutes_ago);
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_ORANGE), 0);
                } else {
                    // Fresh data - normal display
                    snprintf(time_ago_str, sizeof(time_ago_str), "%d mins ago", minutes_ago);
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
                }
                lv_label_set_text(label_time_ago, time_ago_str);
            }

            ESP_LOGD(TAG, "Glucose display: %d mg/dL, %d mins ago, fresh: %s",
                     current_glucose, minutes_ago, glucose_data_fresh ? "yes" : "no");
        } else {
            // No data available (never received or not authenticated) - show status-specific message
            lv_label_set_text(label_glucose, "---");
            lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Clear arrow

            if (sensor_change_mode) {
                // User confirmed sensor change — show friendly message
                lv_label_set_text(label_time_ago, "Sensor Change");
                lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ORANGE), 0);
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_ORANGE), 0);
            } else {
                switch (glucose_status) {
                    case GLUCOSE_STATUS_WARMUP:
                        lv_label_set_text(label_time_ago, "Sensor warmup");
                        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ORANGE), 0);
                        lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_ORANGE), 0);
                        break;

                    case GLUCOSE_STATUS_SIGNAL_LOSS:
                        lv_label_set_text(label_time_ago, "Signal loss");
                        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_RED), 0);
                        lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_RED), 0);
                        break;

                    case GLUCOSE_STATUS_NOT_AUTHENTICATED:
                        lv_label_set_text(label_time_ago, "Not logged in");
                        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_TEXT_DIM), 0);
                        lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_DIM), 0);
                        break;

                    case GLUCOSE_STATUS_NO_DATA:
                    default:
                        lv_label_set_text(label_time_ago, "No Data");
                        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_RED), 0);
                        lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_RED), 0);
                        break;
                }
            }
        }
        lvgl_port_unlock();
    }
}

// Helper: Delete background tasks to free their heap-allocated stacks for TLS.
// vTaskDelete + idle task reclaim frees ~11KB contiguous heap needed for TLS
// handshake. Also stops tasks from competing for the network mutex.
// Returns true if any tasks were deleted.
bool delete_background_tasks_for_ssl(const char *reason) {
    bool deleted_any = false;

    if (weather_task_handle != NULL) {
        vTaskDelete(weather_task_handle);
        weather_task_handle = NULL;
        deleted_any = true;
    }
    if (time_task_handle != NULL) {
        vTaskDelete(time_task_handle);
        time_task_handle = NULL;
        deleted_any = true;
    }
    if (battery_task_handle != NULL) {
        vTaskDelete(battery_task_handle);
        battery_task_handle = NULL;
        deleted_any = true;
    }

    if (deleted_any) {
        // Yield so idle task can reclaim deleted task stacks (~11KB freed from heap).
        // The 100ms gives the idle task time to run on both cores.
        vTaskDelay(pdMS_TO_TICKS(100));

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "Tasks stopped: %s (free=%lu largest=%lu)", reason, free_heap, largest);
        sd_log(TAG, "Tasks stopped: %s free=%lu largest=%lu", reason, free_heap, largest);
    }

    return deleted_any;
}

// Helper: Recreate background tasks after TLS operation completes.
// Heap-allocated stacks so deletion frees contiguous memory for TLS reconnection.
void recreate_background_tasks(void) {
    if (weather_task_handle == NULL) {
        xTaskCreatePinnedToCore(weather_update_task, "weather_update",
            WEATHER_STACK_SIZE, NULL, 3, &weather_task_handle, 0);
    }
    if (time_task_handle == NULL) {
        xTaskCreatePinnedToCore(time_update_task, "time_update",
            TIME_STACK_SIZE, NULL, 3, &time_task_handle, tskNO_AFFINITY);
    }
    if (battery_task_handle == NULL) {
        xTaskCreatePinnedToCore(battery_monitor_task, "battery_mon",
            BATTERY_STACK_SIZE, NULL, 2, &battery_task_handle, 0);
    }
    ESP_LOGI(TAG, "Tasks recreated (heap=%lu)", esp_get_free_heap_size());
}

// ============================================================================
// CGM Provider Dispatch Helpers
// ============================================================================

static inline bool cgm_is_authenticated(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) return nightscout_is_authenticated();
    if (p == CGM_PROVIDER_LIBRE) return libre_is_authenticated();
    return dexcom_is_authenticated();
}

static inline bool cgm_session_needs_refresh(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) return nightscout_session_needs_refresh();
    if (p == CGM_PROVIDER_LIBRE) return libre_session_needs_refresh();
    return dexcom_session_needs_refresh();
}

static inline void cgm_close_client(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) nightscout_close_persistent_client();
    else if (p == CGM_PROVIDER_LIBRE) libre_close_persistent_client();
    else                              dexcom_close_persistent_client();
}

static inline bool cgm_client_is_open(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) return nightscout_persistent_client_is_open();
    if (p == CGM_PROVIDER_LIBRE) return libre_persistent_client_is_open();
    return dexcom_persistent_client_is_open();
}

static inline esp_err_t cgm_authenticate(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) {
        char url[128], token[64];
        if (nvs_get_nightscout_credentials(url, sizeof(url), token, sizeof(token)) != ESP_OK)
            return ESP_ERR_NOT_FOUND;
        return nightscout_authenticate(url, token);
    }
    if (p == CGM_PROVIDER_LIBRE) {
        char email[64], pass[64];
        if (nvs_get_libre_credentials(email, sizeof(email), pass, sizeof(pass)) != ESP_OK)
            return ESP_ERR_NOT_FOUND;
        return libre_authenticate(email, pass);
    }
    char user[64], pass[64];
    if (nvs_get_dexcom_credentials(user, sizeof(user), pass, sizeof(pass)) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    return dexcom_authenticate(user, pass);
}

static inline esp_err_t cgm_fetch(cgm_provider_t p, cgm_glucose_t *g) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) return nightscout_fetch_glucose(g);
    if (p == CGM_PROVIDER_LIBRE) return libre_fetch_glucose(g);
    return dexcom_fetch_glucose(g);
}

static inline bool cgm_needs_tls(cgm_provider_t p) {
    if (p == CGM_PROVIDER_NIGHTSCOUT) return nightscout_uses_https();
    return true;  // Dexcom and Libre always use HTTPS
}

// Glucose update task - fetches glucose periodically (every 90 seconds)
void glucose_update_task(void *pvParameters) {
    // Determine active CGM provider once at task start
    char cgm_type[MAX_CGM_TYPE_LEN] = "dexcom";
    nvs_load_cgm_type(cgm_type, sizeof(cgm_type));
    cgm_provider_t provider = CGM_PROVIDER_DEXCOM;
    if (strcmp(cgm_type, "libre") == 0) provider = CGM_PROVIDER_LIBRE;
    else if (strcmp(cgm_type, "nightscout") == 0) provider = CGM_PROVIDER_NIGHTSCOUT;

    ESP_LOGI(TAG, "Glucose update task started (90s interval, provider=%s)", cgm_type);

    // Exponential backoff for persistent failures (API outage, signal loss)
    // Normal: 90s.  After 3+ failures: 180s.  After 6+: 300s.  After 10+: 600s.
    int consecutive_task_failures = 0;
    int poll_interval_sec = 90;

    // Smart wait: Only wait remaining time if an initial fetch already happened
    const int64_t FETCH_INTERVAL_MS = 90000;  // 90 seconds
    int64_t wait_time_ms = FETCH_INTERVAL_MS;

    if (last_glucose_fetch_time > 0) {
        // Calculate time since last fetch
        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last_glucose_fetch_time;

        if (elapsed < FETCH_INTERVAL_MS) {
            wait_time_ms = FETCH_INTERVAL_MS - elapsed;
            ESP_LOGI(TAG, "Last fetch was %lu ms ago, waiting %lu ms until next fetch", (unsigned long)elapsed, (unsigned long)wait_time_ms);
        } else {
            ESP_LOGI(TAG, "Last fetch was %lu ms ago (>90s), fetching immediately", (unsigned long)elapsed);
            wait_time_ms = 0;
        }
    } else {
        ESP_LOGI(TAG, "No previous fetch detected, waiting full 90 seconds");
    }

    // Initial wait - check every second for force-fetch
    if (wait_time_ms > 0) {
        int wait_seconds = wait_time_ms / 1000;
        for (int i = 0; i < wait_seconds; i++) {
            if (glucose_force_fetch_requested) {
                ESP_LOGI(TAG, "Force-fetch requested during initial wait, breaking early");
                glucose_force_fetch_requested = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    while (1) {
        // Wait for poll_interval_sec OR until force-fetch is requested
        // Check every 1 second to allow responsive force-fetch
        for (int i = 0; i < poll_interval_sec; i++) {
            if (glucose_force_fetch_requested) {
                ESP_LOGI(TAG, "Force-fetch requested, breaking wait loop early");
                glucose_force_fetch_requested = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));  // Check every 1 second
        }

        // SAFETY: Glucose fetching ALWAYS runs when WiFi is connected.
        // Data freshness is life-critical — never skip just because user
        // navigated to settings, alarms, or any other screen.
        // Only UI updates (display, overlays) are gated on home_screen_active.

        // Check if WiFi is connected
        if (!wifi_connected) {
            ESP_LOGI(TAG, "[Glucose Task] WiFi not connected - waiting 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        // Auto-re-auth: if authentication was lost (WiFi drop, session expiry,
        // failed re-auth during API outage), try again using stored credentials.
        // CRITICAL: Must delete background tasks first — TLS handshake needs ~16KB
        // contiguous heap, but running tasks fragment it to ~3KB largest block.
        bool is_auth = cgm_is_authenticated(provider);
        if (!is_auth) {
            ESP_LOGW(TAG, "[Glucose Task] Not authenticated — attempting re-auth (backoff=%ds)",
                     poll_interval_sec);
            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                // Free heap for TLS handshake (skip for HTTP-mode Nightscout)
                bool tasks_deleted = cgm_needs_tls(provider) ?
                    delete_background_tasks_for_ssl("re-auth") : false;
                cgm_close_client(provider);  // Clean slate for fresh handshake

                esp_err_t auth_ret = cgm_authenticate(provider);
                if (auth_ret == ESP_OK) {
                    ESP_LOGI(TAG, "[Glucose Task] Re-auth succeeded — resuming glucose fetch");
                    sd_log(TAG, "Auto re-auth OK after WiFi recovery");
                    is_auth = true;
                    consecutive_task_failures = 0;
                    poll_interval_sec = 90;
                    // Close SSL from auth — glucose fetch will reopen
                    cgm_close_client(provider);
                } else if (auth_ret == ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "[Glucose Task] No stored credentials for %s", cgm_type);
                } else {
                    ESP_LOGW(TAG, "[Glucose Task] Re-auth failed — will retry in %ds", poll_interval_sec);
                    sd_log(TAG, "Auto re-auth failed, retry in %ds", poll_interval_sec);
                    consecutive_task_failures++;
                    if (consecutive_task_failures >= 10) {
                        poll_interval_sec = 600;
                    } else if (consecutive_task_failures >= 6) {
                        poll_interval_sec = 300;
                    } else if (consecutive_task_failures >= 3) {
                        poll_interval_sec = 180;
                    }
                }
                // Always recreate tasks — whether auth succeeded or failed
                if (tasks_deleted) {
                    recreate_background_tasks();
                }
                xSemaphoreGive(network_mutex);
            }

            if (!is_auth) continue;  // Try again next cycle
        }

        ESP_LOGI(TAG, "[Glucose Task] Auth OK, Free heap: %lu bytes", esp_get_free_heap_size());

        // Proactive session refresh: handle BEFORE entering fetch path so the
        // inline refresh inside dexcom_fetch_glucose() never triggers under
        // low-memory conditions. We control task deletion here.
        if (is_auth && cgm_session_needs_refresh(provider)) {
            ESP_LOGI(TAG, "Proactive session refresh — handling at task level");
            sd_log(TAG, "Proactive session refresh");

            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                bool tasks_deleted = delete_background_tasks_for_ssl("session-refresh");
                cgm_close_client(provider);

                esp_err_t ref_ret = cgm_authenticate(provider);
                if (ref_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Proactive refresh OK — keeping fresh client alive");
                    sd_log(TAG, "Proactive refresh OK");
                    // Don't close client — glucose fetch will reuse it
                } else {
                    ESP_LOGW(TAG, "Proactive refresh failed: %s", esp_err_to_name(ref_ret));
                    sd_log(TAG, "Proactive refresh failed: %s", esp_err_to_name(ref_ret));
                    is_auth = cgm_is_authenticated(provider);
                }

                if (tasks_deleted) {
                    recreate_background_tasks();
                }
                xSemaphoreGive(network_mutex);
            }

            if (!is_auth) continue;  // Auth lost during refresh — retry next cycle
        }

        // The initial boot SSL handshake fragments memory permanently (DYNAMIC_FREE
        // creates holes that LVGL/WiFi fill). Fresh handshakes need task deletion
        // to free ~11KB of stack memory for contiguous blocks.

        if (is_auth) {
            // CRITICAL: Take network mutex to ensure exclusive access
            ESP_LOGI(TAG, "Waiting for network mutex...");
            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                ESP_LOGI(TAG, "Network mutex acquired for glucose fetch");

                // Check if we have enough DRAM for SSL connection
                // MUST use MALLOC_CAP_DEFAULT (DRAM only) — NOT MALLOC_CAP_INTERNAL
                // INTERNAL includes ~27KB of IRAM that SSL can't use (byte-access required)
                size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
                size_t min_ever = esp_get_minimum_free_heap_size();

                // Compact memory report
                multi_heap_info_t heap_info;
                heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
                float fragmentation = 0;
                if (heap_info.total_free_bytes > 0) {
                    fragmentation = 100.0f * (1.0f - ((float)heap_info.largest_free_block / (float)heap_info.total_free_bytes));
                }
                ESP_LOGI(TAG, "MEM: free=%lu largest=%lu frag=%.0f%% min_ever=%lu",
                         free_internal, heap_info.largest_free_block, fragmentation, min_ever);
                if (fragmentation > 50.0f) {
                    ESP_LOGW(TAG, "Fragmented: %lu alloc_blocks, %lu free_blocks",
                             heap_info.allocated_blocks, heap_info.free_blocks);
                }
                if (min_ever < 10000) {
                    ESP_LOGW(TAG, "CRITICAL: min_ever=%lu — risk of crashes!", min_ever);
                }

                bool client_open = cgm_client_is_open(provider);
                bool tasks_deleted_for_ssl = false;

                // SD log memory state every cycle
                sd_log(TAG, "MEM free=%lu largest=%lu frag=%.1f%% blocks=%lu/%lu",
                       heap_info.total_free_bytes, heap_info.largest_free_block,
                       fragmentation, heap_info.allocated_blocks, heap_info.free_blocks);

                if (!client_open && cgm_needs_tls(provider)) {
                    // Fresh TLS handshake needs ~10KB contiguous heap.
                    // The initial boot handshake fragments memory permanently
                    // (DYNAMIC_FREE creates holes that LVGL/WiFi fill). Delete
                    // background task stacks (~11KB) to create large enough blocks.
                    // Skipped for HTTP-mode Nightscout (no TLS needed).
                    tasks_deleted_for_ssl = delete_background_tasks_for_ssl("ssl-reconnect");
                }

                ESP_LOGI(TAG, "Proceeding with %s (free=%lu, largest=%lu)",
                         client_open ? "existing SSL" : "fresh SSL",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

                // Mark fetch as in progress
                glucose_fetch_in_progress = true;

                cgm_glucose_t glucose = {0};  // Initialize to prevent garbage data
                ESP_LOGI(TAG, "Fetching periodic glucose reading...");

                // Blink blue to indicate CGM poll starting
                led_blink_blue();

                esp_err_t ret = cgm_fetch(provider, &glucose);

                // Connection died mid-fetch (SSL was open but socket died).
                // Delete tasks if not already deleted, then retry.
                if (ret != ESP_OK && !glucose.valid && !cgm_client_is_open(provider)) {
                    if (!tasks_deleted_for_ssl && cgm_needs_tls(provider)) {
                        tasks_deleted_for_ssl = delete_background_tasks_for_ssl("ssl-reconnect");
                    }
                    ESP_LOGW(TAG, "Connection died mid-fetch — retrying with fresh connection");
                    sd_log(TAG, "SSL died mid-fetch, retry (tasks_del=%d)", tasks_deleted_for_ssl);

                    ESP_LOGI(TAG, "Retry: heap=%lu, largest=%lu",
                             esp_get_free_heap_size(),
                             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

                    memset(&glucose, 0, sizeof(glucose));
                    ret = cgm_fetch(provider, &glucose);
                }

            if (ret == ESP_OK && glucose.valid) {
                // Duplicate detection: skip if same timestamp as last stored reading
                if (glucose.timestamp == glucose_timestamp && glucose_data_valid) {
                    ESP_LOGI(TAG, "Duplicate reading (same timestamp) — skipping storage");
                    glucose_fetch_failed = false;
                    glucose_fetch_in_progress = false;
                    consecutive_task_failures = 0;
                    poll_interval_sec = 90;
                    last_glucose_fetch_time = esp_timer_get_time() / 1000;
                    led_show_success();
                } else {
                // Update glucose data - "Last Known Good" approach
                current_glucose = glucose.value;
                current_trend = glucose.trend;
                glucose_timestamp = glucose.timestamp;
                glucose_data_valid = true;    // We've received valid data at least once
                glucose_data_fresh = true;    // This fetch succeeded
                glucose_status = glucose.status;
                if (sensor_change_mode) {
                    sensor_change_mode = false;
                    ESP_LOGI(TAG, "Sensor change complete - receiving data again");
                }
                reset_nodata_overlay_cycle();  // Allow overlay again if data drops later

                // Update timer for progress bar
                last_glucose_fetch_time = esp_timer_get_time() / 1000;  // milliseconds

                ESP_LOGI(TAG, "Glucose updated: %d mg/dL, trend: %s",
                         glucose.value, cgm_trend_description(glucose.trend));

                // Add to history for trend chart (convert seconds to milliseconds)
                glucose_history_add(glucose.value, (int64_t)glucose.timestamp * 1000);

                // Mark fetch as successful
                glucose_fetch_failed = false;
                glucose_fetch_in_progress = false;

                // Reset backoff on success
                if (consecutive_task_failures > 0) {
                    ESP_LOGI(TAG, "Fetch succeeded after %d failures — resetting poll to 90s", consecutive_task_failures);
                }
                consecutive_task_failures = 0;
                poll_interval_sec = 90;

                // UI updates: only when on home screen (LVGL objects may not exist otherwise)
                if (home_screen_active) {
                    if (lvgl_port_lock(0)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_fetch_timer_bar != NULL) {
                            lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
                            lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                        }
                        lvgl_port_unlock();
                    }
                    update_glucose_display();
                }

                led_show_success();

                sd_log(TAG, "OK: %d mg/dL, %s, heap=%lu",
                       glucose.value, cgm_trend_description(glucose.trend),
                       esp_get_free_heap_size());
                if (sd_glucose_logging_enabled) {
                    sd_log_glucose(glucose.value, cgm_trend_description(glucose.trend));
                }

                // Play success sound on first glucose reading
                if (!first_glucose_received) {
                    first_glucose_received = true;
                    ESP_LOGI(TAG, "First glucose reading received - playing success sound");
                    play_success_sound();
                }

                // Alarms ALWAYS run regardless of active screen (life-safety)
                if (glucose_data_fresh) {
                    time_t alarm_now;
                    time(&alarm_now);
                    int data_age_mins = (int)difftime(alarm_now, glucose_timestamp) / 60;
                    if (data_age_mins <= 10) {
                        check_glucose_alarms(current_glucose);
                        update_visual_alarm_glucose(current_glucose);
                    } else {
                        ESP_LOGW(TAG, "Skipping alarm check - data is %d mins old", data_age_mins);
                    }
                }
                } // end of non-duplicate block
            } else if (ret == ESP_OK) {
                // ── STALE DATA PATH ──
                // API responded successfully but sensor has no fresh reading.
                // The SSL connection is healthy — do NOT close it or apply backoff.
                glucose_data_fresh = false;
                glucose_status = glucose.status;

                // Progress bar: mark that we just talked to the API
                last_glucose_fetch_time = esp_timer_get_time() / 1000;

                // Connection works — do NOT trigger SSL close or backoff
                glucose_fetch_failed = false;
                glucose_fetch_in_progress = false;
                poll_interval_sec = 90;  // Keep normal cadence

                // Safety net: if data is >30 minutes old, force invalid display
                if (glucose_data_valid && glucose_timestamp > 0) {
                    time_t stale_now;
                    time(&stale_now);
                    int stale_mins = (int)difftime(stale_now, glucose_timestamp) / 60;
                    if (stale_mins > 30) {
                        ESP_LOGW(TAG, "SAFETY: Data is %d mins old - forcing display to ---", stale_mins);
                        glucose_data_valid = false;
                        glucose_status = GLUCOSE_STATUS_SIGNAL_LOSS;
                    }
                }

                // UI updates: only when on home screen
                if (home_screen_active) {
                    // Stop animation and set bar to orange (stale, not failed)
                    if (lvgl_port_lock(0)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_fetch_timer_bar != NULL) {
                            lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
                            lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                        }
                        lvgl_port_unlock();
                    }

                    update_glucose_display();

                    // Show no-data overlay if signal lost
                    if (!sensor_change_mode && wifi_connected &&
                        (glucose.status == GLUCOSE_STATUS_NO_DATA ||
                         glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS)) {
                        if (lvgl_port_lock(200)) {
                            show_nodata_overlay();
                            lvgl_port_unlock();
                        }
                    }
                }

                // Brief amber LED (not error red)
                led_blink_blue();

                {
                    const char *sts = (glucose.status == GLUCOSE_STATUS_NO_DATA) ? "NO_DATA" :
                                      (glucose.status == GLUCOSE_STATUS_WARMUP) ? "WARMUP" :
                                      (glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS) ? "SIGNAL_LOSS" :
                                      "STALE";
                    ESP_LOGW(TAG, "Stale data from API: status=%s, heap=%lu",
                             sts, esp_get_free_heap_size());
                    sd_log(TAG, "STALE: status=%s, heap=%lu", sts, esp_get_free_heap_size());
                }
            } else {
                // ── ACTUAL FAILURE PATH ──
                // Connection error, auth failure, etc. SSL may be broken.
                glucose_data_fresh = false;
                glucose_status = glucose.status;

                // Clear glucose_data_valid if auth failed
                if (glucose.status == GLUCOSE_STATUS_NOT_AUTHENTICATED) {
                    glucose_data_valid = false;
                }
                // Safety net: if data is >30 minutes old, force invalid
                if (glucose_data_valid && glucose_timestamp > 0) {
                    time_t stale_now;
                    time(&stale_now);
                    int stale_mins = (int)difftime(stale_now, glucose_timestamp) / 60;
                    if (stale_mins > 30) {
                        ESP_LOGW(TAG, "SAFETY: Data is %d mins old - forcing display to ---", stale_mins);
                        glucose_data_valid = false;
                        glucose_status = GLUCOSE_STATUS_SIGNAL_LOSS;
                    }
                }

                // Mark fetch as failed
                glucose_fetch_failed = true;
                glucose_fetch_in_progress = false;

                // UI updates: only when on home screen
                if (home_screen_active) {
                    // Stop animation and update bar to red (but keep blue glow)
                    if (lvgl_port_lock(0)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_fetch_timer_bar != NULL) {
                            lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
                            lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                            lv_obj_set_style_shadow_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
                        }
                        lvgl_port_unlock();
                    }

                    update_glucose_display();

                    // Show no-data overlay if signal lost
                    if (!sensor_change_mode && wifi_connected &&
                        (glucose.status == GLUCOSE_STATUS_NO_DATA ||
                         glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS)) {
                        if (lvgl_port_lock(200)) {
                            show_nodata_overlay();
                            lvgl_port_unlock();
                        }
                    }
                }

                ESP_LOGW(TAG, "Periodic glucose fetch failed: %s", esp_err_to_name(ret));

                // Show error LED (red blink)
                led_start_error_blink();
                vTaskDelay(pdMS_TO_TICKS(5000));  // Blink for 5 seconds
                led_stop_error_blink();

                {
                    const char *sts = (glucose.status == GLUCOSE_STATUS_OK) ? "OK" :
                                      (glucose.status == GLUCOSE_STATUS_NO_DATA) ? "NO_DATA" :
                                      (glucose.status == GLUCOSE_STATUS_WARMUP) ? "WARMUP" :
                                      (glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS) ? "SIGNAL_LOSS" :
                                      "NOT_AUTH";
                    sd_log(TAG, "FAIL: %s, status=%s, heap=%lu",
                           esp_err_to_name(ret), sts, esp_get_free_heap_size());
                }

                // Exponential backoff: reduce polling frequency during persistent failures.
                // Saves SSL overhead and battery. Resets on first success.
                consecutive_task_failures++;
                if (consecutive_task_failures >= 10) {
                    poll_interval_sec = 600;   // 10 min cap
                } else if (consecutive_task_failures >= 6) {
                    poll_interval_sec = 300;   // 5 min
                } else if (consecutive_task_failures >= 3) {
                    poll_interval_sec = 180;   // 3 min
                } else {
                    poll_interval_sec = 90;    // Normal
                }

                // SIGNAL_LOSS specifically: sensor is offline, data won't change quickly.
                // Use 5-min minimum (no point hammering API for known-stale readings).
                if (glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS && poll_interval_sec < 300) {
                    poll_interval_sec = 300;
                }

                if (poll_interval_sec > 90) {
                    ESP_LOGW(TAG, "Backoff active: next poll in %d seconds (failures=%d)",
                             poll_interval_sec, consecutive_task_failures);
                    sd_log(TAG, "Backoff: %ds (failures=%d)", poll_interval_sec, consecutive_task_failures);
                }
            }

                // SSL lifecycle strategy per provider:
                // - Dexcom/Libre: keep alive on success (connections survive minutes-hours)
                // - Nightscout HTTPS: always close (Cloudflare sends large TLS records
                //   that need ~2KB contiguous for the read buffer; post-reconnect
                //   fragmentation drops largest_block to ~1.9KB, causing read failures)
                // - Nightscout HTTP: keep alive (no TLS buffers)
                bool always_close = (provider == CGM_PROVIDER_NIGHTSCOUT && cgm_needs_tls(provider));

                if (glucose_fetch_failed || always_close) {
                    cgm_close_client(provider);
                    if (glucose_fetch_failed) {
                        sd_log(TAG, "SSL closed (failed), heap=%lu", esp_get_free_heap_size());
                        sd_logger_resume();
                        sd_logger_flush();
                    }
                    ESP_LOGI(TAG, "SSL closed after fetch (heap=%lu)", esp_get_free_heap_size());
                } else {
                    ESP_LOGI(TAG, "Keeping SSL alive for reuse (heap=%lu)", esp_get_free_heap_size());
                }

                // Device heartbeat — throttled to once per 15 min
                {
                    static int64_t last_heartbeat_ms = 0;
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - last_heartbeat_ms >= 15 * 60 * 1000) {
                        heartbeat_send();
                        last_heartbeat_ms = now_ms;
                    }
                }

                // Recreate tasks deleted for SSL handshake
                if (tasks_deleted_for_ssl) {
                    recreate_background_tasks();
                }

                // Release network mutex
                xSemaphoreGive(network_mutex);
                ESP_LOGI(TAG, "Network mutex released after glucose fetch");
            } else {
                ESP_LOGE(TAG, "Failed to acquire network mutex - this should never happen!");
            }
        } else {
            ESP_LOGW(TAG, "Not authenticated - skipping glucose fetch");
        }

        // ── Automatic firmware update check (deferred, every 24h) ──
        // Only after 5 min uptime + first glucose received + home screen active.
        // Version check is HTTP (no TLS) — coexists with live CGM SSL client.
        if (first_glucose_received && home_screen_active && update_should_check()) {
            ESP_LOGI(TAG, "Automatic firmware update check...");
            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                // HTTP version check coexists with live SSL — no need to close CGM client.
                // The network_mutex serializes I/O; multiple open sockets are fine.
                esp_err_t chk = update_check_now();
                xSemaphoreGive(network_mutex);

                if (chk == ESP_OK) {
                    const update_info_t *info = update_get_info();
                    if (info->available) {
                        ESP_LOGI(TAG, "Update available: v%s — showing overlay", info->version);
                        if (lvgl_port_lock(200)) {
                            show_update_overlay();
                            lvgl_port_unlock();
                        }
                    }
                }
            }
        }

        // ── Heap health watchdog ──
        {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            static int critical_heap_count = 0;

            if (largest < 1024) {
                critical_heap_count++;
                ESP_LOGE(TAG, "HEAP CRITICAL: largest=%lu (strike %d/3)",
                         (unsigned long)largest, critical_heap_count);
                sd_log(TAG, "HEAP CRITICAL: largest=%lu (%d/3)",
                       (unsigned long)largest, critical_heap_count);
                sd_logger_flush();

                if (critical_heap_count >= 3) {
                    ESP_LOGE(TAG, "Heap irrecoverable — rebooting");
                    sd_log(TAG, "REBOOT: heap fragmentation recovery");
                    sd_logger_flush();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            } else {
                critical_heap_count = 0;
            }
        }

        // ── Stack high water mark monitoring ──
        {
            TaskHandle_t w = weather_task_handle, t = time_task_handle, b = battery_task_handle;
            if (w || t || b) {
                ESP_LOGI(TAG, "STACK HWM (bytes free): weather=%lu time=%lu battery=%lu",
                         w ? (unsigned long)(uxTaskGetStackHighWaterMark(w) * sizeof(StackType_t)) : 0,
                         t ? (unsigned long)(uxTaskGetStackHighWaterMark(t) * sizeof(StackType_t)) : 0,
                         b ? (unsigned long)(uxTaskGetStackHighWaterMark(b) * sizeof(StackType_t)) : 0);
            }
        }

        // Wait loop is at the beginning of while(1) - checks every second for force-fetch
    }
}

// ==================== LED PWM Functions ====================

// LED functions moved to hardware/led.c

// ==================== Buzzer/Speaker PWM Functions ====================
// Moved to hardware/buzzer.c

// ==================== Battery Monitoring ====================
// Moved to hardware/battery.c

// ==================== Display/Touch/CardKB/LVGL Initialization ====================
// Moved to hardware/display.c



// ==================== UI Screens ====================

// Global styles (accessible from UI modules)
lv_style_t style_bg;
lv_style_t style_card;

void init_styles(void) {
    // Background style - pure black
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(COLOR_BG));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    
    // Card style - clean dark blue-grey
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(COLOR_CARD_BG));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 10);
}

// ===== EVENT HANDLERS =====

// Forward declarations for functions remaining in main.c
void temp_toggle_event_cb(lv_event_t *e);

// Event handler for tapping temperature label to toggle Celsius/Fahrenheit
void temp_toggle_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        // Toggle temperature unit
        user_temp_celsius = !user_temp_celsius;
        ESP_LOGI(TAG, "Temperature unit toggled to %s", user_temp_celsius ? "Celsius" : "Fahrenheit");

        // Save to NVS
        weather_settings_t settings;
        if (nvs_load_weather_settings(&settings) == ESP_OK) {
            nvs_save_weather_settings(settings.zipcode, user_temp_celsius, settings.update_interval_min);
        }

        // Update settings screen label if provided
        lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
        if (label != NULL) {
            lv_label_set_text(label, user_temp_celsius ? "Celsius" : "Fahrenheit");
        }

        // Update weather display
        update_weather_display();
    }
}

void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");

        // Pause background tasks when leaving home screen
        home_screen_active = false;
        pause_background_tasks = true;
        dismiss_nodata_overlay();
        dismiss_wifi_disconnected_overlay();
        ESP_LOGI(TAG, "Background tasks paused (leaving home screen)");

        if (lvgl_port_lock(0)) {
            // MEMORY OPTIMIZATION: Create menu screen on-demand if it doesn't exist
            if (screen_menu == NULL) {
                ESP_LOGI(TAG, "Creating menu screen on-demand (freed previously)");
                create_menu_screen();
                ESP_LOGI(TAG, "Menu screen created. Free heap: %lu bytes", esp_get_free_heap_size());
            }
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}

void wifi_icon_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "WiFi icon clicked - launching WiFi setup...");

        // Pause background tasks when leaving home screen
        home_screen_active = false;
        pause_background_tasks = true;
        dismiss_nodata_overlay();
        dismiss_wifi_disconnected_overlay();
        ESP_LOGI(TAG, "Background tasks paused (leaving home screen)");

        // Close CGM persistent client to free ~30KB memory for WiFi scan/UI
        ESP_LOGI(TAG, "Closing CGM persistent client before WiFi scan");
        dexcom_close_persistent_client();
        libre_close_persistent_client();
        ESP_LOGI(TAG, "Free heap after closing CGM client: %lu bytes (internal: %lu)",
                 esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        // Recreate WiFi list screen fresh (delete old one if exists)
        if (lvgl_port_lock(0)) {
            if (screen_wifi_list != NULL) {
                lv_obj_del(screen_wifi_list);
                screen_wifi_list = NULL;
            }
            create_wifi_list_screen();
            lv_scr_load(screen_wifi_list);
            lvgl_port_unlock();
        }

        // Show scanning overlay on top of WiFi list screen
        show_wifi_scanning_overlay();

        // Start WiFi scan in background task
        xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
    }
}

void back_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back button clicked - returning to home screen");

        // Resume background tasks when returning to home screen
        home_screen_active = true;
        pause_background_tasks = false;
        ESP_LOGI(TAG, "Background tasks resumed (returned to home screen)");

        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_home);
            lvgl_port_unlock();
        }

        // Small delay to let screen transition complete
        vTaskDelay(pdMS_TO_TICKS(100));

        // MEMORY OPTIMIZATION: Delete menu screen after returning to home to free memory
        if (lvgl_port_lock(0)) {
            if (screen_menu != NULL) {
                ESP_LOGI(TAG, "Deleting menu screen to free memory");
                lv_obj_del(screen_menu);
                screen_menu = NULL;
                ESP_LOGI(TAG, "Menu screen deleted. Free heap: %lu bytes", esp_get_free_heap_size());
            }
            lvgl_port_unlock();
        }
    }
}

// Event handler for tapping time label to toggle 12hr/24hr format
void clock_label_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Clock label clicked - toggling time format");

        // Toggle format
        user_24hr_format = !user_24hr_format;

        // Save to NVS
        esp_err_t ret = nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Time format saved: %s", user_24hr_format ? "24hr" : "12hr");
        } else {
            ESP_LOGW(TAG, "Failed to save time format to NVS");
        }

        // Update settings screen label if provided
        lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
        if (label != NULL) {
            lv_label_set_text(label, user_24hr_format ? "24 Hour" : "12 Hour");
        }

        // Update display immediately
        update_time_display();

        // Also update sunrise/sunset times to reflect new format
        update_sunrise_sunset_display();
    }
}

// Animation callback wrapper for lv_bar_set_value
void menu_item_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int item_index = (int)(intptr_t)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Menu item %d clicked", item_index);

        switch (item_index) {
            case 0:  // CGM Settings
                ESP_LOGI(TAG, "Launching CGM Settings...");
                if (lvgl_port_lock(0)) {
                    if (screen_cgm_menu != NULL) {
                        lv_obj_del(screen_cgm_menu);
                        screen_cgm_menu = NULL;
                    }
                    create_cgm_menu_screen();
                    lv_scr_load(screen_cgm_menu);
                    // Single-screen rule: delete menu after loading sub-screen
                    if (screen_menu != NULL) {
                        lv_obj_del(screen_menu);
                        screen_menu = NULL;
                    }
                    lvgl_port_unlock();
                }
                break;
            case 1:  // WiFi Setup
                ESP_LOGI(TAG, "Launching WiFi setup...");

                // Close CGM persistent client to free ~30KB memory for WiFi scan/UI
                ESP_LOGI(TAG, "Closing CGM persistent client before WiFi scan");
                dexcom_close_persistent_client();
                libre_close_persistent_client();
                ESP_LOGI(TAG, "Free heap after closing CGM client: %lu bytes (internal: %lu)",
                         esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

                // Load WiFi list screen (create if needed)
                if (lvgl_port_lock(0)) {
                    if (screen_wifi_list != NULL) {
                        lv_obj_del(screen_wifi_list);
                        screen_wifi_list = NULL;
                    }
                    create_wifi_list_screen();
                    lv_scr_load(screen_wifi_list);
                    // Single-screen rule: delete menu after loading sub-screen
                    if (screen_menu != NULL) {
                        lv_obj_del(screen_menu);
                        screen_menu = NULL;
                    }
                    lvgl_port_unlock();
                }

                // Show scanning overlay on top of WiFi list screen
                show_wifi_scanning_overlay();

                // Start WiFi scan in background task - overlay will be hidden when done
                xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
                break;
            case 2:  // Time/Date/Weather Settings (combined)
                ESP_LOGI(TAG, "Launching Time/Weather settings...");
                if (lvgl_port_lock(0)) {
                    // Clean up any leftover search screen (keyboard = 40+ objects)
                    if (screen_zipcode_entry != NULL) {
                        lv_obj_del(screen_zipcode_entry);
                        screen_zipcode_entry = NULL;
                    }
                    create_time_weather_settings_screen();
                    lv_scr_load(screen_time_weather_settings);
                    // Single-screen rule: delete menu after loading sub-screen
                    if (screen_menu != NULL) {
                        lv_obj_del(screen_menu);
                        screen_menu = NULL;
                    }
                    lvgl_port_unlock();
                }
                break;
            case 3:  // Alarm Settings
                ESP_LOGI(TAG, "Launching Alarm settings...");
                if (screen_alarm_settings != NULL) {
                    lv_obj_del(screen_alarm_settings);
                    screen_alarm_settings = NULL;
                }
                create_alarm_settings_screen();
                if (screen_alarm_settings != NULL) {
                    lv_scr_load(screen_alarm_settings);
                    // Single-screen rule: delete menu after loading sub-screen
                    if (screen_menu != NULL) {
                        lv_obj_del(screen_menu);
                        screen_menu = NULL;
                    }
                }
                break;
            // Case 4 (Power) handled by power overlay in menu_screen.c
            default:
                ESP_LOGI(TAG, "Menu item not implemented yet");
                break;
        }
    }
}

// Function to add message to boot log
void add_boot_log(const char *msg) {
    // Always append to buffer (even if LVGL not ready yet)
    size_t current_len = strlen(boot_log_buffer);
    size_t remaining = sizeof(boot_log_buffer) - current_len - 1;

    if (remaining > strlen(msg) + 1) {
        if (current_len > 0) {
            strncat(boot_log_buffer, "\n", remaining);
            remaining--;
        }
        strncat(boot_log_buffer, msg, remaining);
    }

    // Update label if it exists and LVGL is ready
    if (boot_log_label != NULL) {
        if (lvgl_port_lock(0)) {
            lv_label_set_text(boot_log_label, boot_log_buffer);
            lvgl_port_unlock();
        }
    }

    // Update progress bar based on boot milestones
    if (boot_progress_bar != NULL) {
        if (lvgl_port_lock(0)) {
            int progress = 0;

            // Map boot messages to progress percentage
            if (strstr(msg, "CYGM Starting") != NULL) progress = 5;
            else if (strstr(msg, "NVS") != NULL) progress = 10;
            else if (strstr(msg, "Network mutex") != NULL) progress = 15;
            else if (strstr(msg, "LCD") != NULL) progress = 25;
            else if (strstr(msg, "LED") != NULL) progress = 30;
            else if (strstr(msg, "Buzzer") != NULL) progress = 35;
            else if (strstr(msg, "Battery") != NULL) progress = 40;
            else if (strstr(msg, "Init WiFi") != NULL) progress = 50;
            else if (strstr(msg, "Display ready") != NULL) progress = 60;
            else if (strstr(msg, "WiFi") != NULL) progress = 75;
            else if (strstr(msg, "connected") != NULL) progress = 85;
            else if (strstr(msg, "Home") != NULL) progress = 100;

            // Animate progress bar update
            if (progress > 0) {
                lv_bar_set_value(boot_progress_bar, progress, LV_ANIM_ON);
            }

            lvgl_port_unlock();
        }
    }
}

// Draw small sunrise icon (12x12)
void draw_sunrise_icon(lv_obj_t *canvas) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);

    // Draw small sun
    arc_dsc.color = lv_color_hex(0xFFAA00);
    arc_dsc.width = 4;
    lv_canvas_draw_arc(canvas, 6, 6, 3, 0, 360, &arc_dsc);

    // Draw 4 rays (top, right, bottom, left)
    line_dsc.color = lv_color_hex(0xFFAA00);
    line_dsc.width = 1;
    lv_point_t ray[2];

    // Top ray
    ray[0].x = 6; ray[0].y = 0;
    ray[1].x = 6; ray[1].y = 2;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);

    // Right ray
    ray[0].x = 10; ray[0].y = 6;
    ray[1].x = 12; ray[1].y = 6;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);

    // Bottom ray (shorter, suggesting horizon)
    ray[0].x = 6; ray[0].y = 10;
    ray[1].x = 6; ray[1].y = 11;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);

    // Left ray
    ray[0].x = 0; ray[0].y = 6;
    ray[1].x = 2; ray[1].y = 6;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);
}

// Draw small sunset icon (12x12)
void draw_sunset_icon(lv_obj_t *canvas) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);

    // Draw small sun (lower position for sunset)
    arc_dsc.color = lv_color_hex(0xFF6600);  // More orange/red for sunset
    arc_dsc.width = 4;
    lv_canvas_draw_arc(canvas, 6, 8, 3, 0, 360, &arc_dsc);

    // Draw horizon line
    line_dsc.color = lv_color_hex(COLOR_TEXT_GRAY);
    line_dsc.width = 1;
    lv_point_t horizon[2];
    horizon[0].x = 0; horizon[0].y = 11;
    horizon[1].x = 12; horizon[1].y = 11;
    lv_canvas_draw_line(canvas, horizon, 2, &line_dsc);

    // Draw rays above horizon only
    line_dsc.color = lv_color_hex(0xFF6600);
    lv_point_t ray[2];

    // Top-left ray
    ray[0].x = 2; ray[0].y = 4;
    ray[1].x = 4; ray[1].y = 6;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);

    // Top ray
    ray[0].x = 6; ray[0].y = 3;
    ray[1].x = 6; ray[1].y = 5;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);

    // Top-right ray
    ray[0].x = 8; ray[0].y = 6;
    ray[1].x = 10; ray[1].y = 4;
    lv_canvas_draw_line(canvas, ray, 2, &line_dsc);
}

// ===== SPLASH SCREEN =====
void create_splash_screen(void) {
    screen_splash = lv_obj_create(NULL);
    lv_obj_add_style(screen_splash, &style_bg, 0);

    // App name - large blue title
    lv_obj_t *title = lv_label_create(screen_splash);
    lv_label_set_text(title, "CYGM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // Subtle separator line under title
    lv_obj_t *sep = lv_obj_create(screen_splash);
    lv_obj_set_size(sep, 120, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 1, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 80);

    // Tagline
    lv_obj_t *sub = lv_label_create(screen_splash);
    lv_label_set_text(sub, "Continuous Glucose Monitor");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 90);

    // Website URL
    lv_obj_t *website = lv_label_create(screen_splash);
    lv_label_set_text(website, "CYGM.me");
    lv_obj_set_style_text_font(website, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(website, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(website, LV_ALIGN_TOP_MID, 0, 112);

    // Attribution + version on same line area
    lv_obj_t *author = lv_label_create(screen_splash);
    lv_label_set_text(author, "Carl Brothers");
    lv_obj_set_style_text_font(author, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(author, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(author, LV_ALIGN_TOP_MID, 0, 134);

    lv_obj_t *version = lv_label_create(screen_splash);
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "v%s", CYGM_VERSION_STRING);
    lv_label_set_text(version, version_str);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(version, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(version, LV_ALIGN_TOP_MID, 0, 148);

    // Boot status label - single line above progress bar
    boot_log_label = lv_label_create(screen_splash);
    lv_label_set_text(boot_log_label, "Initializing...");
    lv_label_set_long_mode(boot_log_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(boot_log_label, 240);
    lv_obj_set_style_text_font(boot_log_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(boot_log_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(boot_log_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(boot_log_label, LV_ALIGN_BOTTOM_MID, 0, -32);

    // Fat progress bar - matching WiFi overlay style
    boot_progress_bar = lv_bar_create(screen_splash);
    lv_obj_set_size(boot_progress_bar, 240, 16);
    lv_obj_align(boot_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_bar_set_value(boot_progress_bar, 0, LV_ANIM_OFF);

    // Dark track
    lv_obj_set_style_bg_color(boot_progress_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(boot_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(boot_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(boot_progress_bar, 8, LV_PART_MAIN);

    // Blue gradient indicator
    lv_obj_set_style_bg_color(boot_progress_bar, lv_color_hex(0x1D4ED8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(boot_progress_bar, lv_color_hex(COLOR_ACCENT_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(boot_progress_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(boot_progress_bar, 8, LV_PART_INDICATOR);

    // Clear buffer
    memset(boot_log_buffer, 0, sizeof(boot_log_buffer));
}


// ==================== Main Application ====================

void app_main(void) {
    ESP_LOGI(TAG, "=== CYGM - Continuous Glucose Monitor ===");
    ESP_LOGI(TAG, "Hardware: JC2432W328 (ESP32-D0WD)");
    ESP_LOGI(TAG, "Core 0: WiFi | Core 1: UI/Touch");

    add_boot_log("CYGM Starting...");

    // Initialize NVS
    add_boot_log("Init NVS storage");
    ESP_ERROR_CHECK(nvs_config_init());

    // Create network mutex for memory management
    network_mutex = xSemaphoreCreateMutex();
    if (network_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create network mutex!");
    } else {
        ESP_LOGI(TAG, "Network mutex created for memory management");
    }

    // Load time settings from NVS
    add_boot_log("Load settings");
    load_time_settings();

    // Load weather settings from NVS
    load_weather_settings();

    // Load alarm settings from NVS (or use defaults if not found)
    esp_err_t alarm_ret = nvs_load_alarm_settings(&current_alarm_settings);
    if (alarm_ret != ESP_OK) {
        ESP_LOGI(TAG, "No alarm settings found, using defaults");
        nvs_get_default_alarm_settings(&current_alarm_settings);
    } else {
        ESP_LOGI(TAG, "Alarm settings loaded from NVS");
    }

    // Initialize SD card logger (mount-on-write, ~2.5KB permanent)
    add_boot_log("Init SD logger");
    sd_logger_init();  // Non-fatal if no card inserted
    screenshot_init(); // Serial command listener (type "ss" in monitor)

    // Log reset reason — critical for diagnosing crashes, watchdogs, brownouts
    {
        esp_reset_reason_t reason = esp_reset_reason();
        const char *reason_str;
        switch (reason) {
            case ESP_RST_POWERON:  reason_str = "POWER_ON";   break;
            case ESP_RST_SW:      reason_str = "SW_RESET";    break;
            case ESP_RST_PANIC:   reason_str = "PANIC";       break;
            case ESP_RST_INT_WDT: reason_str = "INT_WDT";     break;
            case ESP_RST_TASK_WDT:reason_str = "TASK_WDT";    break;
            case ESP_RST_WDT:     reason_str = "OTHER_WDT";   break;
            case ESP_RST_DEEPSLEEP:reason_str = "DEEP_SLEEP"; break;
            case ESP_RST_BROWNOUT:reason_str = "BROWNOUT";    break;
            case ESP_RST_SDIO:    reason_str = "SDIO";        break;
            default:              reason_str = "UNKNOWN";      break;
        }
        sd_log(TAG, "Boot: %s, reset=%s, heap=%lu", CYGM_VERSION_STRING, reason_str, esp_get_free_heap_size());
        ESP_LOGI(TAG, "Reset reason: %s (%d)", reason_str, (int)reason);
    }
    sd_logger_flush();

    // Initialize glucose history system (loads from NVS if exists)
    add_boot_log("Load glucose history");
    ESP_ERROR_CHECK(glucose_history_init());
    glucose_history_register_shutdown_handler();

    // Load brightness settings from NVS
    nvs_load_brightness(&screen_brightness_percent);
    saved_brightness_percent = screen_brightness_percent;

    // Load SD glucose logging preference from NVS
    sd_glucose_logging_enabled = nvs_get_sd_glucose_logging();

    // Initialize display (before LVGL)
    add_boot_log("Init display");
    ESP_ERROR_CHECK(display_init());

    // Apply saved brightness
    display_set_brightness(screen_brightness_percent);

    // Initialize LED
    add_boot_log("Init LED");
    led_init();
    led_set_breathing_color(127, 0, 0);  // Red = initializing

    // Initialize Buzzer/Speaker
    add_boot_log("Init buzzer");
    buzzer_init();

    // Initialize Battery Monitoring
    add_boot_log("Init battery");
    battery_init();

    // LED breathing task disabled - LED off
    // xTaskCreate(led_breathing_task, "led_breathe", 2048, NULL, 1, NULL);
    led_set_color(0, 0, 0);  // Turn LED off completely

    // Create Core 1 task: UI + Touch
    add_boot_log("Start UI task");
    xTaskCreatePinnedToCore(
        ui_task_core1,
        "ui_task",
        8192,
        NULL,
        5,  // Higher priority
        &ui_task_handle,
        1   // Core 1
    );

    // Create Core 0 task: WiFi
    add_boot_log("Start WiFi task");
    xTaskCreatePinnedToCore(
        wifi_task_core0,
        "wifi_task",
        8192,
        NULL,
        4,  // Medium priority
        &wifi_task_handle,
        0   // Core 0
    );

    add_boot_log("System ready!");
    ESP_LOGI(TAG, "System initialized - tasks running");

    // Confirm OTA boot (prevents automatic rollback to previous firmware)
    update_confirm_boot();

    // Play boot-up sound to indicate device is ready
    vTaskDelay(pdMS_TO_TICKS(500));  // Small delay to let everything settle
    play_boot_sound();
}
