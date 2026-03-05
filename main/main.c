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
#include "web_server.h"
#include "ui_config.h"
#include "hardware/led.h"
#include "hardware/buzzer.h"
#include "hardware/battery.h"
#include "hardware/display.h"
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
// SD card logging removed - not needed for monitoring device (use serial output for debugging)

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
#define BATTERY_VOLTAGE_DIVIDER 1.46        // Calibrated: shows under-load voltage
#define BATTERY_FULL_VOLTAGE 4.0            // Fully charged under load (100%)
#define BATTERY_NOMINAL_VOLTAGE 3.7         // Nominal voltage (50%)
#define BATTERY_LOW_VOLTAGE 3.2             // Safe shutdown threshold (prevents damage)
#define BATTERY_CRITICAL_PERCENT 10         // Warning threshold (10%)

// Clear WiFi credentials flag
#define CLEAR_WIFI_CREDENTIALS 0

// Web server flag moved to shared_state.h
// Enabled for screenshot functionality

// SD Card Logging: REMOVED PERMANENTLY (2026-01-30)
// Reason: This is a monitoring device, not a data logger. SD card consumed 15KB RAM constantly,
// causing SSL allocation failures for Dexcom (life-critical). Serial output is sufficient for debugging.
// Files removed: sd_logger.c, sd_logger.h
// See git history to restore if needed (not recommended)

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
lv_obj_t *visual_alarm_overlay = NULL;      // White overlay for visual alarm
lv_obj_t *visual_alarm_container = NULL;    // Container overlay for alarm slider (styled)
lv_obj_t *visual_alarm_slider = NULL;       // Slider to dismiss visual alarm
lv_obj_t *visual_alarm_slider_label = NULL; // Label for slider
lv_obj_t *screen_alarm_snooze = NULL;       // Snooze/dismiss screen
lv_timer_t *visual_alarm_timer = NULL;      // Timer for pulsing effect
bool visual_alarm_active = false;
uint8_t visual_alarm_pulse_state = 0;       // 0 = normal, 1 = white
int64_t alarm_snooze_until = 0;             // Unix timestamp when snooze expires

// Audio alarm repeat
lv_timer_t *audio_alarm_timer = NULL;       // Timer for repeating audio
alarm_config_t *active_alarm_config = NULL; // Current alarm being played

