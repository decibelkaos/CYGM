/*
 * main.c - CYGM Continuous Glucose Monitor Display (ESP32-IDF + LVGL).
 * Core 0 runs WiFi and network tasks; Core 1 runs the UI and touch.
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
#include "esp_private/brownout.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
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

// Battery Monitoring (3.7V LiPo). Percent maps under-load voltage:
// 4.0V=100%, 3.7V=50%, 3.2V=0% (shut down below that to avoid cell damage).
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3   // GPIO 39 (ADC1_CH3)
#define BATTERY_ADC_GPIO    39
#define BATTERY_VOLTAGE_DIVIDER 1.50        // Calibrated: shows under-load voltage
#define BATTERY_FULL_VOLTAGE 4.0            // Fully charged under load (100%)
#define BATTERY_NOMINAL_VOLTAGE 3.7         // Nominal voltage (50%)
#define BATTERY_LOW_VOLTAGE 3.2             // Safe shutdown threshold (prevents damage)
#define BATTERY_CRITICAL_PERCENT 10         // Warning threshold (10%)

// SD logging mounts FAT only during a flush (~200ms) then unmounts. A
// permanently mounted volume costs ~13KB of RAM, which starves SSL.

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

// Stacks are heap-allocated and tasks are created AFTER SSL auth, so a deleted
// task frees a block adjacent to the SSL region and the two merge into one large
// contiguous block. TLS reconnection depends on that.

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
cygm_alarm_ext_t alarm_ext_settings;   // Versioned extension blob (see cgm_types.h)
alarm_config_t *editing_alarm = NULL;  // Pointer to alarm being edited
lv_obj_t *alarm_threshold_value_label = NULL;  // Reference to threshold value label for color updates

// Alarm state tracking (enum defined in shared_state.h)
volatile active_alarm_state_t current_alarm_state = ALARM_STATE_NONE;
bool alarm_acknowledged = false;

// Visual alarm components
lv_obj_t *visual_alarm_overlay = NULL;      // Flash overlay for visual alarm
lv_obj_t *visual_alarm_container = NULL;    // Glass panel for alarm dismiss UI
lv_obj_t *visual_alarm_disarm_btn = NULL;      // Hold-to-disarm button
lv_obj_t *visual_alarm_disarm_progress = NULL; // Progress bar inside disarm button
lv_obj_t *visual_alarm_disarm_label = NULL;    // Label on disarm button
static lv_obj_t *visual_alarm_snooze_btn = NULL;  // Giant SNOOZE button on the takeover
static int64_t disarm_press_start_ms = 0;      // Timestamp when button was pressed
#define DISARM_HOLD_MS 1500                     // Hold duration to disarm (1.5s)
#define ALARM_UNATTENDED_MS (30 * 60 * 1000)    // Untouched takeover auto-snoozes after 30 min
lv_timer_t *visual_alarm_timer = NULL;      // Timer for pulsing effect
volatile bool visual_alarm_active = false;
static uint32_t visual_alarm_shown_tick = 0;   // lv_tick when the takeover appeared
// Tier the on-screen takeover was built for. Title, colour and pulse rate are
// baked in at build time, so escalating tier must rebuild, not reuse, the overlay.
static active_alarm_state_t visual_alarm_shown_state = ALARM_STATE_NONE;
uint8_t visual_alarm_pulse_state = 0;       // 0 = dim, 1 = bright flash
static uint32_t visual_alarm_flash_color = COLOR_RED;  // Current alarm flash color
static lv_obj_t *visual_alarm_glucose_label = NULL;    // Glucose value label in alarm card (for live updates)
volatile int64_t alarm_snooze_until = 0;    // Unix timestamp when snooze expires

// Audio alarm repeat
static lv_timer_t *audio_alarm_timer = NULL;       // Timer for repeating audio
static alarm_config_t *active_alarm_config = NULL; // Current alarm being played

// Plays alarm tones off the LVGL render task: play_alarm_tone() blocks 0.5-4s
// and would hold the LVGL port lock that whole time. Pinned to Core 1 so LEDC
// stays on the same core as LED/backlight — cross-core LEDC ops crash the IWDT.
static TaskHandle_t alarm_buzzer_task_handle = NULL;
static volatile alarm_tone_t pending_alarm_tone = ALARM_TONE_BEEP_1;
static volatile uint8_t pending_alarm_volume = 80;

// Deferred alarm cleanup flags (Core 0 writes, Core 1 pulse timer reads)
static volatile bool visual_alarm_stop_requested = false;
static volatile bool visual_alarm_glucose_update_pending = false;
static volatile int  visual_alarm_pending_glucose = 0;
static volatile bool audio_alarm_stop_requested = false;

// ==================== Alarm Engine State ====================

// Which alert the visual/audio alarm represents. Glucose alerts use
// current_alarm_state; the others must not disturb the snooze/acknowledge
// machinery, so they carry their own title, colour and reason line here.
typedef enum {
    CYGM_ALERT_GLUCOSE = 0,   // threshold alarm — driven by current_alarm_state
    CYGM_ALERT_DATA_GAP,      // no fresh reading for N minutes
    CYGM_ALERT_PREDICT_LOW,   // projected to cross urgent low
    CYGM_ALERT_RATE,          // sustained rate of change
} cygm_alert_kind_t;

static cygm_alert_kind_t active_alert_kind  = CYGM_ALERT_GLUCOSE;
static alarm_config_t   *active_alert_cfg   = NULL;   // backs a non-glucose alert
static uint32_t          active_alert_color = COLOR_RED;
static char              active_alert_title[24]  = "";
static char              active_alert_reason[72] = "";

// Synthetic config for the data-gap alert (no user-facing alarm tier of its own)
static alarm_config_t data_gap_alarm_cfg;

// Audio volume state machine: urgent-low guard + escalation
static uint8_t alarm_base_volume    = 80;     // configured volume for this alert
static bool    alarm_audio_urgent   = false;  // urgent low: full volume, unmutable
static int64_t alarm_audio_start_ms = 0;      // uptime ms when this alert started
#define CYGM_ALARM_ESCALATE_STEP_PCT 15       // volume added per escalation step

// Post-alert suppression windows (uptime ms; 0 = armed)
static int64_t data_gap_suppress_until_ms = 0;
static int64_t predict_suppress_until_ms  = 0;
static int64_t rate_suppress_until_ms     = 0;
static bool    data_gap_alert_active      = false;

// Width the hold-to-dismiss progress bar fills to (button width minus padding)
static lv_coord_t disarm_progress_max_w = 0;

// Battery monitoring
adc_oneshot_unit_handle_t adc_handle = NULL;
adc_cali_handle_t adc_cali_handle = NULL;
float battery_voltage = 0.0f;
int battery_percent = 100;
bool battery_low_warning_shown = false;  // Track if low battery warning played
bool battery_present = true;             // Cleared once the ADC shows no cell fitted
lv_obj_t *battery_canvas = NULL;         // Gauge, or the plug icon when no cell
lv_obj_t *battery_percent_label = NULL;  // Battery percentage text label
lv_timer_t *battery_flash_timer = NULL;  // Timer for flashing red at low battery
lv_timer_t *battery_charging_fade_timer = NULL;  // Timer for fading charging icon

// Brightness control
uint8_t screen_brightness_percent = 80;  // Default 80%
uint8_t saved_brightness_percent = 80;   // Saved before alarm override
bool dim_while_charging = false;         // Plugged in = display stays on
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

// Glucose data, last-known-good: always show the last valid reading rather than
// blanking to "---" when a fetch fails on a transient network/API error.
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
lv_obj_t *label_clock_h = NULL;
lv_obj_t *label_clock_colon = NULL;
lv_obj_t *label_clock_m = NULL;
lv_obj_t *label_date = NULL;
lv_obj_t *label_ampm = NULL;  // AM/PM indicator (12hr mode only)
lv_obj_t *label_glucose = NULL;
lv_obj_t *trend_canvas = NULL;  // Canvas for custom-drawn trend arrow
lv_obj_t *label_time_ago = NULL;
lv_obj_t *label_unit = NULL;              // "mg/dL" unit label
lv_obj_t *glucose_freshness_arc = NULL;    // Arc showing glucose reading age (0-5 min)
bool glucose_fetch_active = false;         // True while a glucose fetch is in progress
lv_timer_t *glucose_timer_update = NULL;    // Timer to update the progress bar
int64_t last_glucose_fetch_time = 0;        // Timestamp of last glucose fetch (milliseconds)
bool glucose_fetch_in_progress = false;     // True when actively fetching glucose
bool glucose_fetch_failed = false;          // True if last fetch failed
bool glucose_force_fetch_requested = false; // True when user requested manual fetch
volatile int64_t glucose_next_fetch_ms = 0;  // esp_timer ms deadline of the next pull
volatile int     glucose_fetch_period_s = 90; // interval the deadline was armed with
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
bool user_glucose_mmol = false;  // false = mg/dL (US), true = mmol/L (rest of world)
bool user_date_dmy = false;      // false = US month-day, true = day-month

// ---- Locale glucose formatting helpers ----
// Glucose is stored canonical mg/dL; convert only at the display boundary.
// CONFIG_NEWLIB_NANO_FORMAT=y means printf has no %f, so mmol/L uses integer
// math: (mgdl*555 + 500)/1000 tenths, within 0.05 mmol across 40-400 mg/dL.
const char *cygm_glucose_unit(void) {
    return user_glucose_mmol ? "mmol/L" : "mg/dL";
}

// Non-static: also used by glucose_chart.c (declared in shared_state.h).
int mgdl_to_mmol_tenths(int mgdl) {
    return (mgdl * 555 + 500) / 1000;  // rounded tenths of mmol/L
}

// Format a glucose value (no unit): "100" in mg/dL, "5.6" in mmol/L.
void cygm_format_glucose(int mgdl, char *buf, size_t len) {
    if (user_glucose_mmol) {
        int t = mgdl_to_mmol_tenths(mgdl);
        snprintf(buf, len, "%d.%d", t / 10, t % 10);
    } else {
        snprintf(buf, len, "%d", mgdl);
    }
}

// Format a threshold value WITH unit: "100 mg/dL" or "5.6 mmol/L".
void cygm_format_threshold(int mgdl, char *buf, size_t len) {
    if (user_glucose_mmol) {
        int t = mgdl_to_mmol_tenths(mgdl);
        snprintf(buf, len, "%d.%d mmol/L", t / 10, t % 10);
    } else {
        snprintf(buf, len, "%d mg/dL", mgdl);
    }
}

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
lv_obj_t *expanded_time_h = NULL;
lv_obj_t *expanded_time_colon = NULL;
lv_obj_t *expanded_time_m = NULL;
lv_obj_t *expanded_temp_label = NULL;

// ===== HELPER FUNCTIONS =====

// Ticks of the 100ms timer between visually distinct frames, per weather code.
// Each condition quantises the frame counter differently, so redrawing on every
// tick would repaint identical pixels. Indexed by current_weather_code.
static const uint8_t weather_anim_stride[8] = {
    2,  // 0 clear:         ray rotation / star twinkle
    5,  // 1 partly cloudy: slow cloud drift only
    5,  // 2 overcast:      slow cloud drift only
    2,  // 3 rain:          drops step on frame/2
    4,  // 4 snow:          flakes step on frame/4
    2,  // 5 storm:         drops + lightning window
    5,  // 6 fog:           slow drift only
    3,  // 7 ice:           crystals step on frame/3
};

// Weather icon animation timer callback
void weather_icon_animation_timer_cb(lv_timer_t *timer) {
    if (!home_screen_active || home_weather_canvas == NULL) {
        return;
    }

    static uint8_t tick = 0;
    static int last_drawn_code = -1;

    int code = current_weather_code;
    uint8_t stride = (code >= 0 && code < 8) ? weather_anim_stride[code] : 4;

    // Only advance when the drawn frame would differ; a condition change repaints now.
    tick++;
    if (tick < stride && code == last_drawn_code) {
        return;
    }
    tick = 0;
    weather_icon_anim_frame++;

    // Redraw weather icon with new frame
    if (lvgl_port_lock(1)) {
        draw_weather_icon(home_weather_canvas, code);
        last_drawn_code = code;
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

                // Twinkling stars
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

// ==================== Trend Arrow ====================
//
// One parametric renderer: every trend is the same arrow at a different angle,
// so a trend change can be swept from the old angle to the new one. Angles are
// degrees, 0 = right, +90 = up; screen y grows downward, so the unit direction
// is (cos a, -sin a). Green = stable, orange = single, red = double. Every
// stroke is drawn twice: a wider low-opacity glow, then the solid core on top.

#define TA_ANGLE_NONE  999   // trend with no arrow to draw

// Geometry in a 60-unit design space; TA_S() scales it to the real canvas.
// Barbs are 45 deg so every segment is horizontal, vertical or an exact 1:1
// diagonal — the only cases this renderer draws cleanly with no anti-aliasing.
// Sizes are set against the ink's bounding CIRCLE, not its box: the glyph
// rotates and the canvas does not, so sizing to the upright box clips diagonals.
#define TA_TIP       23   // arrow point, from the centre
#define TA_TAIL      23   // shaft end, from the centre
#define TA_ARM       12   // chevron: setback along and half-width across the
                          // axis — equal is what makes the barbs 45 deg
#define TA_W          7   // stroke weight; forced odd after scaling, because an
                          // even width can flip sides mid-rotation and shimmer
#define TA_D_TIP     22   // doubles are two complete arrows side by side: each
#define TA_D_TAIL    22   // is shorter, with a smaller head and lighter stroke,
#define TA_D_ARM      9   // offset TA_D_OFF to each side of the axis, so the
#define TA_D_W        5   // pair's swept circle stays inside the single's.
#define TA_D_OFF     12
#define TA_GLOW       4   // glow stroke = core + this (a 2 px halo)

typedef struct {
    int cx, cy;        // canvas centre, px
    int32_t c, s;      // cos/sin of the angle, in 1/32768
    int tail, tip;     // shaft end / arrow point, from the centre, px
} trend_geom_t;

// The point `along` px down the axis and `across` px to its on-screen left.
static lv_point_t trend_pt(const trend_geom_t *g, int along, int across) {
    int32_t x = (int32_t)g->cx * 32768 + (int32_t)along * g->c - (int32_t)across * g->s;
    int32_t y = (int32_t)g->cy * 32768 - (int32_t)along * g->s - (int32_t)across * g->c;
    lv_point_t p;
    p.x = (lv_coord_t)((x >= 0) ? (x + 16384) / 32768 : -((16384 - x) / 32768));
    p.y = (lv_coord_t)((y >= 0) ? (y + 16384) / 32768 : -((16384 - y) / 32768));
    return p;
}

// The shaft, tail to point, `off` px left of the axis (0 for the single; the
// doubles draw one at -off and one at +off). It runs all the way to the point so
// the round caps of the shaft and both barbs land on the same pixel.
static void trend_shaft(lv_obj_t *canvas, const trend_geom_t *g, int off,
                        const lv_draw_line_dsc_t *dsc) {
    lv_point_t seg[2];
    seg[0] = trend_pt(g, -g->tail, off);
    seg[1] = trend_pt(g,  g->tip,  off);
    lv_canvas_draw_line(canvas, seg, 2, dsc);
}

// Two barbs back from `apex`, `arm` units along the axis and the same across it,
// which puts them at 45 deg. Left open: a closed outline at this stroke weight
// fills itself in, and two mirrored segments rotate without a lumpy third edge.
static void trend_chevron(lv_obj_t *canvas, const trend_geom_t *g, int apex, int arm,
                          int off, const lv_draw_line_dsc_t *dsc) {
    lv_point_t seg[2];
    lv_point_t tip = trend_pt(g, apex, off);

    seg[0] = tip; seg[1] = trend_pt(g, apex - arm, off + arm);
    lv_canvas_draw_line(canvas, seg, 2, dsc);
    seg[0] = tip; seg[1] = trend_pt(g, apex - arm, off - arm);
    lv_canvas_draw_line(canvas, seg, 2, dsc);
}

static int trend_angle_of(dexcom_trend_t trend) {
    switch (trend) {
        case TREND_DOUBLE_UP:
        case TREND_SINGLE_UP:       return 90;
        case TREND_FORTY_FIVE_UP:   return 45;
        case TREND_FLAT:            return 0;
        case TREND_FORTY_FIVE_DOWN: return -45;
        case TREND_SINGLE_DOWN:
        case TREND_DOUBLE_DOWN:     return -90;
        default:                    return TA_ANGLE_NONE;
    }
}

// Doubles pulse their glow and slide side to side (the wiggle timer below
// writes both); everything else glows at the constant subtle level, unshifted.
#define TA_GLOW_OPA_BASE  LV_OPA_30
static lv_opa_t ta_glow_opa = TA_GLOW_OPA_BASE;
static int      ta_wig_shift = 0;   // design units, across the axis

// Renders `trend` at an arbitrary angle: colour and single/pair form come from
// the trend, direction from the angle, so a sweep can pass through angles no
// trend maps to.
static void trend_render(lv_obj_t *canvas, dexcom_trend_t trend, int angle, int sz) {
    if (canvas == NULL || sz <= 0) return;

    uint32_t ink_hex;
    bool twin;
    switch (trend) {
        case TREND_FLAT:
        case TREND_FORTY_FIVE_UP:
        case TREND_FORTY_FIVE_DOWN:
            ink_hex = 0x10B981; twin = false; break;  // Green — stable
        case TREND_SINGLE_UP:
        case TREND_SINGLE_DOWN:
            ink_hex = 0xF59E0B; twin = false; break;  // Orange — warning
        case TREND_DOUBLE_UP:
        case TREND_DOUBLE_DOWN:
            ink_hex = 0xEF4444; twin = true;  break;  // Red — alarm
        default:
            return;
    }

    #define TA_S(v) ((v) * sz / 60)

    trend_geom_t g;
    g.cx = sz / 2;
    g.cy = sz / 2;
    g.c  = lv_trigo_cos((int16_t)angle);
    g.s  = lv_trigo_sin((int16_t)angle);
    g.tail = TA_S(twin ? TA_D_TAIL : TA_TAIL);
    g.tip  = TA_S(twin ? TA_D_TIP  : TA_TIP);

    int arm = TA_S(twin ? TA_D_ARM : TA_ARM);
    int w   = TA_S(twin ? TA_D_W   : TA_W);
    int off = twin ? TA_S(TA_D_OFF) : 0;
    if (w < 3)   w = 3;
    if (arm < 3) arm = 3;
    if ((w & 1) == 0) w++;   // odd stroke only — even shimmers in rotation

    lv_draw_line_dsc_t glow, core;
    lv_draw_line_dsc_init(&glow);
    glow.round_start = 1;
    glow.round_end = 1;
    glow.width = w + TA_S(TA_GLOW);
    glow.opa = twin ? ta_glow_opa : TA_GLOW_OPA_BASE;
    glow.color = lv_color_hex(ink_hex);
    core = glow;
    core.width = w;
    core.opa = LV_OPA_COVER;

    // All glow strokes first, cores on top, so no glow smears over a core.
    int shift = twin ? TA_S(ta_wig_shift) : 0;   // the alert slide, whole pair
    for (int pass = 0; pass < 2; pass++) {
        const lv_draw_line_dsc_t *d = pass ? &core : &glow;
        for (int side = twin ? -1 : 0; side <= (twin ? 1 : 0); side += 2) {
            trend_shaft(canvas, &g, side * off + shift, d);
            trend_chevron(canvas, &g, g.tip, arm, side * off + shift, d);
        }
    }

    #undef TA_S
}

// ---- Rotation sweep on a trend change ----
// No lv_anim: LVGL animations freeze this hardware. The safe idiom is a
// short-lived lv_timer that steps a value, redraws, and deletes itself. State is
// file statics, nothing is allocated but the timer, and every entry point runs
// under the LVGL lock.
#define TA_TICK_MS  25
#define TA_STEPS    12   // 12 x 25ms = 300ms

// Canvas fill behind the arrow strokes: the glow pass bakes this colour into
// its pixels, so it must match what actually sits behind the canvas.
static lv_color_t trend_fill_bg(void) {
    return lv_color_hex(home_night_face_is_active() ? 0x000000 : COLOR_CARD_BG);
}

static lv_timer_t     *ta_timer  = NULL;
static lv_obj_t       *ta_canvas = NULL;  // canvas the state below describes
static dexcom_trend_t  ta_trend  = TREND_NONE;
static int  ta_size = 0;
static int  ta_from = 0;     // sweep start angle
static int  ta_to   = 0;     // sweep end angle
static int  ta_cur  = 0;     // angle currently on the canvas
static int  ta_step = 0;
static bool ta_drawn = false; // ta_cur / ta_trend describe real pixels

static void trend_wiggle_stop(void);
static void trend_wiggle_sync(void);

static void trend_anim_stop(void) {
    trend_wiggle_stop();
    if (ta_timer != NULL) {
        lv_timer_t *t = ta_timer;
        ta_timer = NULL;   // cleared first so a re-entrant call sees no timer
        lv_timer_del(t);
    }
}

// For when someone else wipes the arrow off the canvas (the stale path), so the
// next real reading redraws instead of rotating up from nothing.
static void trend_anim_reset(void) {
    trend_anim_stop();
    ta_drawn = false;
}

static void trend_anim_tick_cb(lv_timer_t *t) {
    if (ta_canvas == NULL || ta_size <= 0) {   // never expected; fail closed
        if (ta_timer == t) ta_timer = NULL;
        lv_timer_del(t);
        return;
    }

    ta_step++;
    if (ta_step >= TA_STEPS) {
        ta_cur = ta_to;
    } else {
        // Ease-out p = 2u - u^2 for u = step/TA_STEPS: biggest steps first.
        int32_t p = 2 * TA_STEPS * ta_step - ta_step * ta_step;  // 0..STEPS^2
        ta_cur = ta_from + (int)(((int32_t)(ta_to - ta_from) * p) / (TA_STEPS * TA_STEPS));
    }

    lv_canvas_fill_bg(ta_canvas, trend_fill_bg(), LV_OPA_0);
    trend_render(ta_canvas, ta_trend, ta_cur, ta_size);

    if (ta_step >= TA_STEPS) {
        if (ta_timer == t) ta_timer = NULL;
        lv_timer_del(t);   // LVGL supports a timer deleting itself
        trend_wiggle_sync();   // a double landing starts its alert motion
    }
}

// States where the sweep is pointless or invisible — land on the new angle in
// one frame instead.
static bool trend_anim_skip(lv_obj_t *canvas) {
    if (lv_obj_has_flag(canvas, LV_OBJ_FLAG_HIDDEN)) return true;
    if (lv_obj_get_screen(canvas) != lv_scr_act()) return true;
    // The night face dims the arrow rather than hiding it, and a rotating arrow
    // is the last thing that belongs on a sleeping face.
    if (lv_obj_get_style_img_opa(canvas, LV_PART_MAIN) < LV_OPA_COVER) return true;
    return false;
}

// ---- Double-trend shift + glow pulse ----
// The double forms are the urgent ones, so the settled pair is itself a visual
// alert: it slides side to side while the glow breathes in the same rhythm.
// No lv_anim — an lv_timer stepping file statics. Runs only while a double is
// the settled trend on a visible, undimmed canvas; every tick re-checks and
// shuts itself down the moment that stops being true.
#define TA_WIG_TICK_MS 80
#define TA_WIG_STEPS   16   // 16 x 80ms = 1.28s per full slide cycle

// Design units, scaled by TA_S like every other measure
static const int8_t   ta_wig_slide[TA_WIG_STEPS] =
    { 0,  1,  2,  3,  3,  3,  2,  1,  0, -1, -2, -3, -3, -3, -2, -1 };
static const lv_opa_t ta_wig_opa[TA_WIG_STEPS] =
    { 26, 64, 110, 150, 179, 150, 110, 64, 26, 64, 110, 150, 179, 150, 110, 64 };

static lv_timer_t *ta_wig_timer = NULL;
static uint8_t     ta_wig_phase = 0;

static bool trend_is_double(dexcom_trend_t t) {
    return t == TREND_DOUBLE_UP || t == TREND_DOUBLE_DOWN;
}

static void trend_wiggle_stop(void) {
    if (ta_wig_timer != NULL) {
        lv_timer_t *t = ta_wig_timer;
        ta_wig_timer = NULL;
        lv_timer_del(t);
        ESP_LOGI(TAG, "WIGGLE: stop");
    }
    ta_glow_opa = TA_GLOW_OPA_BASE;
    ta_wig_shift = 0;
}

static void trend_wiggle_tick_cb(lv_timer_t *t) {
    if (ta_canvas == NULL || ta_size <= 0 || !trend_is_double(ta_trend) ||
        trend_anim_skip(ta_canvas)) {
        if (ta_wig_timer == t) ta_wig_timer = NULL;
        ta_glow_opa = TA_GLOW_OPA_BASE;
        ta_wig_shift = 0;
        lv_timer_del(t);
        ESP_LOGI(TAG, "WIGGLE: self-stop (canvas gone, trend changed, or skip)");
        return;
    }
    ta_wig_phase = (uint8_t)((ta_wig_phase + 1) % TA_WIG_STEPS);
    ta_glow_opa = ta_wig_opa[ta_wig_phase];
    ta_wig_shift = ta_wig_slide[ta_wig_phase];
    lv_canvas_fill_bg(ta_canvas, trend_fill_bg(), LV_OPA_0);
    trend_render(ta_canvas, ta_trend, ta_cur, ta_size);
}

// Called wherever the arrow lands settled on its target angle.
static void trend_wiggle_sync(void) {
    if (trend_is_double(ta_trend) && ta_canvas != NULL && ta_size > 0 &&
        !trend_anim_skip(ta_canvas)) {
        if (ta_wig_timer == NULL) {
            ta_wig_phase = 0;
            ta_wig_timer = lv_timer_create(trend_wiggle_tick_cb, TA_WIG_TICK_MS, NULL);
            ESP_LOGI(TAG, "WIGGLE: start (trend=%d) %s", (int)ta_trend,
                     ta_wig_timer ? "ok" : "NO HEAP");
        }
    } else {
        trend_wiggle_stop();
    }
}

// Callers clear the canvas first, then call this. Must run under the LVGL lock:
// it can create a timer.
void draw_trend_arrow_sized(lv_obj_t *canvas, dexcom_trend_t trend, int sz) {
    if (canvas == NULL || sz <= 0) return;

    int target = trend_angle_of(trend);
    if (target == TA_ANGLE_NONE) {   // no arrow for this trend; leave it clear
        trend_anim_reset();
        return;
    }

    // A different canvas, or a resize from expand/collapse, invalidates the
    // sweep — its start angle describes pixels that are already gone.
    if (canvas != ta_canvas || sz != ta_size) {
        trend_anim_stop();
        ta_canvas = canvas;
        ta_size = sz;
        ta_drawn = false;
    }

    if (!ta_drawn) {                 // first paint: no previous angle to leave
        ta_trend = trend;
        ta_cur = target;
        ta_drawn = true;
        trend_render(canvas, trend, target, sz);
        trend_wiggle_sync();
        return;
    }

    if (trend == ta_trend) {         // repaint of what is already up
        // ta_wig_shift persists between wiggle ticks, so this repaints the
        // slide's current frame; with no wiggle running the shift is 0.
        trend_render(canvas, trend, ta_cur, sz);
        if (ta_wig_timer == NULL) {
            trend_wiggle_sync();  // restarts a double stopped while off-screen
        }
        return;
    }

    trend_wiggle_stop();             // motion state belongs to the old trend
    ESP_LOGI(TAG, "ARROW: trend %d -> %d (cur=%d target=%d)",
             (int)ta_trend, (int)trend, ta_cur, target);
    ta_trend = trend;                // colour and form follow the new trend now

    if (ta_cur == target || trend_anim_skip(canvas)) {
        trend_anim_stop();
        ta_cur = target;
        trend_render(canvas, trend, target, sz);
        trend_wiggle_sync();
        return;
    }

    // A trend arriving mid-sweep retargets from the interpolated angle rather
    // than snapping back to the previous trend's angle.
    ta_from = ta_cur;
    ta_to   = target;
    ta_step = 0;
    if (ta_timer == NULL) {
        ta_timer = lv_timer_create(trend_anim_tick_cb, TA_TICK_MS, NULL);
        if (ta_timer == NULL) ta_cur = target;   // no heap for it: land now
    }
    trend_render(canvas, trend, ta_cur, sz);
}

void draw_trend_arrow(lv_obj_t *canvas, dexcom_trend_t trend) {
    draw_trend_arrow_sized(canvas, trend, 60);
}

// ==================== Time & SNTP Functions ====================
// Moved to features/time_system.c

// ==================== Weather Functions ====================
// Moved to features/weather_system.c

// ==================== LVGL Lock Helper ====================

// Patience comes from looping 1ms try-locks, never from a longer timeout:
// lvgl_port_lock() timeouts above 1ms risk a FreeRTOS SMP assert crash.
static bool lvgl_port_lock_retry(int max_attempts) {
    for (int i = 0; i < max_attempts; i++) {
        if (lvgl_port_lock(1)) return true;
        // Yield between attempts so the render task can actually RELEASE the
        // mutex; tight non-yielding retries burn the budget and lose every race.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

// ==================== Trend Demo Ticker ====================
//
// Serial "demo on" (hardware/screenshot.c) walks the arrow through the trend
// forms without waiting on the sensor. Each move writes current_trend and
// repaints — the same two steps a real reading performs — and touches nothing
// else: no glucose value, no history point, no alarm evaluation. The timer
// only exists while the flag is set, so an idle device pays nothing.

#define DEMO_TICK_MS     1000
#define DEMO_HOLD_TICKS  10     // a new direction every 10s

static lv_timer_t *demo_timer = NULL;
static int demo_ticks = 0;

// One of the six drawable trends other than the one on screen. Built as a pool
// and indexed rather than retried until different, so this can never spin.
static dexcom_trend_t demo_next_trend(dexcom_trend_t cur) {
    dexcom_trend_t pool[TREND_DOUBLE_DOWN - TREND_DOUBLE_UP + 1];
    int n = 0;
    for (int t = TREND_DOUBLE_UP; t <= TREND_DOUBLE_DOWN; t++) {
        if ((dexcom_trend_t)t != cur) pool[n++] = (dexcom_trend_t)t;
    }
    return pool[esp_random() % (uint32_t)n];
}

static void demo_tick_cb(lv_timer_t *t) {
    if (!cygm_demo_trend_active()) {   // covers "demo off" and the 30-minute expiry
        ESP_LOGI(TAG, "DEMO: inactive - ticker released");
        if (demo_timer == t) demo_timer = NULL;
        lv_timer_del(t);               // LVGL supports a timer deleting itself
        return;
    }

    // Off the home screen there is no arrow to drive; keep counting so returning
    // to it does not stall on a fresh 10s wait.
    if (++demo_ticks < DEMO_HOLD_TICKS) return;
    if (!home_screen_active || trend_canvas == NULL) {
        if (demo_ticks % DEMO_HOLD_TICKS == 0) {
            ESP_LOGW(TAG, "DEMO: blocked (home=%d canvas=%p)",
                     (int)home_screen_active, (void *)trend_canvas);
        }
        return;
    }
    demo_ticks = 0;

    dexcom_trend_t prev = current_trend;
    current_trend = demo_next_trend(current_trend);
    ESP_LOGI(TAG, "DEMO: trend %d -> %d", (int)prev, (int)current_trend);
    lv_canvas_fill_bg(trend_canvas, trend_fill_bg(), LV_OPA_0);
    draw_trend_arrow_sized(trend_canvas, current_trend, cgm_expanded ? 80 : 60);
}

void cygm_demo_trend_sync(void) {
    // The command task calls this, so the lock is taken here rather than assumed.
    if (!lvgl_port_lock_retry(20)) {
        ESP_LOGW(TAG, "Trend demo: LVGL busy — ticker unchanged, retry the command");
        return;
    }

    bool want = cygm_demo_trend_active();
    if (want && demo_timer == NULL) {
        demo_ticks = DEMO_HOLD_TICKS - 1;   // first tick moves the arrow at once
        demo_timer = lv_timer_create(demo_tick_cb, DEMO_TICK_MS, NULL);
        if (demo_timer == NULL) ESP_LOGE(TAG, "Trend demo: no heap for the ticker");
        else ESP_LOGI(TAG, "DEMO: ticker created");
    } else if (want) {
        ESP_LOGI(TAG, "DEMO: ticker already running");
    } else if (!want && demo_timer != NULL) {
        lv_timer_t *t = demo_timer;
        demo_timer = NULL;
        lv_timer_del(t);
        ESP_LOGI(TAG, "DEMO: ticker deleted");
    }

    lvgl_port_unlock();
}

// ==================== Alarm Functions ====================

// Forward declarations for alarm helpers
static void start_visual_alarm(void);
static void start_audio_alarm(alarm_config_t *alarm, bool urgent);
static void trigger_alarm(active_alarm_state_t state);
static void visual_alarm_pulse_timer_cb(lv_timer_t *timer);
static void visual_alarm_delete_cb(lv_event_t *e);
static void audio_alarm_repeat_timer_cb(lv_timer_t *timer);
static void visual_alarm_cleanup_lvgl(void);

static uint32_t get_glucose_alarm_color(int glucose_mg_dl);
void check_glucose_alarms(int glucose_mg_dl);
void update_glucose_display(void);
static void visual_alarm_disarm_event_cb(lv_event_t *e);
static void visual_alarm_snooze_event_cb(lv_event_t *e);
static void alarm_snooze_and_close(void);
static bool raise_alert(cygm_alert_kind_t kind, alarm_config_t *cfg,
                        const char *title, const char *reason, uint32_t color);

// ==================== Alarm Engine Settings ====================

// Factory defaults for the versioned extension blob. nvs_config.c starts from
// these before overlaying a stored blob, so an older/shorter blob keeps sane
// values for the fields it does not carry instead of resetting everything.
void cygm_alarm_ext_defaults(cygm_alarm_ext_t *ext) {
    if (ext == NULL) return;
    memset(ext, 0, sizeof(*ext));
    ext->version             = CYGM_ALARM_EXT_VERSION;
    ext->size                = (uint16_t)sizeof(cygm_alarm_ext_t);
    ext->quiet_enabled       = 0;    // opt-in: silence is never the default
    ext->quiet_start_hour    = 22;
    ext->quiet_start_min     = 0;
    ext->quiet_end_hour      = 7;
    ext->quiet_end_min       = 0;
    ext->escalate_enabled    = 1;
    ext->escalate_step_min   = 3;
    ext->escalate_max_volume = 100;
    ext->gap_enabled         = 1;
    ext->gap_minutes         = 20;   // warns well before the 30-min stale cutoff
    ext->gap_tone            = (uint8_t)ALARM_TONE_ASCENDING;  // distinct from every threshold default
    ext->gap_volume          = 70;
    ext->predict_enabled     = 1;
    ext->predict_horizon_min = 20;
    ext->rate_enabled        = 0;    // opt-in: noisy on real sensor data
    ext->rate_threshold_x10  = 30;   // 3.0 mg/dL per minute
    ext->suppress_min        = 30;
    ext->snooze_default_min  = 30;
    ext->urgent_low_floor    = 55;
    ext->auto_snooze_disabled = 0;  // stored inverted so zero-filled old blobs stay ON
}

// Quiet hours. The window may wrap midnight. Returns false while the clock is
// unsynced — a device that booted to 1970 must never mute itself.
bool cygm_quiet_hours_active(void) {
    if (!alarm_ext_settings.quiet_enabled) return false;
    if (!is_time_synced()) return false;

    time_t now;
    time(&now);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    int cur   = tm_now.tm_hour * 60 + tm_now.tm_min;
    int start = alarm_ext_settings.quiet_start_hour * 60 + alarm_ext_settings.quiet_start_min;
    int end   = alarm_ext_settings.quiet_end_hour   * 60 + alarm_ext_settings.quiet_end_min;

    if (start == end) return false;                    // zero-length window = off
    if (start < end)  return (cur >= start && cur < end);
    return (cur >= start || cur < end);                // wraps midnight
}

// The urgent low tier is non-disableable: when the user has switched the low
// alarm off entirely, the configured safety floor still applies.
int cygm_urgent_low_threshold(void) {
    int floor_mgdl = alarm_ext_settings.urgent_low_floor;
    if (floor_mgdl < 40 || floor_mgdl > 90) floor_mgdl = 55;
    if (!current_alarm_settings.low_alarm.enabled) return floor_mgdl;
    return current_alarm_settings.low_alarm.threshold;
}

// Volume actually handed to the buzzer for this repeat. Urgent low is pinned at
// full volume; otherwise the configured level steps up while the alert stays
// unacknowledged.
static uint8_t alarm_current_volume(void) {
    if (alarm_audio_urgent) return 100;

    int vol = alarm_base_volume;
    if (alarm_ext_settings.escalate_enabled && alarm_audio_start_ms > 0) {
        int step_min = alarm_ext_settings.escalate_step_min;
        if (step_min < 1 || step_min > 30) step_min = 3;
        int64_t elapsed_min = ((esp_timer_get_time() / 1000) - alarm_audio_start_ms) / 60000;
        int steps = (int)(elapsed_min / step_min);
        if (steps > 0) {
            int cap = alarm_ext_settings.escalate_max_volume;
            if (cap < 10 || cap > 100) cap = 100;
            vol += steps * CYGM_ALARM_ESCALATE_STEP_PCT;
            if (vol > cap) vol = cap;
        }
    }
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    return (uint8_t)vol;
}

// Plain-language cause for the data-gap message, from state we already track.
static const char *data_gap_cause(void) {
    if (!wifi_connected) return "WiFi is down";
    if (glucose_status == GLUCOSE_STATUS_NOT_AUTHENTICATED) return "not signed in";
    if (glucose_fetch_failed) return "service unreachable";
    if (glucose_status == GLUCOSE_STATUS_WARMUP) return "sensor warming up";
    return "sensor not reporting";
}

// Least-squares slope over the newest history points, in TENTHS of mg/dL per
// minute (negative = falling). Returns false when the data cannot support a
// trustworthy slope: too few points, a hole in the series, or too short a span.
#define CYGM_SLOPE_POINTS       4
#define CYGM_SLOPE_MIN_POINTS   3
#define CYGM_SLOPE_MAX_GAP_MIN  12   // reject a window containing a hole this wide
#define CYGM_SLOPE_MIN_SPAN_MIN  8   // reject a window shorter than this

static bool glucose_slope_tenths(int *slope_tenths_out, int *newest_mgdl_out) {
    int count = glucose_history_get_count();
    if (count < CYGM_SLOPE_MIN_POINTS) return false;

    int n = (count < CYGM_SLOPE_POINTS) ? count : CYGM_SLOPE_POINTS;
    int first = count - n;

    const glucose_history_point_t *oldest = glucose_history_get_at(first);
    if (oldest == NULL || !oldest->valid) return false;
    int64_t base_ms = oldest->timestamp;

    int64_t t[CYGM_SLOPE_POINTS];   // whole minutes since the window start
    int64_t g[CYGM_SLOPE_POINTS];   // mg/dL
    int64_t prev_ms = base_ms;

    for (int i = 0; i < n; i++) {
        const glucose_history_point_t *p = glucose_history_get_at(first + i);
        if (p == NULL || !p->valid) return false;
        if (i > 0 && (p->timestamp - prev_ms) > (int64_t)CYGM_SLOPE_MAX_GAP_MIN * 60000) return false;
        prev_ms = p->timestamp;
        t[i] = (p->timestamp - base_ms) / 60000;
        g[i] = p->glucose;
    }

    if (t[n - 1] < CYGM_SLOPE_MIN_SPAN_MIN) return false;

    int64_t sum_t = 0, sum_g = 0, sum_tg = 0, sum_tt = 0;
    for (int i = 0; i < n; i++) {
        sum_t  += t[i];
        sum_g  += g[i];
        sum_tg += t[i] * g[i];
        sum_tt += t[i] * t[i];
    }

    int64_t den = (int64_t)n * sum_tt - sum_t * sum_t;
    if (den == 0) return false;
    int64_t num = (int64_t)n * sum_tg - sum_t * sum_g;

    *slope_tenths_out = (int)((num * 10) / den);
    *newest_mgdl_out  = (int)g[n - 1];
    return true;
}

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

    // A real threshold alarm outranks any advisory alert already on screen, and
    // a tier change must rebuild too: escalating LOW WARNING -> LOW ALARM would
    // otherwise leave the urgent tone under a takeover still labelled "LOW WARNING".
    if (active_alert_kind != CYGM_ALERT_GLUCOSE ||
        (visual_alarm_active && visual_alarm_shown_state != state)) {
        stop_visual_alarm();
        stop_audio_alarm();
    }

    // The urgent low tier is non-disableable: it ignores snooze, quiet hours,
    // the per-alarm audio toggle and the configured volume.
    bool urgent = (state == ALARM_STATE_LOW_ALARM);

    ESP_LOGW(TAG, "ALARM TRIGGERED: %s (threshold: %d mg/dL)",
             alarm_name, alarm->threshold);
    sd_log(TAG, "ALARM: %s triggered, glucose=%d threshold=%d audio=%d visual=%d urgent=%d",
           alarm_name, current_glucose, alarm->threshold,
           alarm->audio_enabled, alarm->visual_enabled, urgent);

    // Check if snooze is active
    time_t now;
    time(&now);
    if (!urgent && alarm_snooze_until > 0 && now < alarm_snooze_until) {
        ESP_LOGI(TAG, "Alarm snoozed until %ld (current: %ld)", (long)alarm_snooze_until, (long)now);
        return;
    }

    // Quiet hours downgrade a non-urgent alert to visual + LED only
    bool quiet = !urgent && cygm_quiet_hours_active();
    if (quiet) {
        ESP_LOGI(TAG, "Quiet hours active — %s is visual/LED only", alarm_name);
        sd_log(TAG, "ALARM: quiet hours, %s visual only", alarm_name);
    }

    active_alert_kind = CYGM_ALERT_GLUCOSE;
    active_alert_cfg = NULL;

    // Audio alert (if enabled and not acknowledged)
    if ((alarm->audio_enabled || urgent) && !alarm_acknowledged && !quiet) {
        start_audio_alarm(alarm, urgent);
    }

    // The urgent low forces the takeover even when the tier's visual toggle is
    // off: it is the only SNOOZE/DISMISS surface for the forced audio, and the
    // only host for the pulse timer that runs the deferred-work queue.
    if ((alarm->visual_enabled || urgent) && !alarm_acknowledged) {
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

    // The urgent low tier breaks through an active snooze (non-disableable).
    bool urgent_low = (new_state == ALARM_STATE_LOW_ALARM);

    // Check snooze status
    if (alarm_snooze_until > 0) {
        time_t now;
        time(&now);

        if (now < alarm_snooze_until && !urgent_low) {
            // Still snoozed. Keep tracking the tier anyway: a frozen
            // current_alarm_state would make recovery-then-recrash look like "no
            // change", and the urgent-low gate below would swallow the new alarm.
            current_alarm_state = new_state;
            int minutes_remaining = (alarm_snooze_until - now) / 60;
            int seconds_remaining = (alarm_snooze_until - now) % 60;
            ESP_LOGI(TAG, "Alarm snoozed for %d min %d sec more (until %ld, current %ld)",
                     minutes_remaining, seconds_remaining, (long)alarm_snooze_until, (long)now);
            return;  // Exit early - alarm is snoozed
        } else if (now < alarm_snooze_until) {
            // Urgent low during a snooze. Sound it, but only on ENTRY into the
            // urgent tier so the snooze still damps repeats of the same alarm.
            if (current_alarm_state != new_state) {
                ESP_LOGW(TAG, "Urgent low overrides active snooze (glucose=%d)", glucose_mg_dl);
                sd_log(TAG, "ALARM: urgent low overrides snooze, glucose=%d", glucose_mg_dl);
                current_alarm_state = new_state;
                alarm_acknowledged = false;
                trigger_alarm(new_state);
            }
            return;
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
            alarm_snooze_until = 0;  // Clear stale snooze to prevent suppressing future alarms
            stop_visual_alarm();
            stop_audio_alarm();
        }
    }
}

// Pulses the alarm border and drains the deferred-work queue Core 0 posts when it
// cannot get the LVGL lock. Runs in LVGL task context (Core 1), lock already held.
static void visual_alarm_pulse_timer_cb(lv_timer_t *timer) {
    // ---- Deferred stop: Core 0 requested cleanup but couldn't get LVGL lock ----
    if (visual_alarm_stop_requested) {
        ESP_LOGI(TAG, "Pulse timer: processing deferred alarm cleanup");
        visual_alarm_active = false;
        visual_alarm_stop_requested = false;
        visual_alarm_glucose_update_pending = false;
        // Clean up deferred audio timer too
        if (audio_alarm_stop_requested && audio_alarm_timer != NULL) {
            lv_timer_del(audio_alarm_timer);
            audio_alarm_timer = NULL;
            audio_alarm_stop_requested = false;
        }
        // A newer alarm may have started audio while this teardown was pending and
        // lost its lock. Last tick of this timer: create it here or lose the tone.
        if (active_alarm_config != NULL && audio_alarm_timer == NULL && !audio_alarm_stop_requested) {
            audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
            lv_timer_ready(audio_alarm_timer);
            ESP_LOGI(TAG, "Pulse timer: created deferred audio timer during teardown");
        }
        visual_alarm_cleanup_lvgl();  // Deletes this timer — must return immediately
        return;
    }

    // ---- Deferred audio timer cleanup (stop_audio_alarm lock failed) ----
    if (audio_alarm_stop_requested && audio_alarm_timer != NULL) {
        lv_timer_del(audio_alarm_timer);
        audio_alarm_timer = NULL;
        audio_alarm_stop_requested = false;
        ESP_LOGI(TAG, "Pulse timer: cleaned up deferred audio timer");
    }

    // ---- Deferred audio timer creation (start_audio_alarm lock failed) ----
    if (active_alarm_config != NULL && audio_alarm_timer == NULL && !audio_alarm_stop_requested) {
        audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
        lv_timer_ready(audio_alarm_timer);
        ESP_LOGI(TAG, "Pulse timer: created deferred audio timer");
    }

    if (!visual_alarm_active || visual_alarm_overlay == NULL) {
        return;
    }

    // ---- Deferred glucose update (update_visual_alarm_glucose lock-free path) ----
    if (visual_alarm_glucose_update_pending && visual_alarm_glucose_label != NULL) {
        char buf[16];
        cygm_format_glucose(visual_alarm_pending_glucose, buf, sizeof(buf));
        lv_label_set_text(visual_alarm_glucose_label, buf);
        visual_alarm_glucose_update_pending = false;
    }

    // Auto-clear if glucose data has gone stale (e.g., Dexcom session expired overnight).
    // Skipped for the data-gap alert, which exists precisely BECAUSE data is stale.
    if (active_alert_kind != CYGM_ALERT_DATA_GAP && glucose_data_valid && glucose_timestamp > 0) {
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

    // Unattended takeover: the alarm has been up 30 minutes AND nothing has
    // touched the screen for 30 minutes. Stand down as a snooze — the safe
    // action, since it re-arms and re-fires if the reading is still out of
    // range — and give the display back to the home screen instead of pulsing
    // at an empty room all night. Opt-out lives behind a hold in Alert Options.
    if (!alarm_ext_settings.auto_snooze_disabled &&
        lv_tick_elaps(visual_alarm_shown_tick) >= ALARM_UNATTENDED_MS &&
        lv_disp_get_inactive_time(NULL) >= ALARM_UNATTENDED_MS) {
        ESP_LOGW(TAG, "Alarm unattended for 30 min - auto-snoozing, returning home");
        sd_log(TAG, "ALARM: unattended 30min, auto-snooze glucose=%d state=%d",
               current_glucose, current_alarm_state);
        alarm_snooze_and_close();  // Deletes this timer — must return immediately
        return;
    }

    // Pulse the border (already in LVGL context — lock held via recursive mutex, no inner lock needed)
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
}

// Core-1 task that actually plays the (blocking) tone, signalled by the repeat
// timer. Keeps the multi-second vTaskDelay sequence OUT of the LVGL task.
static void alarm_buzzer_task(void *arg) {
    (void)arg;
    while (1) {
        // Wait for a "play now" signal from audio_alarm_repeat_timer_cb.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!audio_alarm_stop_requested) {
            play_alarm_tone(pending_alarm_tone, pending_alarm_volume);
        }
    }
}

// Repeat timer, in LVGL task context (Core 1). Never plays the tone here — that
// would hold the LVGL lock 0.5-4s; it notifies alarm_buzzer_task and returns.
static void audio_alarm_repeat_timer_cb(lv_timer_t *timer) {
    // Unattended tone-only alarm (tier has Screen Flash off, so there is no
    // takeover and no pulse timer to host the timeout) — same 30-min stand-down
    // as the takeover path, riding this timer instead.
    if (!visual_alarm_active && active_alarm_config != NULL &&
        !alarm_ext_settings.auto_snooze_disabled &&
        alarm_audio_start_ms > 0 &&
        (esp_timer_get_time() / 1000) - alarm_audio_start_ms >= ALARM_UNATTENDED_MS &&
        lv_disp_get_inactive_time(NULL) >= ALARM_UNATTENDED_MS) {
        ESP_LOGW(TAG, "Tone-only alarm unattended for 30 min - auto-snoozing");
        sd_log(TAG, "ALARM: tone-only unattended 30min, auto-snooze glucose=%d state=%d",
               current_glucose, current_alarm_state);
        alarm_snooze_and_close();  // Deletes this timer — must return immediately
        return;
    }

    if (active_alarm_config != NULL && (active_alarm_config->audio_enabled || alarm_audio_urgent)) {
        // Volume is recomputed every repeat: the urgent-low guard pins it at
        // full, escalation steps it up while the alert stays unacknowledged.
        uint8_t vol = alarm_current_volume();
        ESP_LOGI(TAG, "Playing alarm tone: %d at volume %d%%", active_alarm_config->tone, vol);
        pending_alarm_tone = active_alarm_config->tone;
        pending_alarm_volume = vol;
        if (alarm_buzzer_task_handle != NULL) {
            xTaskNotifyGive(alarm_buzzer_task_handle);
        }

        // If repeat is disabled, this was a one-shot — clean up
        if (!active_alarm_config->audio_repeat) {
            active_alarm_config = NULL;
            audio_alarm_timer = NULL;
            lv_timer_del(timer);  // Safe: already in LVGL context
        }
    }
}

// Never calls play_alarm_tone() directly: this may run on Core 0, and driving the
// buzzer from Core 0 contends with LED/backlight LEDC on Core 1 and trips the
// Interrupt WDT. An LVGL timer plays the tone on Core 1's next cycle instead.
static void start_audio_alarm(alarm_config_t *alarm, bool urgent) {
    if (alarm == NULL) return;
    if (!alarm->audio_enabled && !urgent) {
        return;  // urgent low ignores the per-alarm mute
    }

    // Stop any existing audio alarm (also clears the escalation state below)
    stop_audio_alarm();

    // Save reference to alarm config
    active_alarm_config = alarm;
    alarm_audio_urgent   = urgent;
    alarm_base_volume    = alarm->volume;
    alarm_audio_start_ms = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "Scheduling alarm tone: %d at volume %d%%%s (via LVGL timer)",
             alarm->tone, urgent ? 100 : alarm->volume, urgent ? " URGENT" : "");

    // lv_timer_ready() makes it fire on the very next LVGL cycle.
    audio_alarm_stop_requested = false;
    if (lvgl_port_lock(1)) {
        audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
        lv_timer_ready(audio_alarm_timer);  // Fire immediately
        lvgl_port_unlock();
    } else {
        // Lock failed — pulse timer will create the audio timer on next tick
        ESP_LOGW(TAG, "LVGL lock failed — audio timer creation deferred to pulse timer");
    }
}

// Stop audio alarm
void stop_audio_alarm(void) {
    // Clear config FIRST so timer callback won't start a new tone
    active_alarm_config = NULL;
    alarm_audio_urgent   = false;
    alarm_audio_start_ms = 0;   // resets the escalation ramp

    if (audio_alarm_timer != NULL) {
        ESP_LOGI(TAG, "Stopping audio alarm repeat timer");
        if (lvgl_port_lock(1)) {
            lv_timer_del(audio_alarm_timer);
            audio_alarm_timer = NULL;
            lvgl_port_unlock();
        } else {
            // Defer deletion to the pulse timer; leave the handle set so it can find it.
            audio_alarm_stop_requested = true;
            ESP_LOGW(TAG, "LVGL lock failed — audio timer deletion deferred to pulse timer");
        }
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

        // Progress fill spans the button interior (set when the button was built)
        int progress_w = (int)((elapsed * disarm_progress_max_w) / DISARM_HOLD_MS);
        if (progress_w > disarm_progress_max_w) progress_w = disarm_progress_max_w;
        if (visual_alarm_disarm_progress != NULL) {
            lv_obj_set_width(visual_alarm_disarm_progress, progress_w);
        }

        // Update label text to show countdown
        if (visual_alarm_disarm_label != NULL) {
            if (elapsed < DISARM_HOLD_MS) {
                char buf[32];
                int remaining_tenths = (int)((DISARM_HOLD_MS - elapsed) / 100);
                snprintf(buf, sizeof(buf), "%d.%d s", remaining_tenths / 10, remaining_tenths % 10);
                lv_label_set_text(visual_alarm_disarm_label, buf);
            }
        }

        if (elapsed >= DISARM_HOLD_MS) {
            disarm_press_start_ms = 0;

            // The hold completes with the finger still DOWN; once the takeover is
            // deleted the release would land on whatever sits underneath. Swallow
            // input until the finger lifts.
            lv_indev_wait_release(lv_indev_get_act());

            // Snapshot before teardown — stop_visual_alarm() resets the alert kind
            cygm_alert_kind_t kind = active_alert_kind;
            active_alarm_state_t state_at_dismiss = current_alarm_state;

            stop_visual_alarm();
            stop_audio_alarm();

            // SAFETY: only acknowledge a threshold alarm that is still standing.
            // If it auto-cancelled while the overlay was stuck (failed LVGL lock),
            // acknowledging would suppress the next real alarm.
            if (kind == CYGM_ALERT_GLUCOSE && state_at_dismiss != ALARM_STATE_NONE) {
                alarm_acknowledged = true;
                ESP_LOGI(TAG, "Alarm dismissed (state=%d, glucose=%d)", state_at_dismiss, current_glucose);
                sd_log(TAG, "ALARM: dismissed, glucose=%d state=%d", current_glucose, state_at_dismiss);
            } else {
                ESP_LOGI(TAG, "Alert dismissed (kind=%d, no alarm state to acknowledge)", kind);
                sd_log(TAG, "ALARM: alert dismissed, kind=%d", kind);
            }

            // The takeover can be raised over any sub-screen, so return home;
            // home_screen_active must never claim home when it is not displayed.
            if (screen_home != NULL && lv_scr_act() != screen_home) {
                lv_scr_load(screen_home);
            }
            home_screen_active = (lv_scr_act() == screen_home);
            pause_background_tasks = false;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Released before hold completed — reset
        if (disarm_press_start_ms != 0) {
            disarm_press_start_ms = 0;
            if (visual_alarm_disarm_progress != NULL) {
                lv_obj_set_width(visual_alarm_disarm_progress, 0);
            }
            if (visual_alarm_disarm_label != NULL) {
                lv_label_set_text(visual_alarm_disarm_label, "DISMISS");
            }
            ESP_LOGI(TAG, "Dismiss button released early - reset");
        }
    }
}

// Shared by the SNOOZE tap and the unattended-alarm timeout: arm the snooze
// window (or the per-type suppression), tear the takeover down, return home.
// Runs in LVGL context only (event callback or the pulse timer).
static void alarm_snooze_and_close(void) {
    int snooze_min = alarm_ext_settings.snooze_default_min;
    if (snooze_min < 5 || snooze_min > 120) snooze_min = 30;

    cygm_alert_kind_t kind = active_alert_kind;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (kind == CYGM_ALERT_GLUCOSE) {
        // SAFETY: never set a snooze for an alarm that already cleared — it
        // would suppress the next real one for the whole window.
        if (current_alarm_state != ALARM_STATE_NONE) {
            time_t now;
            time(&now);
            alarm_snooze_until = now + (snooze_min * 60);
            alarm_acknowledged = true;
            ESP_LOGI(TAG, "Alarm snoozed %d min (until %ld)", snooze_min, (long)alarm_snooze_until);
            sd_log(TAG, "ALARM: snoozed %dmin, glucose=%d state=%d",
                   snooze_min, current_glucose, current_alarm_state);
        } else {
            ESP_LOGW(TAG, "Snooze ignored — alarm already cleared");
            sd_log(TAG, "ALARM: snooze rejected, alarm state=NONE");
        }
    } else {
        // Non-threshold alerts have no snooze timer of their own — extend the
        // per-type suppression window instead.
        int64_t until = now_ms + (int64_t)snooze_min * 60000;
        if (kind == CYGM_ALERT_DATA_GAP)          data_gap_suppress_until_ms = until;
        else if (kind == CYGM_ALERT_PREDICT_LOW)  predict_suppress_until_ms  = until;
        else                                      rate_suppress_until_ms     = until;
        ESP_LOGI(TAG, "Alert kind=%d suppressed for %d min", kind, snooze_min);
        sd_log(TAG, "ALARM: alert kind=%d suppressed %dmin", kind, snooze_min);
    }

    stop_visual_alarm();
    stop_audio_alarm();

    // Same return-to-home as DISMISS: the takeover may have been raised over a
    // sub-screen, and the flags must not claim home unless home is displayed.
    if (screen_home != NULL && lv_scr_act() != screen_home) {
        lv_scr_load(screen_home);
    }
    home_screen_active = (lv_scr_act() == screen_home);
    pause_background_tasks = false;
}

// SNOOZE is a single tap: it is the safe action (the alarm re-arms itself), so
// it must not require a hold at 3am. DISMISS keeps the hold-to-confirm above.
static void visual_alarm_snooze_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    alarm_snooze_and_close();
}

// Full-screen takeover: value, reason line, and giant SNOOZE / DISMISS buttons.
// Two objects: `overlay` is the opaque backdrop whose border the pulse timer
// flashes, `container` is the content panel inset so that frame stays visible.
static void start_visual_alarm(void) {
    // visual_alarm_active with a stop pending is NOT "already active": it is an old
    // takeover whose teardown a failed try-lock deferred. Returning here would let
    // the pulse timer destroy the new alert's takeover a moment later, leaving
    // audio with no way to dismiss it. Finish that teardown, then build on top.
    bool finish_deferred_teardown = (visual_alarm_active && visual_alarm_stop_requested);
    if (visual_alarm_active && !finish_deferred_teardown) {
        return;  // Already active
    }

    ESP_LOGI(TAG, "Starting visual alarm (kind=%d)", active_alert_kind);
    visual_alarm_active = true;
    visual_alarm_stop_requested = false;
    visual_alarm_glucose_update_pending = false;
    visual_alarm_pulse_state = 0;
    visual_alarm_shown_state = current_alarm_state;
    visual_alarm_shown_tick = lv_tick_get();  // Starts the unattended-timeout clock

    // Determine alarm info and active config
    const char *alarm_type = "ALARM";
    uint32_t alarm_color = COLOR_RED;
    alarm_config_t *active_cfg = NULL;
    char reason_text[80] = "";
    bool value_is_stale = false;

    if (active_alert_kind != CYGM_ALERT_GLUCOSE) {
        // Data-gap / predictive / rate alert — title, colour and reason were
        // composed by the raising check, which has the numbers in hand.
        alarm_type  = active_alert_title;
        alarm_color = active_alert_color;
        active_cfg  = active_alert_cfg;
        value_is_stale = (active_alert_kind == CYGM_ALERT_DATA_GAP);
        snprintf(reason_text, sizeof(reason_text), "%s", active_alert_reason);
    } else {
        const char *direction = "Below";
        if (current_alarm_state == ALARM_STATE_HIGH_ALARM) {
            alarm_type = "HIGH ALARM";
            active_cfg = &current_alarm_settings.high_alarm;
            direction = "Above";
        } else if (current_alarm_state == ALARM_STATE_HIGH_WARNING) {
            alarm_type = "HIGH WARNING";
            active_cfg = &current_alarm_settings.high_warning;
            direction = "Above";
        } else if (current_alarm_state == ALARM_STATE_LOW_WARNING) {
            alarm_type = "LOW WARNING";
            active_cfg = &current_alarm_settings.low_warning;
        } else if (current_alarm_state == ALARM_STATE_LOW_ALARM) {
            alarm_type = "LOW ALARM";
            active_cfg = &current_alarm_settings.low_alarm;
        }
        if (active_cfg != NULL) {
            char thr[24];
            cygm_format_threshold(active_cfg->threshold, thr, sizeof(thr));
            int age_min = 0;
            if (glucose_timestamp > 0) {
                time_t now;
                time(&now);
                age_min = (int)difftime(now, glucose_timestamp) / 60;
                if (age_min < 0) age_min = 0;
            }
            snprintf(reason_text, sizeof(reason_text), "%s %s - reading %d min old",
                     direction, thr, age_min);
        }
        if (active_cfg) alarm_color = active_cfg->text_color;
    }

    // Screen flash: brightness boost (gated by global + per-alarm toggle)
    if (!active_cfg || active_cfg->visual_enabled) {
        saved_brightness_percent = screen_brightness_percent;
        screen_brightness_percent = 100;
        display_set_brightness(100);
        ESP_LOGI(TAG, "Brightness set to 100%% for visual alarm (saved: %d%%)", saved_brightness_percent);
    }

    // LED flash (gated by global + per-alarm toggle)
    if (!active_cfg || active_cfg->led_enabled) {
        led_start_alarm_flash();
    }
    visual_alarm_flash_color = alarm_color;

    // Life-safety: retry lock with ~500ms patience — alarm MUST display
    bool alarm_locked = false;
    for (int retry = 0; retry < 50 && !alarm_locked; retry++) {
        alarm_locked = lvgl_port_lock(1);
        if (!alarm_locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!alarm_locked) {
        // Failed after 500ms — reset state so alarm re-triggers on next glucose check
        ESP_LOGE(TAG, "CRITICAL: LVGL lock failed for visual alarm — will retry next cycle");
        sd_log(TAG, "ALARM: LVGL lock failed, resetting for retry");
        visual_alarm_active = false;
        current_alarm_state = ALARM_STATE_NONE;  // Reset so check_glucose_alarms re-triggers
        active_alert_kind = CYGM_ALERT_GLUCOSE;
        active_alert_cfg = NULL;
        led_stop_alarm_flash();
        screen_brightness_percent = saved_brightness_percent;
        display_set_brightness(saved_brightness_percent);
        if (finish_deferred_teardown) {
            // The previous takeover is still on screen with its teardown owed to
            // the pulse timer. Hand it back rather than stranding it there.
            visual_alarm_active = true;
            visual_alarm_stop_requested = true;
        }
        return;
    }

    if (finish_deferred_teardown) {
        // Complete the teardown the pulse timer owed, so nothing is left behind
        // to destroy the objects built below.
        ESP_LOGI(TAG, "Completing deferred alarm teardown before new takeover");
        visual_alarm_glucose_update_pending = false;
        if (audio_alarm_stop_requested && audio_alarm_timer != NULL) {
            lv_timer_del(audio_alarm_timer);
            audio_alarm_timer = NULL;
            audio_alarm_stop_requested = false;
        }
        if (active_alarm_config != NULL && audio_alarm_timer == NULL && !audio_alarm_stop_requested) {
            audio_alarm_timer = lv_timer_create(audio_alarm_repeat_timer_cb, 3000, NULL);
            lv_timer_ready(audio_alarm_timer);
            ESP_LOGI(TAG, "Created deferred audio timer during teardown");
        }
        visual_alarm_cleanup_lvgl();
    }

    // Takeover geometry: the opaque backdrop owns the pulsing frame, so the
    // content panel is inset far enough on every side to leave it visible.
    const lv_coord_t TAKEOVER_INSET = 10;
    const lv_coord_t panel_w = LV_HOR_RES - (TAKEOVER_INSET * 2);
    const lv_coord_t panel_h = LV_VER_RES - (TAKEOVER_INSET * 2);
    const lv_coord_t btn_pad = 8;
    const lv_coord_t btn_h   = 68;   // giant target: well past CYGM_HIT_TARGET_MIN
    const lv_coord_t btn_w   = (panel_w - (btn_pad * 3)) / 2;

    // ---- Opaque backdrop with alarm-color border frame at screen edges ----
    visual_alarm_overlay = lv_obj_create(lv_scr_act());
        lv_obj_add_event_cb(visual_alarm_overlay, visual_alarm_delete_cb,
                            LV_EVENT_DELETE, NULL);
        lv_obj_set_size(visual_alarm_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(visual_alarm_overlay, 0, 0);
        lv_obj_set_style_bg_color(visual_alarm_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(visual_alarm_overlay, LV_OPA_COVER, 0);  // takeover: hide the screen beneath
        lv_obj_set_style_border_width(visual_alarm_overlay, 0, 0);  // Starts off, pulse timer toggles
        lv_obj_set_style_border_side(visual_alarm_overlay, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(visual_alarm_overlay, 12, 0);  // Rounded inner corners on border
        lv_obj_set_style_pad_all(visual_alarm_overlay, 0, 0);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(visual_alarm_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(visual_alarm_overlay);

        // ---- Content panel ----
        visual_alarm_container = lv_obj_create(lv_scr_act());
        lv_obj_add_event_cb(visual_alarm_container, visual_alarm_delete_cb,
                            LV_EVENT_DELETE, NULL);
        lv_obj_set_size(visual_alarm_container, panel_w, panel_h);
        lv_obj_center(visual_alarm_container);
        lv_obj_set_style_bg_color(visual_alarm_container, lv_color_hex(COLOR_MODAL_BG), 0);
        lv_obj_set_style_bg_opa(visual_alarm_container, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(visual_alarm_container, 12, 0);
        lv_obj_set_style_border_width(visual_alarm_container, 1, 0);
        lv_obj_set_style_border_color(visual_alarm_container, lv_color_hex(COLOR_MODAL_BORDER), 0);
        lv_obj_set_style_border_opa(visual_alarm_container, LV_OPA_50, 0);
        lv_obj_set_style_shadow_width(visual_alarm_container, 0, 0);  // NEVER use shadows — causes device freeze
        lv_obj_set_style_pad_all(visual_alarm_container, 0, 0);
        lv_obj_clear_flag(visual_alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(visual_alarm_container);

        // ---- Title row: bell + alert type ----
        lv_obj_t *bell = lv_label_create(visual_alarm_container);
        lv_label_set_text(bell, LV_SYMBOL_BELL);
        lv_obj_set_style_text_font(bell, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bell, lv_color_hex(alarm_color), 0);
        lv_obj_align(bell, LV_ALIGN_TOP_LEFT, 10, 6);

        lv_obj_t *type_label = lv_label_create(visual_alarm_container);
        lv_label_set_text(type_label, alarm_type);
        lv_obj_set_style_text_font(type_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(type_label, lv_color_hex(alarm_color), 0);
        lv_obj_align(type_label, LV_ALIGN_TOP_LEFT, 38, 9);

        // ---- Divider ----
        lv_obj_t *div = lv_obj_create(visual_alarm_container);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, panel_w - 24, 1);
        lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 32);
        lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

        // ---- Reason line: WHY this alert fired, in plain language ----
        lv_obj_t *reason_label = lv_label_create(visual_alarm_container);
        lv_label_set_long_mode(reason_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(reason_label, panel_w - 24);
        lv_obj_set_style_text_align(reason_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(reason_label, reason_text);
        lv_obj_set_style_text_font(reason_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(reason_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_align(reason_label, LV_ALIGN_TOP_MID, 0, 37);

        // ---- Value (huge, locale-formatted) ----
        visual_alarm_glucose_label = lv_label_create(visual_alarm_container);
        char glucose_text[16];
        if (glucose_data_valid) {
            cygm_format_glucose(current_glucose, glucose_text, sizeof(glucose_text));
        } else {
            snprintf(glucose_text, sizeof(glucose_text), "---");
        }
        lv_label_set_text(visual_alarm_glucose_label, glucose_text);
        lv_obj_set_style_text_font(visual_alarm_glucose_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(visual_alarm_glucose_label,
                                    lv_color_hex(value_is_stale ? COLOR_TEXT_DIM : alarm_color), 0);
        // y=72 clears a 2-line reason at montserrat_12 with room to spare.
        lv_obj_align(visual_alarm_glucose_label, LV_ALIGN_TOP_MID, -16, 72);

        // ---- Unit label, on the value's baseline ----
        lv_obj_t *unit_label = lv_label_create(visual_alarm_container);
        lv_label_set_text(unit_label, cygm_glucose_unit());
        lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(unit_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align_to(unit_label, visual_alarm_glucose_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -10);

        // ---- SNOOZE: single tap, the safe action ----
        visual_alarm_snooze_btn = lv_obj_create(visual_alarm_container);
        lv_obj_remove_style_all(visual_alarm_snooze_btn);
        lv_obj_set_size(visual_alarm_snooze_btn, btn_w, btn_h);
        lv_obj_align(visual_alarm_snooze_btn, LV_ALIGN_BOTTOM_LEFT, btn_pad, -btn_pad);
        lv_obj_set_style_bg_color(visual_alarm_snooze_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(visual_alarm_snooze_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(visual_alarm_snooze_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_pad_all(visual_alarm_snooze_btn, 0, 0);
        lv_obj_clear_flag(visual_alarm_snooze_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(visual_alarm_snooze_btn, LV_OBJ_FLAG_CLICKABLE);
        cygm_apply_ghost_btn(visual_alarm_snooze_btn);

        lv_obj_t *snooze_label = lv_label_create(visual_alarm_snooze_btn);
        lv_label_set_text(snooze_label, "SNOOZE");
        lv_obj_set_style_text_font(snooze_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(snooze_label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
        lv_obj_align(snooze_label, LV_ALIGN_CENTER, 0, -8);

        {
            int snooze_min = alarm_ext_settings.snooze_default_min;
            if (snooze_min < 5 || snooze_min > 120) snooze_min = 30;
            char snooze_hint[20];
            snprintf(snooze_hint, sizeof(snooze_hint), "%d min", snooze_min);
            lv_obj_t *snooze_sub = lv_label_create(visual_alarm_snooze_btn);
            lv_label_set_text(snooze_sub, snooze_hint);
            lv_obj_set_style_text_font(snooze_sub, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(snooze_sub, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_align(snooze_sub, LV_ALIGN_CENTER, 0, 12);
        }

        lv_obj_add_event_cb(visual_alarm_snooze_btn, visual_alarm_snooze_event_cb, LV_EVENT_CLICKED, NULL);

        // ---- DISMISS: hold to confirm, so one stray tap cannot silence an alarm ----
        visual_alarm_disarm_btn = lv_obj_create(visual_alarm_container);
        lv_obj_remove_style_all(visual_alarm_disarm_btn);
        lv_obj_set_size(visual_alarm_disarm_btn, btn_w, btn_h);
        lv_obj_align(visual_alarm_disarm_btn, LV_ALIGN_BOTTOM_RIGHT, -btn_pad, -btn_pad);
        lv_obj_set_style_bg_color(visual_alarm_disarm_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(visual_alarm_disarm_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(visual_alarm_disarm_btn, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_pad_all(visual_alarm_disarm_btn, 0, 0);
        lv_obj_clear_flag(visual_alarm_disarm_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(visual_alarm_disarm_btn, LV_OBJ_FLAG_CLICKABLE);
        // Shared pressed feedback + enlarged touch box; the local styles above still win.
        cygm_apply_ghost_btn(visual_alarm_disarm_btn);

        // Progress fill bar (grows from left as user holds)
        disarm_progress_max_w = btn_w - 8;
        visual_alarm_disarm_progress = lv_obj_create(visual_alarm_disarm_btn);
        lv_obj_remove_style_all(visual_alarm_disarm_progress);
        lv_obj_set_size(visual_alarm_disarm_progress, 0, btn_h - 8);  // Starts at 0 width
        lv_obj_align(visual_alarm_disarm_progress, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_color(visual_alarm_disarm_progress, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(visual_alarm_disarm_progress, LV_OPA_30, 0);
        lv_obj_set_style_radius(visual_alarm_disarm_progress, 8, 0);
        lv_obj_clear_flag(visual_alarm_disarm_progress, LV_OBJ_FLAG_CLICKABLE);

        // Button label
        visual_alarm_disarm_label = lv_label_create(visual_alarm_disarm_btn);
        lv_label_set_text(visual_alarm_disarm_label, "DISMISS");
        lv_obj_set_style_text_font(visual_alarm_disarm_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(visual_alarm_disarm_label, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(visual_alarm_disarm_label, LV_ALIGN_CENTER, 0, -8);

        lv_obj_t *disarm_sub = lv_label_create(visual_alarm_disarm_btn);
        lv_label_set_text(disarm_sub, "hold 1.5s");
        lv_obj_set_style_text_font(disarm_sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(disarm_sub, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(disarm_sub, LV_ALIGN_CENTER, 0, 12);

        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(visual_alarm_disarm_btn, visual_alarm_disarm_event_cb, LV_EVENT_PRESS_LOST, NULL);

        // Pulse timer — fast for urgent alarms (200ms), moderate for warnings (350ms)
        // Must be created inside LVGL lock (LVGL API requirement)
        if (visual_alarm_timer != NULL) {
            lv_timer_del(visual_alarm_timer);  // Clean up stale timer from previous alarm
            visual_alarm_timer = NULL;
        }
        uint32_t pulse_ms = (current_alarm_state == ALARM_STATE_HIGH_ALARM ||
                             current_alarm_state == ALARM_STATE_LOW_ALARM) ? 200 : 350;
        visual_alarm_timer = lv_timer_create(visual_alarm_pulse_timer_cb, pulse_ms, NULL);

        lvgl_port_unlock();
}  // end start_visual_alarm

// Delete all visual alarm LVGL objects. MUST be called with the LVGL lock held.
// Statics are NULLed BEFORE the deletes so visual_alarm_delete_cb can tell an
// internal teardown from an external kill (e.g. the watchdog deleting the screen).
static void visual_alarm_cleanup_lvgl(void) {
    if (visual_alarm_timer != NULL) {
        lv_timer_del(visual_alarm_timer);
        visual_alarm_timer = NULL;
    }
    lv_obj_t *ov = visual_alarm_overlay;
    lv_obj_t *ct = visual_alarm_container;
    visual_alarm_overlay = NULL;
    visual_alarm_container = NULL;
    visual_alarm_disarm_btn = NULL;
    visual_alarm_disarm_progress = NULL;
    visual_alarm_disarm_label = NULL;
    visual_alarm_snooze_btn = NULL;
    visual_alarm_glucose_label = NULL;
    disarm_press_start_ms = 0;
    if (ov != NULL) lv_obj_del(ov);
    if (ct != NULL) lv_obj_del(ct);
}

// LV_EVENT_DELETE watchdog for the takeover pair. Fires on every deletion path but
// only acts when a static is still set, i.e. something other than
// visual_alarm_cleanup_lvgl() destroyed the object. Without it the pulse timer
// keeps styling freed memory. Hardware stops stay with stop_visual_alarm() —
// this is corruption containment, not an alarm dismissal.
static void visual_alarm_delete_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    bool external = (target == visual_alarm_overlay) || (target == visual_alarm_container);
    if (!external) {
        return;  // Internal teardown already NULLed the statics
    }
    if (visual_alarm_timer != NULL) {
        lv_timer_del(visual_alarm_timer);
        visual_alarm_timer = NULL;
    }
    lv_obj_t *ov = visual_alarm_overlay;
    lv_obj_t *ct = visual_alarm_container;
    visual_alarm_overlay = NULL;
    visual_alarm_container = NULL;
    visual_alarm_disarm_btn = NULL;
    visual_alarm_disarm_progress = NULL;
    visual_alarm_disarm_label = NULL;
    visual_alarm_snooze_btn = NULL;
    visual_alarm_glucose_label = NULL;
    visual_alarm_glucose_update_pending = false;
    disarm_press_start_ms = 0;
    // The pair are siblings: deleting one externally strands the other.
    // Delete the survivor too (its own cb then no-ops on NULLed statics).
    if (target == ov && ct != NULL) lv_obj_del(ct);
    if (target == ct && ov != NULL) lv_obj_del(ov);
    // Unlatch the state so check_glucose_alarms re-triggers a standing alarm and
    // rebuilds the takeover on its next check.
    visual_alarm_active = false;
    visual_alarm_stop_requested = false;
    current_alarm_state = ALARM_STATE_NONE;
}

// Stop visual alarm — hardware cleanup is immediate, LVGL cleanup deferred if lock fails
void stop_visual_alarm(void) {
    // Non-LVGL cleanup: brightness + LED (idempotent, safe from any core)
    if (visual_alarm_active && !visual_alarm_stop_requested) {
        ESP_LOGI(TAG, "Stopping visual alarm");

        // Restore saved brightness
        screen_brightness_percent = saved_brightness_percent;
        display_set_brightness(saved_brightness_percent);
        ESP_LOGI(TAG, "Brightness restored to %d%%", saved_brightness_percent);

        // Stop LED alarm flash
        led_stop_alarm_flash();
    }

    // Every teardown path returns the engine to plain glucose alerting; a
    // lingering non-glucose kind would mis-label or mis-gate the next alarm.
    active_alert_kind = CYGM_ALERT_GLUCOSE;
    active_alert_cfg = NULL;

    // Try immediate LVGL cleanup
    if (lvgl_port_lock(1)) {
        visual_alarm_active = false;
        visual_alarm_stop_requested = false;
        visual_alarm_glucose_update_pending = false;
        visual_alarm_cleanup_lvgl();
        lvgl_port_unlock();
    } else if (visual_alarm_active) {
        // Lock failed (Core 1 busy). Keep visual_alarm_active=true so the pulse
        // timer keeps running and does the cleanup on its next tick.
        visual_alarm_stop_requested = true;
        ESP_LOGW(TAG, "LVGL lock failed — deferring visual alarm cleanup to pulse timer");
    }
    // If !visual_alarm_active && lock failed: objects may already be cleaned up
    // or will be cleaned up by a pending deferred stop — nothing more to do.
}

// Update glucose value shown on the visual alarm card (defers to pulse timer)
static void update_visual_alarm_glucose(int glucose_mg_dl) {
    if (!visual_alarm_active || visual_alarm_glucose_label == NULL) return;
    if (active_alert_kind != CYGM_ALERT_GLUCOSE) return;  // other kinds own their own value text
    // Defer to pulse timer (runs every 200-350ms in LVGL context)
    visual_alarm_pending_glucose = glucose_mg_dl;
    visual_alarm_glucose_update_pending = true;
}

// ==================== Non-Threshold Alerts ====================

// Raise a data-gap / predictive / rate alert. Never urgent, so quiet hours
// downgrade them to visual + LED only, and they never touch current_alarm_state.
// Returns true only once the takeover is really on screen — callers stamp their
// suppression window off that, so a lock failure costs one cycle, not the window.
static bool raise_alert(cygm_alert_kind_t kind, alarm_config_t *cfg,
                        const char *title, const char *reason, uint32_t color) {
    if (visual_alarm_active || current_alarm_state != ALARM_STATE_NONE) {
        return false;  // never stack on top of a live alarm
    }

    active_alert_kind  = kind;
    active_alert_cfg   = cfg;
    active_alert_color = color;
    snprintf(active_alert_title,  sizeof(active_alert_title),  "%s", title);
    snprintf(active_alert_reason, sizeof(active_alert_reason), "%s", reason);

    // Visual first: if the takeover cannot be shown, bail before starting a
    // tone that would have no on-screen explanation and no way to dismiss it.
    start_visual_alarm();
    if (!visual_alarm_active) {
        active_alert_kind = CYGM_ALERT_GLUCOSE;
        active_alert_cfg = NULL;
        return false;
    }

    if (cfg != NULL && cfg->audio_enabled && !cygm_quiet_hours_active()) {
        start_audio_alarm(cfg, false);
    }
    return true;
}

// Data-gap watchdog. Warns BEFORE the 30-minute stale cutoff and never touches
// glucose_data_valid, so the existing safety net is unchanged.
void check_data_gap_alert(void) {
    if (!alarm_ext_settings.gap_enabled) return;
    if (sensor_change_mode) return;        // user already told us the sensor is out
    if (!first_glucose_received || glucose_timestamp <= 0) return;
    if (!is_time_synced()) return;         // age is meaningless without a real clock

    int gap_min = alarm_ext_settings.gap_minutes;
    if (gap_min < 10 || gap_min > 60) gap_min = 20;

    time_t now;
    time(&now);
    int age_min = (int)difftime(now, glucose_timestamp) / 60;

    // Gap closed — clear a standing gap alert and re-arm for the next one
    if (age_min < gap_min) {
        if (data_gap_alert_active) {
            data_gap_alert_active = false;
            data_gap_suppress_until_ms = 0;
            if (active_alert_kind == CYGM_ALERT_DATA_GAP) {
                ESP_LOGI(TAG, "Data gap closed — clearing gap alert");
                stop_visual_alarm();
                stop_audio_alarm();
            }
        }
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < data_gap_suppress_until_ms) return;
    if (visual_alarm_active || current_alarm_state != ALARM_STATE_NONE) return;

    int suppress_min = alarm_ext_settings.suppress_min;
    if (suppress_min < 5 || suppress_min > 120) suppress_min = 30;

    // Distinct tone and its own volume — this is not a threshold alarm
    uint8_t tone = alarm_ext_settings.gap_tone;
    if (tone >= (uint8_t)ALARM_TONE_COUNT) tone = (uint8_t)ALARM_TONE_ASCENDING;
    uint8_t vol = alarm_ext_settings.gap_volume;
    if (vol > 100) vol = 70;

    memset(&data_gap_alarm_cfg, 0, sizeof(data_gap_alarm_cfg));
    data_gap_alarm_cfg.enabled        = true;
    data_gap_alarm_cfg.text_color     = COLOR_ORANGE;
    data_gap_alarm_cfg.audio_enabled  = true;
    data_gap_alarm_cfg.visual_enabled = true;
    data_gap_alarm_cfg.led_enabled    = true;
    data_gap_alarm_cfg.tone           = (alarm_tone_t)tone;
    data_gap_alarm_cfg.volume         = vol;
    data_gap_alarm_cfg.audio_repeat   = false;  // one shot per suppression window

    char reason[72];
    snprintf(reason, sizeof(reason), "No readings for %d min - %s", age_min, data_gap_cause());

    ESP_LOGW(TAG, "DATA GAP: %s", reason);
    sd_log(TAG, "ALARM: data gap %d min, cause=%s", age_min, data_gap_cause());

    if (raise_alert(CYGM_ALERT_DATA_GAP, &data_gap_alarm_cfg, "NO DATA", reason, COLOR_ORANGE)) {
        data_gap_suppress_until_ms = now_ms + (int64_t)suppress_min * 60000;
        data_gap_alert_active = true;
    }
}

// Predictive urgent-low and sustained rate-of-change alerts.
void check_predictive_alerts(void) {
    if (!alarm_ext_settings.predict_enabled && !alarm_ext_settings.rate_enabled) return;
    if (!glucose_data_valid || !glucose_data_fresh) return;
    if (visual_alarm_active || current_alarm_state != ALARM_STATE_NONE) return;

    // A live snooze damps predictive noise too. The urgent-low guard still
    // breaks through on the real threshold, so nothing dangerous is masked.
    time_t now;
    time(&now);
    if (alarm_snooze_until > 0 && now < alarm_snooze_until) return;

    int slope_tenths = 0, newest = 0;
    if (!glucose_slope_tenths(&slope_tenths, &newest)) return;

    int64_t now_ms = esp_timer_get_time() / 1000;
    int suppress_min = alarm_ext_settings.suppress_min;
    if (suppress_min < 5 || suppress_min > 120) suppress_min = 30;
    int64_t suppress_ms = (int64_t)suppress_min * 60000;

    // ---- Projected to cross urgent low ----
    if (alarm_ext_settings.predict_enabled && slope_tenths < 0 &&
        now_ms >= predict_suppress_until_ms) {
        int horizon = alarm_ext_settings.predict_horizon_min;
        if (horizon < 5 || horizon > 45) horizon = 20;

        int urgent_thr = cygm_urgent_low_threshold();
        int projected = newest + (slope_tenths * horizon) / 10;

        if (newest > urgent_thr && projected <= urgent_thr) {
            char thr_str[24], reason[72];
            cygm_format_threshold(urgent_thr, thr_str, sizeof(thr_str));
            snprintf(reason, sizeof(reason), "Falling fast - below %s within %d min",
                     thr_str, horizon);

            ESP_LOGW(TAG, "PREDICT LOW: newest=%d slope_x10=%d projected=%d thr=%d",
                     newest, slope_tenths, projected, urgent_thr);
            sd_log(TAG, "ALARM: predictive low, newest=%d slope_x10=%d proj=%d",
                   newest, slope_tenths, projected);

            if (raise_alert(CYGM_ALERT_PREDICT_LOW, &current_alarm_settings.low_warning,
                            "PREDICTED LOW", reason,
                            current_alarm_settings.low_warning.text_color)) {
                predict_suppress_until_ms = now_ms + suppress_ms;
            }
            return;
        }
    }

    // ---- Sustained rate of change ----
    if (alarm_ext_settings.rate_enabled && now_ms >= rate_suppress_until_ms) {
        int thr_x10 = alarm_ext_settings.rate_threshold_x10;
        if (thr_x10 < 10 || thr_x10 > 60) thr_x10 = 30;
        int magnitude = (slope_tenths < 0) ? -slope_tenths : slope_tenths;

        if (magnitude >= thr_x10) {
            bool rising = (slope_tenths > 0);
            alarm_config_t *cfg = rising ? &current_alarm_settings.high_warning
                                         : &current_alarm_settings.low_warning;
            ESP_LOGW(TAG, "RATE ALERT: %s, slope_x10=%d newest=%d",
                     rising ? "rising" : "falling", slope_tenths, newest);
            sd_log(TAG, "ALARM: rate alert slope_x10=%d newest=%d", slope_tenths, newest);

            if (raise_alert(CYGM_ALERT_RATE, cfg,
                            rising ? "RISING FAST" : "FALLING FAST",
                            rising ? "Glucose is rising quickly"
                                   : "Glucose is falling quickly",
                            cfg->text_color)) {
                rate_suppress_until_ms = now_ms + suppress_ms;
            }
        }
    }
}

// Update glucose display on home screen
void update_glucose_display(void) {
    // Check if widgets exist (home screen is loaded)
    if (label_glucose == NULL || trend_canvas == NULL || label_time_ago == NULL) {
        return;
    }

    if (lvgl_port_lock_retry(15)) {  // Life-safety: 15 attempts (~15ms patience)
        if (glucose_data_valid) {
            // Calculate data age first - this determines display behavior
            time_t now;
            time(&now);
            int minutes_ago = (int)difftime(now, glucose_timestamp) / 60;

            // SAFETY CRITICAL: data older than 15 min must not show a number — a
            // stale reading (181 when the truth is 52) can be life-threatening.
            if (minutes_ago > 15) {
                lv_label_set_text(label_glucose, "---");
                lv_canvas_fill_bg(trend_canvas, trend_fill_bg(), LV_OPA_0);  // Clear arrow
                trend_anim_reset();   // nothing may redraw over the cleared canvas
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
                cygm_format_glucose(current_glucose, glucose_str, sizeof(glucose_str));
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
                lv_canvas_fill_bg(trend_canvas, trend_fill_bg(), LV_OPA_0);
                if (cgm_expanded) {
                    draw_trend_arrow_sized(trend_canvas, current_trend, 80);
                } else {
                    draw_trend_arrow(trend_canvas, current_trend);
                }

                update_ambient_tint();

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
            lv_canvas_fill_bg(trend_canvas, trend_fill_bg(), LV_OPA_0);  // Clear arrow
            trend_anim_reset();   // nothing may redraw over the cleared canvas

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
    } else {
        ESP_LOGW(TAG, "LVGL lock timeout in update_glucose_display");
    }
}

// Delete background tasks to free their heap-allocated stacks for TLS: the idle
// task's reclaim yields the contiguous heap a handshake needs, and stopped tasks
// stop competing for the network mutex. Returns true if anything was deleted.
bool delete_background_tasks_for_ssl(const char *reason) {
    bool deleted_any = false;

    // Only weather_task (~3KB) goes. time_task and battery_task stay so the clock
    // and battery monitoring keep running even if the handshake hangs, and the
    // largest free block is already well past the ~17KB TLS needs.
    if (weather_task_handle != NULL) {
        vTaskDelete(weather_task_handle);
        weather_task_handle = NULL;
        deleted_any = true;
    }

    if (deleted_any) {
        // Yield long enough for the idle task to reclaim the stack on both cores.
        vTaskDelay(pdMS_TO_TICKS(100));

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "Tasks stopped: %s (free=%lu largest=%lu)", reason, free_heap, largest);
        sd_log(TAG, "Tasks stopped: %s free=%lu largest=%lu", reason, free_heap, largest);
    }

    return deleted_any;
}

// Recreate background tasks after a TLS operation. Routed through
// ensure_tasks_running() so core pinning, priority and stack size live in one
// place; idempotent, a running task is left alone.
void recreate_background_tasks(void) {
    ensure_tasks_running();
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
        // Arm the home-screen countdown arc for this wait
        glucose_next_fetch_ms = esp_timer_get_time() / 1000 + wait_time_ms;
        glucose_fetch_period_s = (wait_seconds > 0) ? wait_seconds : 1;
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
        glucose_next_fetch_ms = esp_timer_get_time() / 1000 + (int64_t)poll_interval_sec * 1000;
        glucose_fetch_period_s = poll_interval_sec;
        for (int i = 0; i < poll_interval_sec; i++) {
            if (glucose_force_fetch_requested) {
                ESP_LOGI(TAG, "Force-fetch requested, breaking wait loop early");
                glucose_force_fetch_requested = false;
                glucose_next_fetch_ms = esp_timer_get_time() / 1000;  // arc: pull is now
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));  // Check every 1 second
        }

        // Data-gap watchdog runs BEFORE the WiFi gate so a dead link is exactly
        // the case it can still report. Cheap and self-suppressing.
        check_data_gap_alert();

        // SAFETY: glucose fetching always runs while WiFi is up — freshness is
        // life-critical, so it never pauses for navigation. Only UI updates are
        // gated on home_screen_active.
        if (!wifi_connected) {
            ESP_LOGI(TAG, "[Glucose Task] WiFi not connected - waiting 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        // Auto-re-auth from stored credentials after a lost session. CRITICAL:
        // delete background tasks first — the TLS handshake needs ~16KB contiguous
        // heap, and running tasks fragment it down to a ~3KB largest block.
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

        // Refresh the session BEFORE entering the fetch path, so the inline
        // refresh inside the fetch never runs under low-memory conditions.
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

        if (is_auth) {
            // The network mutex serialises all SSL operations.
            ESP_LOGI(TAG, "Waiting for network mutex...");
            if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                ESP_LOGI(TAG, "Network mutex acquired for glucose fetch");

                // MALLOC_CAP_DEFAULT (DRAM only), never MALLOC_CAP_INTERNAL —
                // INTERNAL counts ~27KB of IRAM that SSL cannot use.
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
                    // A fresh TLS handshake needs ~10KB contiguous heap, and the
                    // boot handshake fragments memory permanently. Deleting task
                    // stacks (~11KB) is what makes a block that large available.
                    tasks_deleted_for_ssl = delete_background_tasks_for_ssl("ssl-reconnect");
                }

                ESP_LOGI(TAG, "Proceeding with %s (free=%lu, largest=%lu)",
                         client_open ? "existing SSL" : "fresh SSL",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

                // Mark fetch as in progress
                glucose_fetch_in_progress = true;
                glucose_fetch_active = true;
                cgm_glucose_t glucose = {0};  // Initialize to prevent garbage data
                esp_err_t ret = ESP_FAIL;

                // Re-check WiFi — could have dropped during task deletion / mutex wait.
                // Starting an SSL handshake with dead WiFi blocks for minutes.
                if (!wifi_connected) {
                    ESP_LOGW(TAG, "WiFi lost before fetch — aborting");
                    glucose_fetch_in_progress = false;
                    glucose_fetch_active = false;
                    goto fetch_cleanup;
                }

                ESP_LOGI(TAG, "Fetching periodic glucose reading...");

                // Blink blue to indicate CGM poll starting
                led_blink_blue();

                ret = cgm_fetch(provider, &glucose);

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
                    glucose_fetch_active = false;
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
                ESP_LOGI(TAG, "History done, updating UI (heap=%lu)", esp_get_free_heap_size());

                // Mark fetch as successful
                glucose_fetch_failed = false;
                glucose_fetch_in_progress = false;
                glucose_fetch_active = false;

                // Reset backoff on success
                if (consecutive_task_failures > 0) {
                    ESP_LOGI(TAG, "Fetch succeeded after %d failures — resetting poll to 90s", consecutive_task_failures);
                }
                consecutive_task_failures = 0;
                poll_interval_sec = 90;

                // UI updates: only when on home screen (LVGL objects may not exist otherwise)
                if (home_screen_active) {
                    // 15 attempts: fewer loses the race against alarm-overlay render load.
                    if (lvgl_port_lock_retry(15)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        if (glucose_freshness_arc != NULL) {
                            lv_arc_set_value(glucose_freshness_arc, 100);  // Fresh reading
                        }
                        lvgl_port_unlock();
                    } else {
                        ESP_LOGW(TAG, "LVGL lock timeout in glucose success UI");
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

                // Play success sound on first glucose reading (only on fresh power-on)
                if (!first_glucose_received) {
                    first_glucose_received = true;
                    if (esp_reset_reason() == ESP_RST_POWERON) {
                        ESP_LOGI(TAG, "First glucose reading received - playing success sound");
                        play_success_sound();
                    }
                }

                // Alarms ALWAYS run regardless of active screen (life-safety)
                if (glucose_data_fresh) {
                    time_t alarm_now;
                    time(&alarm_now);
                    int data_age_mins = (int)difftime(alarm_now, glucose_timestamp) / 60;
                    if (data_age_mins <= 10) {
                        check_glucose_alarms(current_glucose);
                        update_visual_alarm_glucose(current_glucose);
                        // Slope-based alerts run on the freshly stored reading
                        check_predictive_alerts();
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
                glucose_fetch_active = false;
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
                    if (lvgl_port_lock(1)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        // Duplicate/stale: arc stays at current value (reading age unchanged)
                        lvgl_port_unlock();
                    }

                    update_glucose_display();

                    // Show no-data overlay if signal lost
                    if (!sensor_change_mode && wifi_connected &&
                        (glucose.status == GLUCOSE_STATUS_NO_DATA ||
                         glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS)) {
                        if (lvgl_port_lock_retry(50)) {
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
                glucose_fetch_active = false;

                // UI updates: only when on home screen
                if (home_screen_active) {
                    // Stop animation and update bar to red (but keep blue glow)
                    if (lvgl_port_lock(1)) {
                        if (glucose_fetch_animation_timer != NULL) {
                            lv_timer_del(glucose_fetch_animation_timer);
                            glucose_fetch_animation_timer = NULL;
                        }
                        // Error: arc color will be updated by timer callback
                        lvgl_port_unlock();
                    }

                    update_glucose_display();

                    // Show no-data overlay if signal lost
                    if (!sensor_change_mode && wifi_connected &&
                        (glucose.status == GLUCOSE_STATUS_NO_DATA ||
                         glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS)) {
                        if (lvgl_port_lock_retry(50)) {
                            show_nodata_overlay();
                            lvgl_port_unlock();
                        }
                    }
                }

                ESP_LOGW(TAG, "Periodic glucose fetch failed: %s", esp_err_to_name(ret));

                // Error LED blink is deferred until after network_mutex release (below)

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

            fetch_cleanup:
                glucose_fetch_active = false;  // Safety net: ensure arc doesn't stay blue
                // Always close SSL after a fetch for TLS providers: post-reconnect
                // fragmentation leaves the largest block under the ~1.7KB a TLS read
                // needs, so a kept-alive client fails next cycle. HTTP-mode stays open.
                bool always_close = cgm_needs_tls(provider);

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

                // Deferred error LED blink — runs AFTER mutex release to avoid
                // blocking weather/time tasks for 5 seconds during the delay.
                if (glucose_fetch_failed) {
                    led_start_error_blink();
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    led_stop_error_blink();
                }
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
                        if (lvgl_port_lock_retry(50)) {
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

            if (largest < 2048) {
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
lv_style_t style_btn_ghost;
lv_style_t style_btn_pressed;

void init_styles(void) {
    // Background style - pure black
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(COLOR_BG));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    // Card style - clean dark blue-grey with zone-colored border
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(COLOR_CARD_BG));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_border_width(&style_card, 2);
    lv_style_set_border_opa(&style_card, LV_OPA_60);
    lv_style_set_pad_all(&style_card, 10);

    // Ghost button base — the canonical look every UI module shares
    lv_style_init(&style_btn_ghost);
    lv_style_set_bg_opa(&style_btn_ghost, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_btn_ghost, CYGM_BTN_BORDER_W);
    lv_style_set_border_color(&style_btn_ghost, lv_color_hex(COLOR_ACCENT_BLUE));
    lv_style_set_border_opa(&style_btn_ghost, LV_OPA_60);
    lv_style_set_radius(&style_btn_ghost, CYGM_BTN_RADIUS);
    lv_style_set_shadow_width(&style_btn_ghost, 0);

    // Pressed feedback — colour shift only. No LV_STYLE_TRANSITION is set, so this
    // never schedules a style animation, and no border colour, so accents survive.
    lv_style_init(&style_btn_pressed);
    lv_style_set_bg_color(&style_btn_pressed, lv_color_hex(COLOR_PRESSED));
    lv_style_set_bg_opa(&style_btn_pressed, LV_OPA_COVER);
    lv_style_set_border_opa(&style_btn_pressed, LV_OPA_COVER);
}

// Apply the canonical ghost look and grow the touch box toward the minimum
// comfortable target. The DRAWN size is never changed — only ext_click_area,
// so layouts stay pixel-identical while fingers get a bigger target.
void cygm_apply_ghost_btn(lv_obj_t *btn) {
    if (btn == NULL) return;

    lv_obj_add_style(btn, &style_btn_ghost, 0);
    lv_obj_add_style(btn, &style_btn_pressed, LV_STATE_PRESSED);

    lv_coord_t ext = CYGM_BTN_EXT_CLICK;
    lv_coord_t w = lv_obj_get_width(btn);
    lv_coord_t h = lv_obj_get_height(btn);
    if (w > 0 && w < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - w + 1) / 2;
        if (need > ext) ext = need;
    }
    if (h > 0 && h < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - h + 1) / 2;
        if (need > ext) ext = need;
    }
    lv_obj_set_ext_click_area(btn, ext);
}

// ===== EVENT HANDLERS =====

// Forward declarations for functions remaining in main.c
void temp_toggle_event_cb(lv_event_t *e);

// Tapping the temperature label toggles Celsius/Fahrenheit.
void temp_toggle_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        user_temp_celsius = !user_temp_celsius;
        ESP_LOGI(TAG, "Temperature unit toggled to %s", user_temp_celsius ? "Celsius" : "Fahrenheit");

        weather_settings_t settings;
        if (nvs_load_weather_settings(&settings) == ESP_OK) {
            nvs_save_weather_settings(settings.zipcode, user_temp_celsius, settings.update_interval_min);
        }

        // The settings screen passes its own label as user_data.
        lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
        if (label != NULL) {
            lv_label_set_text(label, user_temp_celsius ? "Celsius" : "Fahrenheit");
        }

        update_weather_display();
    }
}

// Tapping the glucose unit label toggles mg/dL <-> mmol/L. Glucose stays
// canonical mg/dL internally; this only flips the display unit.
void glucose_unit_toggle_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    user_glucose_mmol = !user_glucose_mmol;
    ESP_LOGI(TAG, "Glucose unit toggled to %s", cygm_glucose_unit());
    nvs_save_glucose_mmol(user_glucose_mmol);

    // Refresh the unit label and the live value so the change is immediate.
    if (label_unit != NULL) {
        lv_label_set_text(label_unit, cygm_glucose_unit());
    }
    if (label_glucose != NULL && glucose_data_valid) {
        char glucose_str[16];
        cygm_format_glucose(current_glucose, glucose_str, sizeof(glucose_str));
        lv_label_set_text(label_glucose, glucose_str);
    }
}

void menu_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Settings button clicked - switching to menu screen");

        home_screen_active = false;
        pause_background_tasks = true;
        dismiss_nodata_overlay();
        dismiss_wifi_disconnected_overlay();
        ESP_LOGI(TAG, "Background tasks paused (leaving home screen)");

        if (lvgl_port_lock(1)) {
            // Created on demand; freed again on the way back to home.
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

        home_screen_active = false;
        pause_background_tasks = true;
        dismiss_nodata_overlay();
        dismiss_wifi_disconnected_overlay();
        ESP_LOGI(TAG, "Background tasks paused (leaving home screen)");

        // The scan and its UI need the ~30KB the persistent CGM client holds.
        ESP_LOGI(TAG, "Closing CGM persistent client before WiFi scan");
        dexcom_close_persistent_client();
        libre_close_persistent_client();
        ESP_LOGI(TAG, "Free heap after closing CGM client: %lu bytes (internal: %lu)",
                 esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        if (lvgl_port_lock(1)) {
            if (screen_wifi_list != NULL) {
                lv_obj_del(screen_wifi_list);
                screen_wifi_list = NULL;
            }
            create_wifi_list_screen();
            lv_scr_load(screen_wifi_list);
            lvgl_port_unlock();
        }

        show_wifi_scanning_overlay();
        xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
    }
}

void back_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back button clicked - returning to home screen");

        home_screen_active = true;
        pause_background_tasks = false;
        ESP_LOGI(TAG, "Background tasks resumed (returned to home screen)");

        // Load home and free the abandoned menu under ONE lock: lv_scr_load with
        // no animation swaps synchronously, so the old screen can go immediately.
        if (lvgl_port_lock(1)) {
            lv_scr_load(screen_home);

            if (screen_menu != NULL) {
                ESP_LOGI(TAG, "Deleting menu screen to free memory");
                lv_obj_del(screen_menu);
                screen_menu = NULL;
                ESP_LOGI(TAG, "Menu screen deleted. Free heap: %lu bytes", esp_get_free_heap_size());
            }
            // The WiFi screens route their back buttons here too, and must not be
            // stranded: a populated list plus password screen and keyboard is
            // 7-11KB of the 36KB LVGL pool.
            if (screen_wifi_password != NULL) {
                lv_obj_del(screen_wifi_password);
                screen_wifi_password = NULL;
                password_field = NULL;
                keyboard = NULL;
            }
            if (screen_wifi_list != NULL) {
                lv_obj_del(screen_wifi_list);
                screen_wifi_list = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Tapping the time label toggles 12hr/24hr format.
void clock_label_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Clock label clicked - toggling time format");

        user_24hr_format = !user_24hr_format;

        esp_err_t ret = nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Time format saved: %s", user_24hr_format ? "24hr" : "12hr");
        } else {
            ESP_LOGW(TAG, "Failed to save time format to NVS");
        }

        // The settings screen passes its own label as user_data.
        lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
        if (label != NULL) {
            lv_label_set_text(label, user_24hr_format ? "24 Hour" : "12 Hour");
        }

        update_time_display();
        update_sunrise_sunset_display();   // sunrise/sunset follow the same format
    }
}

// Tapping the date label toggles month-day <-> day-month.
void date_label_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    user_date_dmy = !user_date_dmy;
    ESP_LOGI(TAG, "Date format toggled to %s", user_date_dmy ? "D/M" : "M/D");
    nvs_save_date_dmy(user_date_dmy);
    update_time_display();
}

// Main-menu item tap handler — dispatches to the selected sub-screen
void menu_item_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int item_index = (int)(intptr_t)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Menu item %d clicked", item_index);

        switch (item_index) {
            case 0:  // CGM Settings
                ESP_LOGI(TAG, "Launching CGM Settings...");
                if (lvgl_port_lock(1)) {
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

                // The scan and its UI need the ~30KB the persistent CGM client holds.
                ESP_LOGI(TAG, "Closing CGM persistent client before WiFi scan");
                dexcom_close_persistent_client();
                libre_close_persistent_client();
                ESP_LOGI(TAG, "Free heap after closing CGM client: %lu bytes (internal: %lu)",
                         esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

                if (lvgl_port_lock(1)) {
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

                show_wifi_scanning_overlay();   // the scan task hides it when done
                xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
                break;
            case 2:  // Time/Date/Weather Settings (combined)
                ESP_LOGI(TAG, "Launching Time/Weather settings...");
                if (lvgl_port_lock(1)) {
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

// Append a message to the boot log.
void add_boot_log(const char *msg) {
    // Buffer the message even before LVGL is up, so nothing is lost.
    size_t current_len = strlen(boot_log_buffer);
    size_t remaining = sizeof(boot_log_buffer) - current_len - 1;

    if (remaining > strlen(msg) + 1) {
        if (current_len > 0) {
            strncat(boot_log_buffer, "\n", remaining);
            remaining--;
        }
        strncat(boot_log_buffer, msg, remaining);
    }

    if (boot_log_label != NULL) {
        if (lvgl_port_lock(1)) {
            lv_label_set_text(boot_log_label, boot_log_buffer);
            lvgl_port_unlock();
        }
    }

    // Boot milestones drive the progress bar, matched off the message text.
    if (boot_progress_bar != NULL) {
        if (lvgl_port_lock(1)) {
            int progress = 0;

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

            if (progress > 0) {
                lv_bar_set_value(boot_progress_bar, progress, LV_ANIM_OFF);  // LV_ANIM_ON calls lv_anim_start — freeze risk
            }

            lvgl_port_unlock();
        }
    }
}

// Sunrise / sunset icons (12x12). At this size they must differ by silhouette,
// not hue: chevron direction, horizon height, and whether the sun bulges above
// the line or hangs below it. lv_canvas_draw_arc paints the ring
// [radius - width, radius], so width >= radius gives a solid half-disc.
#define SUN_ICON_RISE_COLOR 0xFACC15   // COLOR_YELLOW — morning light
#define SUN_ICON_SET_COLOR  0xF97316   // deep orange; COLOR_ORANGE reads as
                                       // yellow next to the sunrise at 12px

void draw_sunrise_icon(lv_obj_t *canvas) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    lv_point_t pts[2];

    // Chevron pointing UP, rows 0..4
    line_dsc.color = lv_color_hex(SUN_ICON_RISE_COLOR);
    line_dsc.width = 2;
    line_dsc.round_start = 1;
    line_dsc.round_end = 1;
    pts[0] = (lv_point_t){2, 3};
    pts[1] = (lv_point_t){6, 0};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);
    pts[0] = (lv_point_t){6, 0};
    pts[1] = (lv_point_t){10, 3};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);

    // Ground line along the bottom row, drawn before the sun so the dome
    // covers its middle and the sun reads as emerging from it
    line_dsc.color = lv_color_hex(COLOR_TEXT_DIM);
    line_dsc.width = 1;
    line_dsc.round_start = 0;
    line_dsc.round_end = 0;
    pts[0] = (lv_point_t){0, 10};
    pts[1] = (lv_point_t){11, 10};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);

    // Half-sun ABOVE the line: solid dome, rows 7..10, leaves the line showing
    // 3px either side so the horizon still reads
    arc_dsc.color = lv_color_hex(SUN_ICON_RISE_COLOR);
    arc_dsc.width = 3;
    lv_canvas_draw_arc(canvas, 6, 10, 3, 180, 360, &arc_dsc);
}

void draw_sunset_icon(lv_obj_t *canvas) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    lv_point_t pts[2];

    // Chevron pointing DOWN, rows 1..5
    line_dsc.color = lv_color_hex(SUN_ICON_SET_COLOR);
    line_dsc.width = 2;
    line_dsc.round_start = 1;
    line_dsc.round_end = 1;
    pts[0] = (lv_point_t){2, 1};
    pts[1] = (lv_point_t){6, 4};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);
    pts[0] = (lv_point_t){6, 4};
    pts[1] = (lv_point_t){10, 1};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);

    // Horizon across the middle — the sun has already dropped through it
    line_dsc.color = lv_color_hex(COLOR_TEXT_DIM);
    line_dsc.width = 1;
    line_dsc.round_start = 0;
    line_dsc.round_end = 0;
    pts[0] = (lv_point_t){0, 7};
    pts[1] = (lv_point_t){11, 7};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);

    // Half-sun BELOW the line: solid dome hanging down, rows 7..10
    arc_dsc.color = lv_color_hex(SUN_ICON_SET_COLOR);
    arc_dsc.width = 3;
    lv_canvas_draw_arc(canvas, 6, 7, 3, 0, 180, &arc_dsc);
}

// ===== SPLASH SCREEN =====
void create_splash_screen(void) {
    screen_splash = lv_obj_create(NULL);
    lv_obj_add_style(screen_splash, &style_bg, 0);
    lv_obj_clear_flag(screen_splash, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Zone color bars at top (glucose range: red/orange/green/yellow/red) ----
    // Proportions 1:1:3:1:1 = 7 parts across 320px
    #define ZONE_BAR_H 4
    const struct { int w; uint32_t color; } zones[] = {
        {46, COLOR_RED}, {46, COLOR_ORANGE}, {136, COLOR_GREEN},
        {46, COLOR_YELLOW}, {46, COLOR_RED}
    };
    int zone_x = 0;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *z = lv_obj_create(screen_splash);
        lv_obj_remove_style_all(z);
        lv_obj_set_size(z, zones[i].w, ZONE_BAR_H);
        lv_obj_set_pos(z, zone_x, 0);
        lv_obj_set_style_bg_color(z, lv_color_hex(zones[i].color), 0);
        lv_obj_set_style_bg_opa(z, LV_OPA_COVER, 0);
        zone_x += zones[i].w;
    }

    // ---- Top gradient wave (blue tint fading down) ----
    lv_obj_t *top_wave = lv_obj_create(screen_splash);
    lv_obj_remove_style_all(top_wave);
    lv_obj_set_size(top_wave, 320, 60);
    lv_obj_set_pos(top_wave, 0, ZONE_BAR_H);
    lv_obj_set_style_bg_color(top_wave, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(top_wave, LV_OPA_10, 0);
    lv_obj_set_style_bg_grad_color(top_wave, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(top_wave, LV_GRAD_DIR_VER, 0);

    // ---- Ghost typography backdrop (large faint CYGM behind everything) ----
    lv_obj_t *ghost = lv_label_create(screen_splash);
    lv_label_set_text(ghost, "CYGM");
    lv_obj_set_style_text_font(ghost, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ghost, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_opa(ghost, LV_OPA_10, 0);
    lv_obj_set_style_text_letter_space(ghost, 16, 0);
    lv_obj_align(ghost, LV_ALIGN_CENTER, 0, -18);

    // ---- Main title: CYGM ----
    lv_obj_t *title = lv_label_create(screen_splash);
    lv_label_set_text(title, "CYGM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_letter_space(title, 10, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -52);

    // ---- Tagline ----
    lv_obj_t *tagline = lv_label_create(screen_splash);
    lv_label_set_text(tagline, "CONTINUOUS GLUCOSE MONITOR");
    lv_obj_set_style_text_font(tagline, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(tagline, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_letter_space(tagline, 3, 0);
    lv_obj_align(tagline, LV_ALIGN_CENTER, 0, -28);

    // ---- Separator line ----
    lv_obj_t *sep = lv_obj_create(screen_splash);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 80, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sep, 1, 0);
    lv_obj_align(sep, LV_ALIGN_CENTER, 0, -16);

    // ---- Author ----
    lv_obj_t *author = lv_label_create(screen_splash);
    lv_label_set_text(author, "Carl Brothers");
    lv_obj_set_style_text_font(author, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(author, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(author, LV_ALIGN_CENTER, 0, -2);

    // ---- Website (accent light blue) ----
    lv_obj_t *website = lv_label_create(screen_splash);
    lv_label_set_text(website, "CYGM.me");
    lv_obj_set_style_text_font(website, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(website, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(website, LV_ALIGN_CENTER, 0, 12);

    // ---- Version (1 size larger, stands out) ----
    lv_obj_t *version = lv_label_create(screen_splash);
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "v%s", CYGM_VERSION_STRING);
    lv_label_set_text(version, version_str);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(version, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_letter_space(version, 1, 0);
    lv_obj_align(version, LV_ALIGN_CENTER, 0, 28);

    // ---- Boot status label (raised higher) ----
    boot_log_label = lv_label_create(screen_splash);
    lv_label_set_text(boot_log_label, "Initializing...");
    lv_label_set_long_mode(boot_log_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(boot_log_label, 240);
    lv_obj_set_style_text_font(boot_log_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(boot_log_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(boot_log_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(boot_log_label, LV_ALIGN_BOTTOM_MID, 0, -26);

    // ---- Slim progress bar (raised higher) ----
    boot_progress_bar = lv_bar_create(screen_splash);
    lv_obj_set_size(boot_progress_bar, 220, 3);
    lv_obj_align(boot_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_bar_set_value(boot_progress_bar, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(boot_progress_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(boot_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(boot_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(boot_progress_bar, 2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(boot_progress_bar, lv_color_hex(0x1D4ED8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(boot_progress_bar, lv_color_hex(COLOR_ACCENT_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(boot_progress_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(boot_progress_bar, 2, LV_PART_INDICATOR);

    // ---- Bottom gradient wave ----
    lv_obj_t *bot_wave = lv_obj_create(screen_splash);
    lv_obj_remove_style_all(bot_wave);
    lv_obj_set_size(bot_wave, 320, 40);
    lv_obj_set_pos(bot_wave, 0, 200);
    lv_obj_set_style_bg_color(bot_wave, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(bot_wave, LV_OPA_10, 0);
    lv_obj_set_style_bg_grad_color(bot_wave, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(bot_wave, LV_GRAD_DIR_VER, 0);

    // ---- Zone color bars at bottom ----
    zone_x = 0;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *z = lv_obj_create(screen_splash);
        lv_obj_remove_style_all(z);
        lv_obj_set_size(z, zones[i].w, ZONE_BAR_H);
        lv_obj_set_pos(z, zone_x, 240 - ZONE_BAR_H);
        lv_obj_set_style_bg_color(z, lv_color_hex(zones[i].color), 0);
        lv_obj_set_style_bg_opa(z, LV_OPA_COVER, 0);
        zone_x += zones[i].w;
    }

    memset(boot_log_buffer, 0, sizeof(boot_log_buffer));
}


// ==================== Main Application ====================

void app_main(void) {
    // Sleep guard: a USB unplug fakes a wake two ways — a brownout reset from the
    // rail dip, or a GPIO 0 glitch on the EXT0 wake pin as the UART bridge dies.
    esp_reset_reason_t boot_reason = esp_reset_reason();

    if (boot_reason == ESP_RST_BROWNOUT) {
        // Power dip during USB disconnect — return to sleep immediately
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
        esp_deep_sleep_start();
    }

    if (boot_reason == ESP_RST_DEEPSLEEP) {
        // Verify the button is ACTUALLY pressed: a USB-disconnect glitch on
        // GPIO 0 settles within ~10ms, while a real press holds LOW for 200ms+.
        gpio_reset_pin(GPIO_NUM_0);
        gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
        gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(100));  // Let glitch settle
        if (gpio_get_level(GPIO_NUM_0) == 1) {
            // Button NOT pressed — spurious wakeup from USB disconnect
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
            esp_deep_sleep_start();
        }
        // Button IS pressed — legitimate wake, continue boot
    }

    // The brownout detector has done its job above. Disable it for normal
    // operation: USB disconnect dips the rail enough to trip false resets, and the
    // battery monitor already shuts down at 3.2V, well above the 2.43V threshold.
    esp_brownout_disable();

    ESP_LOGI(TAG, "=== CYGM - Continuous Glucose Monitor ===");
    ESP_LOGI(TAG, "Hardware: JC2432W328 (ESP32-D0WD)");
    ESP_LOGI(TAG, "Core 0: WiFi | Core 1: UI/Touch");

    add_boot_log("CYGM Starting...");

    add_boot_log("Init NVS storage");
    ESP_ERROR_CHECK(nvs_config_init());

    // Serialises every network/SSL operation.
    network_mutex = xSemaphoreCreateMutex();
    if (network_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create network mutex!");
    } else {
        ESP_LOGI(TAG, "Network mutex created for memory management");
    }

    add_boot_log("Load settings");
    load_time_settings();
    load_weather_settings();

    esp_err_t alarm_ret = nvs_load_alarm_settings(&current_alarm_settings);
    if (alarm_ret != ESP_OK) {
        ESP_LOGI(TAG, "No alarm settings found, using defaults");
        nvs_get_default_alarm_settings(&current_alarm_settings);
    } else {
        ESP_LOGI(TAG, "Alarm settings loaded from NVS");
    }

    // Alarm-engine extension blob (quiet hours, escalation, data-gap watchdog,
    // predictive alerts) under its own versioned key, so growing it can never
    // reset the thresholds above. Seed defaults first: the loader migrates a
    // short or old blob on top rather than wiping it.
    cygm_alarm_ext_defaults(&alarm_ext_settings);
    esp_err_t alarm_ext_ret = nvs_load_alarm_ext(&alarm_ext_settings);
    ESP_LOGI(TAG, "Alarm engine settings: %s (v%u, %u bytes)",
             alarm_ext_ret == ESP_OK ? "loaded from NVS" : "defaults",
             (unsigned)alarm_ext_settings.version, (unsigned)alarm_ext_settings.size);


    add_boot_log("Init SD logger");
    sd_logger_init();  // Non-fatal if no card inserted
    screenshot_init(); // Serial command listener (type "ss" in monitor)

    // Reset reason — the first thing to check when diagnosing a crash or watchdog.
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

    add_boot_log("Load glucose history");
    ESP_ERROR_CHECK(glucose_history_init());
    glucose_history_register_shutdown_handler();

    nvs_load_brightness(&screen_brightness_percent);
    saved_brightness_percent = screen_brightness_percent;
    dim_while_charging = nvs_load_dim_while_charging();
    sd_glucose_logging_enabled = nvs_get_sd_glucose_logging();

    add_boot_log("Init display");
    ESP_ERROR_CHECK(display_init());
    display_set_brightness(screen_brightness_percent);

    add_boot_log("Init LED");
    led_init();

    add_boot_log("Init buzzer");
    buzzer_init();

    // Pinned to Core 1 so LEDC stays on the LED/backlight core (avoids Int-WDT).
    if (xTaskCreatePinnedToCore(alarm_buzzer_task, "alarm_buzzer", 2560, NULL, 4,
                                &alarm_buzzer_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create alarm_buzzer task — alarm audio disabled");
        alarm_buzzer_task_handle = NULL;
    }

    add_boot_log("Init battery");
    battery_init();

    add_boot_log("Start UI task");
    BaseType_t ui_rc = xTaskCreatePinnedToCore(
        ui_task_core1,
        "ui_task",
        8192,
        NULL,
        5,  // Higher priority
        &ui_task_handle,
        1   // Core 1
    );
    if (ui_rc != pdPASS) {
        // Boot-critical: without the UI task the device is a black screen.
        // Log loudly so a fragmented-heap failure is diagnosable, not silent.
        ESP_LOGE(TAG, "FATAL: failed to create ui_task (rc=%d) — UI will not run", ui_rc);
        add_boot_log("ERROR: UI task failed!");
    }

    add_boot_log("Start WiFi task");
    BaseType_t wifi_rc = xTaskCreatePinnedToCore(
        wifi_task_core0,
        "wifi_task",
        8192,
        NULL,
        4,  // Medium priority
        &wifi_task_handle,
        0   // Core 0
    );
    if (wifi_rc != pdPASS) {
        ESP_LOGE(TAG, "FATAL: failed to create wifi_task (rc=%d) — no network", wifi_rc);
        add_boot_log("ERROR: WiFi task failed!");
    }

    add_boot_log("System ready!");
    ESP_LOGI(TAG, "System initialized - tasks running");

    // Confirm OTA boot (prevents automatic rollback to previous firmware)
    update_confirm_boot();

    // Play boot-up sound — but ONLY on fresh power-on.
    // Crash/watchdog/software reboots must be silent (user may be sleeping).
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_POWERON) {
        vTaskDelay(pdMS_TO_TICKS(500));
        play_boot_sound();
    } else {
        ESP_LOGI(TAG, "Silent boot (reset reason: %d)", reset_reason);
    }
}