// Battery monitoring
adc_oneshot_unit_handle_t adc_handle = NULL;
adc_cali_handle_t adc_cali_handle = NULL;
float battery_voltage = 0.0f;
int battery_percent = 100;
bool battery_low_warning_shown = false;  // Track if low battery warning played
lv_obj_t *battery_icon = NULL;           // Battery icon on home screen
lv_obj_t *battery_canvas = NULL;         // Battery canvas for custom drawn icon
lv_obj_t *battery_percent_label = NULL;  // Battery percentage text label
lv_obj_t *battery_charging_icon = NULL;  // Lightning bolt icon (shown when charging)
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
bool use_postal_code_entry = true;  // true = postal code, false = city dropdown

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
void draw_trend_arrow(lv_obj_t *canvas, dexcom_trend_t trend) {
    if (canvas == NULL) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.round_start = 1;
    line_dsc.round_end = 1;

    lv_draw_line_dsc_t glow_dsc;
    lv_draw_line_dsc_init(&glow_dsc);
    glow_dsc.round_start = 1;
    glow_dsc.round_end = 1;

    lv_point_t points[10];

    switch (trend) {
        case TREND_FLAT: {
            // Green glow - horizontal arrow (LARGER, 1.3x scale)
            glow_dsc.color = lv_color_hex(0x10B981);
            glow_dsc.width = 12;
            // Horizontal shaft (centered at 30,30)
            points[0] = (lv_point_t){6, 30};
            points[1] = (lv_point_t){50, 30};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Arrowhead V-shape (perfectly symmetric)
            points[0] = (lv_point_t){50, 30};  // Tip
            points[1] = (lv_point_t){41, 21};  // Upper winglet
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){50, 30};  // Tip
            points[1] = (lv_point_t){41, 39};  // Lower winglet (symmetric: 30-9=21, 30+9=39)
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrow
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 9;
            points[0] = (lv_point_t){6, 30};
            points[1] = (lv_point_t){50, 30};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){50, 30};
            points[1] = (lv_point_t){41, 21};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){50, 30};
            points[1] = (lv_point_t){41, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_FORTY_FIVE_UP: {
            // Green glow (LARGER, 1.3x scale)
            glow_dsc.color = lv_color_hex(0x10B981);
            glow_dsc.width = 12;
            // Diagonal shaft
            points[0] = (lv_point_t){10, 50};
            points[1] = (lv_point_t){47, 13};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Arrowhead V-shape (rotated 45°)
            points[0] = (lv_point_t){47, 13};
            points[1] = (lv_point_t){35, 12};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){47, 13};
            points[1] = (lv_point_t){48, 25};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrow
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 9;
            points[0] = (lv_point_t){10, 50};
            points[1] = (lv_point_t){47, 13};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){47, 13};
            points[1] = (lv_point_t){35, 12};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){47, 13};
            points[1] = (lv_point_t){48, 25};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_FORTY_FIVE_DOWN: {
            // Green glow (LARGER, 1.3x scale)
            glow_dsc.color = lv_color_hex(0x10B981);
            glow_dsc.width = 12;
            // Diagonal shaft
            points[0] = (lv_point_t){10, 10};
            points[1] = (lv_point_t){47, 47};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Arrowhead V-shape (rotated -45°)
            points[0] = (lv_point_t){47, 47};
            points[1] = (lv_point_t){35, 48};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){47, 47};
            points[1] = (lv_point_t){48, 35};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrow
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 9;
            points[0] = (lv_point_t){10, 10};
            points[1] = (lv_point_t){47, 47};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){47, 47};
            points[1] = (lv_point_t){35, 48};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){47, 47};
            points[1] = (lv_point_t){48, 35};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_SINGLE_UP: {
            // Orange glow (LARGER, 1.3x scale)
            glow_dsc.color = lv_color_hex(0xFFA500);
            glow_dsc.width = 12;
            // Vertical shaft
            points[0] = (lv_point_t){30, 54};
            points[1] = (lv_point_t){30, 13};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Arrowhead (symmetric)
            points[0] = (lv_point_t){30, 13};
            points[1] = (lv_point_t){21, 22};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){30, 13};
            points[1] = (lv_point_t){39, 22};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrow
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 9;
            points[0] = (lv_point_t){30, 54};
            points[1] = (lv_point_t){30, 13};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){30, 13};
            points[1] = (lv_point_t){21, 22};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){30, 13};
            points[1] = (lv_point_t){39, 22};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_SINGLE_DOWN: {
            // Orange glow
            glow_dsc.color = lv_color_hex(0xFFA500);
            glow_dsc.width = 11;
            // Vertical shaft (38 pixels)
            points[0] = (lv_point_t){30, 8};
            points[1] = (lv_point_t){30, 46};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Arrowhead
            points[0] = (lv_point_t){30, 46};
            points[1] = (lv_point_t){22, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){30, 46};
            points[1] = (lv_point_t){38, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrow
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 8;
            points[0] = (lv_point_t){30, 8};
            points[1] = (lv_point_t){30, 46};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){30, 46};
            points[1] = (lv_point_t){22, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){30, 46};
            points[1] = (lv_point_t){38, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_DOUBLE_UP: {
            // Red glow
            glow_dsc.color = lv_color_hex(0xFF4444);
            glow_dsc.width = 11;
            // Left arrow (36 pixels)
            points[0] = (lv_point_t){19, 52};
            points[1] = (lv_point_t){19, 16};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){19, 16};
            points[1] = (lv_point_t){11, 23};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){19, 16};
            points[1] = (lv_point_t){26, 23};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Right arrow (36 pixels)
            points[0] = (lv_point_t){41, 52};
            points[1] = (lv_point_t){41, 16};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){41, 16};
            points[1] = (lv_point_t){34, 23};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){41, 16};
            points[1] = (lv_point_t){49, 23};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrows
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 8;
            // Left arrow
            points[0] = (lv_point_t){19, 52};
            points[1] = (lv_point_t){19, 16};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){19, 16};
            points[1] = (lv_point_t){11, 23};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){19, 16};
            points[1] = (lv_point_t){26, 23};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            // Right arrow
            points[0] = (lv_point_t){41, 52};
            points[1] = (lv_point_t){41, 16};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){41, 16};
            points[1] = (lv_point_t){34, 23};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){41, 16};
            points[1] = (lv_point_t){49, 23};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        case TREND_DOUBLE_DOWN: {
            // Red glow
            glow_dsc.color = lv_color_hex(0xFF4444);
            glow_dsc.width = 11;
            // Left arrow (38 pixels)
            points[0] = (lv_point_t){19, 8};
            points[1] = (lv_point_t){19, 46};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){19, 46};
            points[1] = (lv_point_t){11, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){19, 46};
            points[1] = (lv_point_t){26, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            // Right arrow (38 pixels)
            points[0] = (lv_point_t){41, 8};
            points[1] = (lv_point_t){41, 46};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){41, 46};
            points[1] = (lv_point_t){34, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);
            points[0] = (lv_point_t){41, 46};
            points[1] = (lv_point_t){49, 39};
            lv_canvas_draw_line(canvas, points, 2, &glow_dsc);

            // Blue arrows
            line_dsc.color = lv_color_hex(0x3B82F6);
            line_dsc.width = 8;
            // Left arrow
            points[0] = (lv_point_t){19, 8};
            points[1] = (lv_point_t){19, 46};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){19, 46};
            points[1] = (lv_point_t){11, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){19, 46};
            points[1] = (lv_point_t){26, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            // Right arrow
            points[0] = (lv_point_t){41, 8};
            points[1] = (lv_point_t){41, 46};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){41, 46};
            points[1] = (lv_point_t){34, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            points[0] = (lv_point_t){41, 46};
            points[1] = (lv_point_t){49, 39};
            lv_canvas_draw_line(canvas, points, 2, &line_dsc);
            break;
        }

        default:
            // No trend or unknown - draw nothing
            break;
    }
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
void glucose_timer_bar_event_cb(lv_event_t *e);
static void visual_alarm_slider_event_cb(lv_event_t *e);

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

    // Check if snooze is active
    time_t now;
    time(&now);
    if (alarm_snooze_until > 0 && now < alarm_snooze_until) {
        ESP_LOGI(TAG, "Alarm snoozed until %lld (current: %ld)", alarm_snooze_until, (long)now);
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
            ESP_LOGI(TAG, "Alarm snoozed for %d min %d sec more (until %lld, current %ld)",
                     minutes_remaining, seconds_remaining, alarm_snooze_until, (long)now);
            return;  // Exit early - alarm is snoozed
        } else if (new_state != ALARM_STATE_NONE) {
            // Snooze expired and alarm condition still exists - re-trigger
            ESP_LOGI(TAG, "Snooze expired! Re-triggering alarm (state: %d)", new_state);
            alarm_snooze_until = 0;  // Clear snooze
            alarm_acknowledged = false;  // Reset acknowledgment
            current_alarm_state = new_state;  // Update state
            trigger_alarm(new_state);
            return;  // Don't process state change logic below
        } else {
            // Snooze expired but glucose returned to normal
            ESP_LOGI(TAG, "Snooze expired but glucose returned to normal");
            alarm_snooze_until = 0;  // Clear snooze
            current_alarm_state = ALARM_STATE_NONE;
            return;
        }
    }

    // State changed?
    if (new_state != current_alarm_state) {
        ESP_LOGI(TAG, "Alarm state changed: %d -> %d", current_alarm_state, new_state);
        current_alarm_state = new_state;
        alarm_acknowledged = false;  // Reset acknowledgment on state change

        if (new_state != ALARM_STATE_NONE) {
            trigger_alarm(new_state);
        } else {
            ESP_LOGI(TAG, "Glucose returned to normal range");
            stop_visual_alarm();
            stop_audio_alarm();
        }
    }
}

// Timer callback for pulsing white overlay (2 times per second = 250ms per phase)
static void visual_alarm_pulse_timer_cb(lv_timer_t *timer) {
    if (!visual_alarm_active || visual_alarm_overlay == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        if (visual_alarm_pulse_state == 0) {
            // Fade to white
            lv_obj_set_style_bg_opa(visual_alarm_overlay, LV_OPA_90, 0);
            visual_alarm_pulse_state = 1;
            ESP_LOGD(TAG, "Visual alarm: WHITE");
        } else {
            // Fade to transparent
            lv_obj_set_style_bg_opa(visual_alarm_overlay, LV_OPA_TRANSP, 0);
            visual_alarm_pulse_state = 0;
            ESP_LOGD(TAG, "Visual alarm: transparent");
        }
        lv_obj_invalidate(visual_alarm_overlay);  // Force redraw
        lvgl_port_unlock();
    }
}

// Timer callback for repeating audio alarm (every 3 seconds)
static void audio_alarm_repeat_timer_cb(lv_timer_t *timer) {
    if (active_alarm_config != NULL && active_alarm_config->audio_enabled) {
        ESP_LOGI(TAG, "Repeating alarm tone: %d at volume %d%%", active_alarm_config->tone, active_alarm_config->volume);
        play_alarm_tone(active_alarm_config->tone, active_alarm_config->volume);
    }
}

// Start audio alarm (with optional repeat)
static void start_audio_alarm(alarm_config_t *alarm) {
    if (alarm == NULL || !alarm->audio_enabled) {
        return;
    }

    // Stop any existing audio alarm
    stop_audio_alarm();

    // Save reference to alarm config
    active_alarm_config = alarm;

    // Play initial tone
    ESP_LOGI(TAG, "Playing alarm tone: %d at volume %d%%", alarm->tone, alarm->volume);
    play_alarm_tone(alarm->tone, alarm->volume);

    // Start repeat timer if enabled
    if (alarm->audio_repeat) {
        ESP_LOGI(TAG, "Starting audio alarm repeat timer (every 3 seconds)");
        audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
    }
}

// Stop audio alarm
static void stop_audio_alarm(void) {
    if (audio_alarm_timer != NULL) {
        ESP_LOGI(TAG, "Stopping audio alarm repeat timer");
        lv_timer_del(audio_alarm_timer);
        audio_alarm_timer = NULL;
    }
    active_alarm_config = NULL;
    buzzer_stop();  // Ensure buzzer is stopped
}

// Event handler for visual alarm slider (dismiss)
static void visual_alarm_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        int32_t value = lv_slider_get_value(slider);

        // Slider must be moved to 100 to dismiss
        if (value >= 100) {
            ESP_LOGI(TAG, "Visual alarm slider activated - showing snooze/dismiss overlay");
            stop_visual_alarm();
            stop_audio_alarm();  // Stop audio alarm when slider is dismissed

            // Show snooze/dismiss overlay on home screen
            if (lvgl_port_lock(0)) {
                if (screen_alarm_snooze != NULL) {
                    lv_obj_del(screen_alarm_snooze);
                    screen_alarm_snooze = NULL;
                }
                create_alarm_snooze_screen();  // Creates overlay on home screen
                lvgl_port_unlock();
            }
        }
    } else if (code == LV_EVENT_RELEASED) {
        // If slider released before reaching 100, snap back to 0
        int32_t value = lv_slider_get_value(slider);
        if (value < 100) {
            lv_slider_set_value(slider, 0, LV_ANIM_ON);
            ESP_LOGI(TAG, "Slider released before 100%% - snapping back");
        }
    }
}

// Start visual alarm (pulsing white screen with slider)
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

    if (lvgl_port_lock(0)) {
        // Create white overlay that covers entire screen (for pulsing effect)
        visual_alarm_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(visual_alarm_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(visual_alarm_overlay, 0, 0);
        lv_obj_set_style_bg_color(visual_alarm_overlay, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(visual_alarm_overlay, LV_OPA_TRANSP, 0);  // Start transparent
        lv_obj_set_style_border_width(visual_alarm_overlay, 0, 0);
        lv_obj_set_style_pad_all(visual_alarm_overlay, 0, 0);
        lv_obj_set_style_radius(visual_alarm_overlay, 0, 0);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(visual_alarm_overlay);  // Move to front

        // Determine which alarm triggered
        const char *alarm_type = "Unknown";
        uint32_t alarm_color = COLOR_RED;
        if (current_alarm_state == ALARM_STATE_HIGH_ALARM) {
            alarm_type = "HIGH";
            alarm_color = current_alarm_settings.high_alarm.text_color;
        } else if (current_alarm_state == ALARM_STATE_HIGH_WARNING) {
            alarm_type = "HIGH WARN";
            alarm_color = current_alarm_settings.high_warning.text_color;
        } else if (current_alarm_state == ALARM_STATE_LOW_WARNING) {
            alarm_type = "LOW WARN";
            alarm_color = current_alarm_settings.low_warning.text_color;
        } else if (current_alarm_state == ALARM_STATE_LOW_ALARM) {
            alarm_type = "LOW";
            alarm_color = current_alarm_settings.low_alarm.text_color;
        }

        // Create styled container overlay (matches brightness/snooze overlay style)
        visual_alarm_container = lv_obj_create(lv_scr_act());
        lv_obj_set_size(visual_alarm_container, 280, 200);
        lv_obj_center(visual_alarm_container);
        lv_obj_set_style_bg_color(visual_alarm_container, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(visual_alarm_container, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(visual_alarm_container, 2, 0);
        lv_obj_set_style_border_color(visual_alarm_container, lv_color_hex(COLOR_RED), 0);  // Red border
        lv_obj_set_style_radius(visual_alarm_container, 12, 0);
        lv_obj_clear_flag(visual_alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(visual_alarm_container);

        // Title "ALARM" (top left)
        lv_obj_t *title = lv_label_create(visual_alarm_container);
        lv_label_set_text(title, "ALARM");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

        // Alarm type (top right)
        lv_obj_t *type_label = lv_label_create(visual_alarm_container);
        lv_label_set_text(type_label, alarm_type);
        lv_obj_set_style_text_font(type_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(type_label, lv_color_hex(alarm_color), 0);
        lv_obj_align(type_label, LV_ALIGN_TOP_RIGHT, -10, 10);

        // Current glucose value (centered, prominent)
        lv_obj_t *glucose_label = lv_label_create(visual_alarm_container);
        char glucose_text[32];
        snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", current_glucose);
        lv_label_set_text(glucose_label, glucose_text);
        lv_obj_set_style_text_font(glucose_label, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(glucose_label, lv_color_hex(alarm_color), 0);
        lv_obj_align(glucose_label, LV_ALIGN_TOP_MID, 0, 45);

        // Create slider to dismiss (inside container)
        visual_alarm_slider = lv_slider_create(visual_alarm_container);
        lv_obj_set_size(visual_alarm_slider, 240, 30);
        lv_obj_align(visual_alarm_slider, LV_ALIGN_BOTTOM_MID, 0, -15);
        lv_slider_set_range(visual_alarm_slider, 0, 100);
        lv_slider_set_value(visual_alarm_slider, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(visual_alarm_slider, lv_color_hex(0xFF0000), LV_PART_MAIN);
        lv_obj_set_style_bg_color(visual_alarm_slider, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(visual_alarm_slider, lv_color_hex(0x00FF00), LV_PART_KNOB);
        lv_obj_set_style_pad_all(visual_alarm_slider, 2, LV_PART_KNOB);  // Smaller knob padding
        lv_obj_add_event_cb(visual_alarm_slider, visual_alarm_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(visual_alarm_slider, visual_alarm_slider_event_cb, LV_EVENT_RELEASED, NULL);

        // Slider label INSIDE slider (white text)
        visual_alarm_slider_label = lv_label_create(visual_alarm_slider);
        lv_label_set_text(visual_alarm_slider_label, "Slide to disarm");
        lv_obj_set_style_text_color(visual_alarm_slider_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(visual_alarm_slider_label, &lv_font_montserrat_14, 0);
        lv_obj_align(visual_alarm_slider_label, LV_ALIGN_CENTER, 0, 0);

        lvgl_port_unlock();
    }

    // Start pulse timer (250ms = 4 times per second, alternating white/normal = 2 pulses per second)
    visual_alarm_timer = lv_timer_create(visual_alarm_pulse_timer_cb, 250, NULL);
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
            visual_alarm_slider = NULL;
            visual_alarm_slider_label = NULL;
        }
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
            // "Last Known Good" approach - always show last valid glucose reading
            // This prevents user anxiety from seeing "---" during temporary failures

            // Update glucose value
            char glucose_str[16];
            snprintf(glucose_str, sizeof(glucose_str), "%d", current_glucose);
            lv_label_set_text(label_glucose, glucose_str);

            // Color code based on alarm thresholds (configured by user)
            uint32_t alarm_color = get_glucose_alarm_color(current_glucose);
            lv_obj_set_style_text_color(label_glucose, lv_color_hex(alarm_color), 0);

            // Update trend arrow canvas
            lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Clear canvas
            draw_trend_arrow(trend_canvas, current_trend);

            // Calculate minutes ago
            time_t now;
            time(&now);
            int minutes_ago = (int)difftime(now, glucose_timestamp) / 60;

            char time_ago_str[32];
            if (minutes_ago < 1) {
                snprintf(time_ago_str, sizeof(time_ago_str), "Just now");
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
            } else if (minutes_ago == 1) {
                snprintf(time_ago_str, sizeof(time_ago_str), "1 min ago");
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
            } else if (minutes_ago >= 15) {
                // Very stale data - show warning but keep displaying value
                snprintf(time_ago_str, sizeof(time_ago_str), "Last: %d mins ago", minutes_ago);
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_RED), 0);  // Stale data warning
            } else if (!glucose_data_fresh) {
                // Data is not fresh (last fetch failed) - indicate staleness
                snprintf(time_ago_str, sizeof(time_ago_str), "Last: %d mins ago", minutes_ago);
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_ORANGE), 0);  // Stale but not critical
            } else {
                // Fresh data - normal display
                snprintf(time_ago_str, sizeof(time_ago_str), "%d mins ago", minutes_ago);
                lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
            }
            lv_label_set_text(label_time_ago, time_ago_str);

            // Glucose display updated (reduced logging to save serial bandwidth)
            ESP_LOGD(TAG, "Glucose display updated: %d mg/dL, %s, %d mins ago, fresh: %s",
                     current_glucose, dexcom_trend_description(current_trend), minutes_ago,
                     glucose_data_fresh ? "yes" : "no");
        } else {
            // No data available (never received or not authenticated) - show status-specific message
            lv_label_set_text(label_glucose, "---");
            lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Clear arrow

            switch (glucose_status) {
                case GLUCOSE_STATUS_WARMUP:
                    lv_label_set_text(label_time_ago, "Sensor warmup");
                    lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ORANGE), 0);  // Orange
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
                    lv_label_set_text(label_time_ago, "No sensor data");
                    lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_TEXT_DIM), 0);
                    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_GRAY), 0);
                    break;
            }
        }
        lvgl_port_unlock();
    }
}

// Glucose update task - fetches glucose periodically (every 90 seconds)
void glucose_update_task(void *pvParameters) {
    ESP_LOGI(TAG, "Glucose update task started (90 second interval)");

    // Smart wait: Only wait remaining time if an initial fetch already happened
    const int64_t FETCH_INTERVAL_MS = 90000;  // 90 seconds
    int64_t wait_time_ms = FETCH_INTERVAL_MS;

    if (last_glucose_fetch_time > 0) {
        // Calculate time since last fetch
        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last_glucose_fetch_time;

        if (elapsed < FETCH_INTERVAL_MS) {
            wait_time_ms = FETCH_INTERVAL_MS - elapsed;
            ESP_LOGI(TAG, "Last fetch was %lld ms ago, waiting %lld ms until next fetch", elapsed, wait_time_ms);
        } else {
            ESP_LOGI(TAG, "Last fetch was %lld ms ago (>90s), fetching immediately", elapsed);
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
        // Wait for 90 seconds OR until force-fetch is requested
        // Check every 1 second to allow responsive force-fetch
        for (int i = 0; i < 90; i++) {
            if (glucose_force_fetch_requested) {
                ESP_LOGI(TAG, "Force-fetch requested, breaking wait loop early");
                glucose_force_fetch_requested = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));  // Check every 1 second
        }

        // STRICT runtime policy: never fetch off-home
        if (pause_background_tasks || !home_screen_active) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Check if WiFi is connected
        if (!wifi_connected) {
            ESP_LOGI(TAG, "[Glucose Task] WiFi not connected - pausing (waiting 10s)");
            vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
            continue;
        }

        // Only fetch if authenticated
        bool is_auth = dexcom_is_authenticated();
        ESP_LOGI(TAG, "[Glucose Task] Auth status: %s, Free heap: %lu bytes",
                 is_auth ? "AUTHENTICATED" : "NOT AUTHENTICATED",
                 esp_get_free_heap_size());

        if (is_auth) {
            // CRITICAL: Take network mutex to ensure exclusive access
            ESP_LOGI(TAG, "Waiting for network mutex...");
            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                ESP_LOGI(TAG, "Network mutex acquired for glucose fetch");

                // Re-check policy immediately after lock: abort off-home work
                if (pause_background_tasks || !home_screen_active) {
                    xSemaphoreGive(network_mutex);
                    ESP_LOGI(TAG, "Network mutex released (off-home policy)");
                    continue;
                }

                // Check if we have enough INTERNAL memory for SSL connection
                // SSL MUST use internal DRAM, not external PSRAM
                size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t free_total = esp_get_free_heap_size();
                size_t min_ever = esp_get_minimum_free_heap_size();

                ESP_LOGI(TAG, "===== GLUCOSE FETCH MEMORY CHECK =====");
                ESP_LOGI(TAG, "Free internal (DRAM): %lu bytes", free_internal);
                ESP_LOGI(TAG, "Free total: %lu bytes", free_total);
                ESP_LOGI(TAG, "Minimum ever: %lu bytes", min_ever);

                // Detailed memory breakdown by heap capabilities
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "=== DETAILED MEMORY BREAKDOWN ===");
                ESP_LOGI(TAG, "DMA-capable memory (MALLOC_CAP_DMA):   %6lu bytes free",
                         heap_caps_get_free_size(MALLOC_CAP_DMA));
                ESP_LOGI(TAG, "32-bit aligned memory (MALLOC_CAP_32BIT): %6lu bytes free",
                         heap_caps_get_free_size(MALLOC_CAP_32BIT));
                ESP_LOGI(TAG, "Internal memory (MALLOC_CAP_INTERNAL): %6lu bytes free",
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                ESP_LOGI(TAG, "Default memory (MALLOC_CAP_DEFAULT):  %6lu bytes free",
                         heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "Largest free blocks by type:");
                ESP_LOGI(TAG, "  INTERNAL:  %6lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                ESP_LOGI(TAG, "  DMA:       %6lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
                ESP_LOGI(TAG, "  DEFAULT:   %6lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                ESP_LOGI(TAG, "");
                multi_heap_info_t heap_info;
                heap_caps_get_info(&heap_info, MALLOC_CAP_INTERNAL);
                ESP_LOGI(TAG, "Internal heap fragmentation info:");
                ESP_LOGI(TAG, "  Total free: %lu bytes", heap_info.total_free_bytes);
                ESP_LOGI(TAG, "  Allocated: %lu bytes", heap_info.total_allocated_bytes);
                ESP_LOGI(TAG, "  Largest block: %lu bytes", heap_info.largest_free_block);
                ESP_LOGI(TAG, "  Min free ever: %lu bytes", heap_info.minimum_free_bytes);
                ESP_LOGI(TAG, "  Allocated blocks: %lu", heap_info.allocated_blocks);
                ESP_LOGI(TAG, "  Free blocks: %lu", heap_info.free_blocks);
                ESP_LOGI(TAG, "  Total blocks: %lu", heap_info.total_blocks);

                // Calculate fragmentation percentage
                float fragmentation = 0;
                if (heap_info.total_free_bytes > 0) {
                    fragmentation = 100.0f * (1.0f - ((float)heap_info.largest_free_block / (float)heap_info.total_free_bytes));
                }
                ESP_LOGI(TAG, "  Fragmentation: %.1f%% (largest block is %.1f%% of total free)",
                         fragmentation,
                         100.0f * ((float)heap_info.largest_free_block / (float)heap_info.total_free_bytes));

                // Warn if severely fragmented
                if (fragmentation > 50.0f) {
                    ESP_LOGW(TAG, "  WARNING: Severe memory fragmentation! %lu allocated blocks, only %lu free blocks",
                             heap_info.allocated_blocks, heap_info.free_blocks);
                    ESP_LOGW(TAG, "  Average allocated block size: %lu bytes",
                             heap_info.total_allocated_bytes / heap_info.allocated_blocks);
                    if (heap_info.free_blocks > 0) {
                        ESP_LOGW(TAG, "  Average free block size: %lu bytes",
                                 heap_info.total_free_bytes / heap_info.free_blocks);
                    }
                }
                ESP_LOGI(TAG, "=================================");
                ESP_LOGI(TAG, "");

                // WARNING: If minimum ever is critically low, log it
                if (min_ever < 10000) {
                    ESP_LOGW(TAG, "CRITICAL: System went down to %lu bytes at some point! Risk of crashes!", min_ever);
                }

                // Dynamic memory threshold based on whether persistent client is open
                // - Client CLOSED (needs new SSL handshake): 35KB minimum
                // - Client OPEN (reusing connection): 20KB minimum
                bool client_open = dexcom_persistent_client_is_open();
                size_t required_memory = client_open ? 20000 : 35000;
                const char *connection_type = client_open ? "REUSING existing SSL connection" : "CREATING new SSL connection";

                // Check largest free block to detect fragmentation
                size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                ESP_LOGI(TAG, "Largest contiguous block: %lu bytes", largest_block);

                if (largest_block < 30000 && !client_open) {
                    ESP_LOGW(TAG, "Memory fragmented! Largest block: %lu (need ~30KB for SSL)", largest_block);
                    ESP_LOGW(TAG, "Skipping glucose fetch - will retry in 90 seconds");

                    // Mark fetch as failed due to memory
                    glucose_fetch_failed = true;
                    glucose_fetch_in_progress = false;

                    // Stop animation and update bar to red
                    if (lvgl_port_lock(0)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_fetch_timer_bar != NULL) {
                            lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
                            lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                        }
                        lvgl_port_unlock();
                    }

                    xSemaphoreGive(network_mutex);
                    continue;
                }

                ESP_LOGI(TAG, "Persistent client status: %s", client_open ? "OPEN" : "CLOSED");
                ESP_LOGI(TAG, "Memory requirement: %lu bytes (%s)", required_memory, connection_type);

                if (free_internal < required_memory) {
                    ESP_LOGE(TAG, "Insufficient INTERNAL memory: %lu bytes (need %lu for %s)",
                             free_internal, required_memory, connection_type);
                    ESP_LOGE(TAG, "Skipping glucose fetch - will retry in 90 seconds");

                    // Mark fetch as failed due to memory
                    glucose_fetch_failed = true;
                    glucose_fetch_in_progress = false;

                    // Stop animation and update bar to red
                    if (lvgl_port_lock(0)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_fetch_timer_bar != NULL) {
                            lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
                            lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                            // Keep blue glow even on failure
                            lv_obj_set_style_shadow_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
                        }
                        lvgl_port_unlock();
                    }

                    // Release mutex before error handling
                    xSemaphoreGive(network_mutex);
                    ESP_LOGI(TAG, "Network mutex released (insufficient memory)");

                    // Show error LED
                    led_start_error_blink();
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    led_stop_error_blink();

                    continue;
                }

                ESP_LOGI(TAG, "Memory check PASSED - %lu bytes available (%s)",
                         free_internal, connection_type);

                // Mark fetch as in progress
                glucose_fetch_in_progress = true;

                dexcom_glucose_t glucose = {0};  // Initialize to prevent garbage data
                ESP_LOGI(TAG, "Fetching periodic glucose reading...");

                // Blink blue to indicate Dexcom poll starting
                led_blink_blue();

                esp_err_t ret = dexcom_fetch_glucose(&glucose);

            if (ret == ESP_OK && glucose.valid) {
                // Update glucose data - "Last Known Good" approach
                current_glucose = glucose.value;
                current_trend = glucose.trend;
                glucose_timestamp = glucose.timestamp;
                glucose_data_valid = true;    // We've received valid data at least once
                glucose_data_fresh = true;    // This fetch succeeded
                glucose_status = glucose.status;

                // Update timer for progress bar
                last_glucose_fetch_time = esp_timer_get_time() / 1000;  // milliseconds

                ESP_LOGI(TAG, "Glucose updated: %d mg/dL, trend: %s",
                         glucose.value, dexcom_trend_description(glucose.trend));

                // Add to history for trend chart (convert seconds to milliseconds)
                glucose_history_add(glucose.value, (int64_t)glucose.timestamp * 1000);

                // Mark fetch as successful
                glucose_fetch_failed = false;
                glucose_fetch_in_progress = false;

                // Stop animation and restore blue bar
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

                // Show success LED (green fade)
                led_show_success();

                // SD logging removed: sd_log(TAG, "Glucose update SUCCESS: %d mg/dL, trend: %s, heap: %lu bytes", ...);

                // Play success sound on first glucose reading
                if (!first_glucose_received) {
                    first_glucose_received = true;
                    ESP_LOGI(TAG, "First glucose reading received - playing success sound");
                    play_success_sound();
                }

                // Check alarms
                check_glucose_alarms(current_glucose);

                // Update display
                update_glucose_display();
            } else {
                // Fetch failed - mark data as stale but keep displaying last known good value
                glucose_data_fresh = false;   // This fetch failed
                glucose_status = glucose.status;

                // Only clear glucose_data_valid if we're not authenticated (need re-login)
                if (glucose.status == GLUCOSE_STATUS_NOT_AUTHENTICATED) {
                    glucose_data_valid = false;
                }

                // Mark fetch as failed
                glucose_fetch_failed = true;
                glucose_fetch_in_progress = false;

                // Stop animation and update bar to red (but keep blue glow)
                if (lvgl_port_lock(0)) {
                    if (glucose_fetch_animation_timer != NULL) {
                        lv_timer_del(glucose_fetch_animation_timer);
                        glucose_fetch_animation_timer = NULL;
                    }
                    if (glucose_fetch_timer_bar != NULL) {
                        lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
                        lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
                        // Keep blue glow even on failure
                        lv_obj_set_style_shadow_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
                    }
                    lvgl_port_unlock();
                }

                update_glucose_display();
                ESP_LOGW(TAG, "Periodic glucose fetch failed: %s", esp_err_to_name(ret));

                // Show error LED (red blink)
                led_start_error_blink();
                vTaskDelay(pdMS_TO_TICKS(5000));  // Blink for 5 seconds
                led_stop_error_blink();

                // SD logging removed: sd_log(TAG, "Glucose update FAILED: %s, status: %s, heap: %lu bytes", ...);
            }

                // AGGRESSIVE MEMORY FIX: Close SSL connection after EVERY fetch to prevent memory leaks
                // Forces fresh SSL handshake every 90 seconds, but prevents memory accumulation
                ESP_LOGI(TAG, "Closing Dexcom SSL connection to free memory (aggressive mode)");
                dexcom_close_persistent_client();
                ESP_LOGI(TAG, "Free heap after SSL close: %lu bytes", esp_get_free_heap_size());

                // Release network mutex
                xSemaphoreGive(network_mutex);
                ESP_LOGI(TAG, "Network mutex released after glucose fetch");
            } else {
                ESP_LOGE(TAG, "Failed to acquire network mutex - this should never happen!");
            }
        } else {
            ESP_LOGW(TAG, "Not authenticated - skipping glucose fetch");
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

// Animation timer for glucose fetch bouncing indicator (Knight Rider style for errors)
// Event handler for tapping glucose timer bar to force immediate fetch
void glucose_timer_bar_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    ESP_LOGI(TAG, "Timer bar event received: code=%d (CLICKED=%d)", code, LV_EVENT_CLICKED);

    if (code == LV_EVENT_CLICKED) {
        if (glucose_fetch_in_progress) {
            ESP_LOGI(TAG, "Glucose fetch already in progress, ignoring click");
            return;
        }

        ESP_LOGI(TAG, "Force glucose fetch requested by user (timer bar tapped)");
        glucose_force_fetch_requested = true;
        glucose_fetch_failed = false;
        glucose_fetch_in_progress = true;
    }
}

void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");

        // Pause background tasks when leaving home screen
        home_screen_active = false;
        pause_background_tasks = true;
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
        ESP_LOGI(TAG, "Background tasks paused (leaving home screen)");

        // Close Dexcom persistent client to free ~30KB memory for WiFi scan/UI
        ESP_LOGI(TAG, "Closing Dexcom persistent client before WiFi scan");
        dexcom_close_persistent_client();
        ESP_LOGI(TAG, "Free heap after closing Dexcom client: %lu bytes (internal: %lu)",
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
                    lvgl_port_unlock();
                }
                break;
            case 1:  // WiFi Setup
                ESP_LOGI(TAG, "Launching WiFi setup...");

                // Close Dexcom persistent client to free ~30KB memory for WiFi scan/UI
                ESP_LOGI(TAG, "Closing Dexcom persistent client before WiFi scan");
                dexcom_close_persistent_client();
                ESP_LOGI(TAG, "Free heap after closing Dexcom client: %lu bytes (internal: %lu)",
                         esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

                // Load WiFi list screen (create if needed)
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

                // Start WiFi scan in background task - overlay will be hidden when done
                xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
                break;
            case 2:  // Time/Date/Weather Settings (combined)
                ESP_LOGI(TAG, "Launching Time/Weather settings...");
                if (lvgl_port_lock(0)) {
                    if (screen_time_weather_settings != NULL) {
                        lv_obj_del(screen_time_weather_settings);
                        screen_time_weather_settings = NULL;
                    }
                    create_time_weather_settings_screen();
                    lv_scr_load(screen_time_weather_settings);
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
                }
                break;
            case 4:  // Deep Sleep
                ESP_LOGI(TAG, "Entering deep sleep mode...");
                ESP_LOGI(TAG, "Device will wake on button press (GPIO 0)");
                vTaskDelay(pdMS_TO_TICKS(500));  // Brief delay for UI feedback
                esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  // Wake on GPIO 0 (boot button), 0 = low level
                esp_deep_sleep_start();
                break;
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
    lv_label_set_text(website, "DigitalKaos.tech");
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

    // Initialize UI configuration system
    add_boot_log("Init UI config");
    ESP_ERROR_CHECK(ui_config_init());

#if CLEAR_WIFI_CREDENTIALS
    nvs_clear_wifi_credentials();
    ESP_LOGI(TAG, "WiFi credentials cleared");
#endif

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

    // Initialize glucose history system (loads from NVS if exists)
    add_boot_log("Load glucose history");
    ESP_ERROR_CHECK(glucose_history_init());

    // Load brightness settings from NVS
    nvs_load_brightness(&screen_brightness_percent);
    saved_brightness_percent = screen_brightness_percent;

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
        4096,
        NULL,
        4,  // Medium priority
        &wifi_task_handle,
        0   // Core 0
    );

    add_boot_log("System ready!");
    ESP_LOGI(TAG, "System initialized - tasks running");

    // Play boot-up sound to indicate device is ready
    vTaskDelay(pdMS_TO_TICKS(500));  // Small delay to let everything settle
    play_boot_sound();
}
