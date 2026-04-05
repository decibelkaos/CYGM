/*
 * home_screen.c
 *
 * Main home screen implementation
 */

#include "home_screen.h"
#include "shared_state.h"
#include "main.h"
#include "features/weather_system.h"
#include "features/glucose_history.h"
#include "hardware/display.h"
#include "hardware/battery.h"
#include "nvs_config.h"
#include "ui/glucose_chart.h"
#include "ui/wifi_screens.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "sd_logger.h"
#include "lvgl.h"
#include <stdlib.h>
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "HOME_SCREEN";

// Large glucose font (digits only, generated via lv_font_conv)
extern const lv_font_t font_montserrat_120;

// File-scope references for expand/collapse repositioning
static lv_obj_t *brightness_btn_ref = NULL;
static lv_obj_t *expanded_weather_icon = NULL;  // Mini weather icon for expanded top bar
static lv_color_t canvas_buf_trend[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 80)];  // Sized for expanded 80x80

// Forward declarations
static void brightness_icon_event_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);
static void brightness_ok_event_cb(lv_event_t *e);
static void create_brightness_overlay(void);
static void close_brightness_overlay(void);
static void draw_lightbulb_icon(lv_obj_t *canvas, uint32_t color);
static void battery_tap_event_cb(lv_event_t *e);
static void glucose_timer_update_cb(lv_timer_t *timer);
static void trend_arrow_event_cb(lv_event_t *e);
static void home_gesture_event_cb(lv_event_t *e);
static void expand_cgm_card(void);
static void collapse_cgm_card(void);
static void expand_left_fade_done_cb(lv_anim_t *a);

// Animation helper callbacks for expand/collapse
static void anim_x_cb(void *var, int32_t v) {
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}
static void anim_width_cb(void *var, int32_t v) {
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)v);
}
static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

// ==================== Ambient Tint Helpers ====================

// Linear interpolation between two colors. t=0 returns c1, t=255 returns c2.
// Uses lv_color_mix which handles RGB565 format correctly.
static lv_color_t color_lerp(lv_color_t c1, lv_color_t c2, uint8_t t) {
    // lv_color_mix(c1, c2, mix) — mix=255 returns c1, mix=0 returns c2
    return lv_color_mix(c2, c1, t);
}

// Blend a tint color onto the card background at a given intensity (0-255).
// intensity=0 returns pure card bg, intensity=255 returns pure tint.
static lv_color_t blend_tint(uint32_t tint_hex, uint8_t intensity) {
    return color_lerp(lv_color_hex(COLOR_CARD_BG), lv_color_hex(tint_hex), intensity);
}

// Compute glucose zone tint color based on current glucose and alarm thresholds.
// Returns the blended card background color.
static lv_color_t compute_glucose_zone_tint(int glucose) {
    int low_alarm_val   = current_alarm_settings.low_alarm.threshold;
    int low_warn_val    = current_alarm_settings.low_warning.threshold;
    int high_warn_val   = current_alarm_settings.high_warning.threshold;
    int high_alarm_val  = current_alarm_settings.high_alarm.threshold;

    // Tint intensity: 0 = no tint (pure card bg), 18 = max subtle tint (~7%)
    const uint8_t MAX_TINT = 18;

    if (glucose <= low_alarm_val) {
        // At or below low alarm: full red tint
        return blend_tint(COLOR_TINT_RED, MAX_TINT);
    } else if (glucose <= low_warn_val) {
        // Between low alarm and low warning: red to yellow gradient
        int range = low_warn_val - low_alarm_val;
        if (range <= 0) return blend_tint(COLOR_TINT_YELLOW, MAX_TINT);
        uint8_t t = (uint8_t)((glucose - low_alarm_val) * 255 / range);
        lv_color_t zone_color = color_lerp(lv_color_hex(COLOR_TINT_RED), lv_color_hex(COLOR_TINT_YELLOW), t);
        return color_lerp(lv_color_hex(COLOR_CARD_BG), zone_color, MAX_TINT);
    } else if (glucose < high_warn_val) {
        // In range: green tint, weakest at midpoint
        int midpoint = (low_warn_val + high_warn_val) / 2;
        int half_range = (high_warn_val - low_warn_val) / 2;
        if (half_range <= 0) return blend_tint(COLOR_TINT_GREEN, MAX_TINT / 2);
        int dist = abs(glucose - midpoint);
        // Closer to midpoint = less tint (more calm), closer to edge = more tint
        uint8_t edge_factor = (uint8_t)(dist * MAX_TINT / half_range);
        uint8_t intensity = MAX_TINT / 3 + edge_factor * 2 / 3;
        if (intensity > MAX_TINT) intensity = MAX_TINT;
        return blend_tint(COLOR_TINT_GREEN, intensity);
    } else if (glucose < high_alarm_val) {
        // Between high warning and high alarm: yellow to red gradient
        int range = high_alarm_val - high_warn_val;
        if (range <= 0) return blend_tint(COLOR_TINT_YELLOW, MAX_TINT);
        uint8_t t = (uint8_t)((glucose - high_warn_val) * 255 / range);
        lv_color_t zone_color = color_lerp(lv_color_hex(COLOR_TINT_YELLOW), lv_color_hex(COLOR_TINT_RED), t);
        return color_lerp(lv_color_hex(COLOR_CARD_BG), zone_color, MAX_TINT);
    } else {
        // At or above high alarm: full red tint
        return blend_tint(COLOR_TINT_RED, MAX_TINT);
    }
}

// Compute time-of-day tint adjustment. Returns a color to blend additively.
// Uses sunrise_time and sunset_time globals from weather_system.
static lv_color_t compute_time_of_day_tint(lv_color_t base) {
    time_t now;
    time(&now);

    // If sunrise/sunset not available, return base unchanged
    if (sunrise_time == 0 || sunset_time == 0) return base;

    const int GOLDEN_MINUTES = 30;
    const uint8_t TOD_TINT = 8;  // ~3% overlay

    double diff_sunrise = difftime(now, sunrise_time) / 60.0;  // minutes from sunrise
    double diff_sunset  = difftime(now, sunset_time) / 60.0;   // minutes from sunset

    // Golden hour around sunrise (+-30 min)
    if (diff_sunrise >= -GOLDEN_MINUTES && diff_sunrise <= GOLDEN_MINUTES) {
        uint8_t intensity = (uint8_t)(TOD_TINT * (GOLDEN_MINUTES - abs((int)diff_sunrise)) / GOLDEN_MINUTES);
        return color_lerp(base, lv_color_hex(COLOR_TINT_AMBER), intensity);
    }
    // Golden hour around sunset (+-30 min)
    if (diff_sunset >= -GOLDEN_MINUTES && diff_sunset <= GOLDEN_MINUTES) {
        uint8_t intensity = (uint8_t)(TOD_TINT * (GOLDEN_MINUTES - abs((int)diff_sunset)) / GOLDEN_MINUTES);
        return color_lerp(base, lv_color_hex(COLOR_TINT_AMBER), intensity);
    }
    // Night (after sunset+30 or before sunrise-30)
    if (diff_sunset > GOLDEN_MINUTES || diff_sunrise < -GOLDEN_MINUTES) {
        return color_lerp(base, lv_color_hex(COLOR_TINT_COOL), TOD_TINT);
    }

    // Daytime — no change
    return base;
}

// Combined: compute final card tint from glucose + time of day
static lv_color_t compute_ambient_tint(int glucose) {
    lv_color_t zone_tint = compute_glucose_zone_tint(glucose);
    return compute_time_of_day_tint(zone_tint);
}

// Public: update ambient tint on both cards (call after glucose update)
void update_ambient_tint(void) {
    if (!glucose_data_valid || current_glucose <= 0) return;
    if (left_card == NULL || right_card == NULL) return;
    lv_color_t tint = compute_ambient_tint(current_glucose);
    lv_obj_set_style_bg_color(left_card, tint, 0);
    lv_obj_set_style_bg_color(right_card, tint, 0);
}

// ==================== No-Data / Sensor Change Overlay ====================

static lv_obj_t *nodata_overlay = NULL;
static bool nodata_overlay_shown_this_cycle = false;  // Don't re-show after user dismisses
static lv_timer_t *nodata_auto_dismiss_timer = NULL;
static lv_obj_t *nodata_countdown_label = NULL;
static int nodata_countdown_sec = 30;

// Clean up the auto-dismiss timer
static void nodata_cleanup_timer(void) {
    if (nodata_auto_dismiss_timer) {
        lv_timer_del(nodata_auto_dismiss_timer);
        nodata_auto_dismiss_timer = NULL;
    }
    nodata_countdown_label = NULL;
}

static void nodata_yes_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nodata_cleanup_timer();
    sensor_change_mode = true;
    ESP_LOGI(TAG, "User confirmed sensor change");
    if (nodata_overlay) { lv_obj_del(nodata_overlay); nodata_overlay = NULL; }
    update_glucose_display();
}

static void nodata_no_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nodata_cleanup_timer();
    sensor_change_mode = false;
    ESP_LOGI(TAG, "User denied sensor change — showing error state");
    if (nodata_overlay) { lv_obj_del(nodata_overlay); nodata_overlay = NULL; }
    update_glucose_display();
}

// Auto-dismiss timer: ticks every 1 second, auto-selects "No" after 30 seconds.
// SAFETY: The device must NEVER stop trying to fetch glucose data because
// the user was asleep and didn't respond to this overlay.
static void nodata_auto_dismiss_cb(lv_timer_t *timer) {
    nodata_countdown_sec--;

    // Update countdown label (only valid while overlay exists)
    if (nodata_overlay && nodata_countdown_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Auto-dismiss in %ds", nodata_countdown_sec);
        lv_label_set_text(nodata_countdown_label, buf);
    }

    if (nodata_countdown_sec <= 0) {
        ESP_LOGW(TAG, "No-data overlay auto-dismissed (30s timeout) — defaulting to NO");
        nodata_cleanup_timer();
        sensor_change_mode = false;
        if (nodata_overlay) { lv_obj_del(nodata_overlay); nodata_overlay = NULL; }
        update_glucose_display();
    }
}

void show_nodata_overlay(void) {
    if (nodata_overlay != NULL || nodata_overlay_shown_this_cycle) return;
    nodata_overlay_shown_this_cycle = true;

    // Dimmed backdrop
    nodata_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(nodata_overlay);
    lv_obj_set_size(nodata_overlay, 320, 240);
    lv_obj_set_style_bg_color(nodata_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(nodata_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(nodata_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(nodata_overlay);

    // Glass card
    lv_obj_t *card = lv_obj_create(nodata_overlay);
    lv_obj_set_size(card, 280, 170);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3A4A), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 25, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon + title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  No Data Received");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // Divider
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 250, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 42);

    // Question
    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, "Are you changing your\nCGM sensor?");
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 54);

    // Yes button (green outline)
    lv_obj_t *btn_yes = lv_btn_create(card);
    lv_obj_set_size(btn_yes, 110, 36);
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 20, -14);
    lv_obj_set_style_bg_opa(btn_yes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0x1a3a2a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_yes, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_yes, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_border_width(btn_yes, 1, 0);
    lv_obj_set_style_radius(btn_yes, 8, 0);
    lv_obj_set_style_shadow_width(btn_yes, 0, 0);
    lv_obj_t *lbl_yes = lv_label_create(btn_yes);
    lv_label_set_text(lbl_yes, "Yes");
    lv_obj_set_style_text_color(lbl_yes, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_center(lbl_yes);
    lv_obj_add_event_cb(btn_yes, nodata_yes_cb, LV_EVENT_CLICKED, NULL);

    // No button (red outline)
    lv_obj_t *btn_no = lv_btn_create(card);
    lv_obj_set_size(btn_no, 110, 36);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
    lv_obj_set_style_bg_opa(btn_no, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x3a1a1a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_no, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_no, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(btn_no, 1, 0);
    lv_obj_set_style_radius(btn_no, 8, 0);
    lv_obj_set_style_shadow_width(btn_no, 0, 0);
    lv_obj_t *lbl_no = lv_label_create(btn_no);
    lv_label_set_text(lbl_no, "No");
    lv_obj_set_style_text_color(lbl_no, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(lbl_no);
    lv_obj_add_event_cb(btn_no, nodata_no_cb, LV_EVENT_CLICKED, NULL);

    // Countdown label (bottom of card)
    nodata_countdown_sec = 30;
    nodata_countdown_label = lv_label_create(card);
    lv_label_set_text(nodata_countdown_label, "Auto-dismiss in 30s");
    lv_obj_set_style_text_font(nodata_countdown_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nodata_countdown_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(nodata_countdown_label, LV_ALIGN_TOP_MID, 0, 100);

    // Auto-dismiss timer: defaults to "No" after 30 seconds.
    // SAFETY: If user is asleep, device must keep trying to fetch data.
    nodata_auto_dismiss_timer = lv_timer_create(nodata_auto_dismiss_cb, 1000, NULL);
}

void dismiss_nodata_overlay(void) {
    nodata_cleanup_timer();
    if (nodata_overlay != NULL) {
        lv_obj_del(nodata_overlay);
        nodata_overlay = NULL;
    }
}

// Reset the "already shown" flag when data comes back
void reset_nodata_overlay_cycle(void) {
    nodata_overlay_shown_this_cycle = false;
}

// ==================== WiFi Disconnected Overlay ====================

static lv_obj_t *wifi_disc_overlay = NULL;

static void wifi_disc_dismiss_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "WiFi disconnect overlay dismissed by user");
    if (wifi_disc_overlay) { lv_obj_del(wifi_disc_overlay); wifi_disc_overlay = NULL; }
}

static void wifi_disc_settings_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "User tapped WiFi Settings from disconnect overlay");

    // Clean up overlay first
    if (wifi_disc_overlay) { lv_obj_del(wifi_disc_overlay); wifi_disc_overlay = NULL; }

    // Leave home screen
    home_screen_active = false;
    pause_background_tasks = true;

    // Free memory for WiFi scan (same flow as menu WiFi Setup)
    dexcom_close_persistent_client();
    libre_close_persistent_client();

    // Navigate to WiFi list screen (already in LVGL context — no lock needed)
    if (screen_wifi_list != NULL) {
        lv_obj_del(screen_wifi_list);
        screen_wifi_list = NULL;
    }
    create_wifi_list_screen();
    lv_scr_load(screen_wifi_list);

    // Start scan
    show_wifi_scanning_overlay();
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
}

void show_wifi_disconnected_overlay(void) {
    // Don't duplicate, and only show on home screen
    if (wifi_disc_overlay != NULL || !home_screen_active) return;

    // Dimmed backdrop
    wifi_disc_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(wifi_disc_overlay);
    lv_obj_set_size(wifi_disc_overlay, 320, 240);
    lv_obj_set_style_bg_color(wifi_disc_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_disc_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(wifi_disc_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wifi_disc_overlay);

    // Glass card — orange border for warning (not red = alarm)
    lv_obj_t *card = lv_obj_create(wifi_disc_overlay);
    lv_obj_set_size(card, 260, 155);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 25, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi icon (large, orange)
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "WiFi Disconnected");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    // Subtitle
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "Auto-reconnect in progress...");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 68);

    // Divider
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 230, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 88);

    // "WiFi Settings" button (ghost-blue, primary action)
    lv_obj_t *btn_settings = lv_btn_create(card);
    lv_obj_set_size(btn_settings, 115, 34);
    lv_obj_align(btn_settings, LV_ALIGN_BOTTOM_LEFT, 15, -12);
    lv_obj_set_style_bg_opa(btn_settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x1a2a45), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_settings, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_settings, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(btn_settings, 1, 0);
    lv_obj_set_style_radius(btn_settings, 8, 0);
    lv_obj_set_style_shadow_width(btn_settings, 0, 0);
    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_color(lbl_settings, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl_settings);
    lv_obj_add_event_cb(btn_settings, wifi_disc_settings_cb, LV_EVENT_CLICKED, NULL);

    // "Dismiss" button (ghost-gray, secondary)
    lv_obj_t *btn_dismiss = lv_btn_create(card);
    lv_obj_set_size(btn_dismiss, 100, 34);
    lv_obj_align(btn_dismiss, LV_ALIGN_BOTTOM_RIGHT, -15, -12);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_dismiss, lv_color_hex(0x2a2a2a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_dismiss, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_dismiss, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_border_width(btn_dismiss, 1, 0);
    lv_obj_set_style_radius(btn_dismiss, 8, 0);
    lv_obj_set_style_shadow_width(btn_dismiss, 0, 0);
    lv_obj_t *lbl_dismiss = lv_label_create(btn_dismiss);
    lv_label_set_text(lbl_dismiss, "Dismiss");
    lv_obj_set_style_text_color(lbl_dismiss, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(lbl_dismiss);
    lv_obj_add_event_cb(btn_dismiss, wifi_disc_dismiss_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGW(TAG, "WiFi disconnected overlay shown");
    sd_log(TAG, "WiFi disconnected overlay shown");
}

void dismiss_wifi_disconnected_overlay(void) {
    if (wifi_disc_overlay != NULL) {
        lv_obj_del(wifi_disc_overlay);
        wifi_disc_overlay = NULL;
        ESP_LOGI(TAG, "WiFi disconnected overlay dismissed (reconnected)");
    }
}

// ==================== Login Success Overlay ====================

static lv_obj_t *login_success_overlay = NULL;

static void login_success_dismiss_cb(lv_event_t *e) {
    (void)e;
    if (login_success_overlay) {
        lv_obj_del(login_success_overlay);
        login_success_overlay = NULL;
    }
}

static void login_success_timer_cb(lv_timer_t *timer) {
    lv_timer_del(timer);
    if (login_success_overlay) {
        lv_obj_del(login_success_overlay);
        login_success_overlay = NULL;
    }
}

void show_login_success_overlay_ui(void) {
    if (login_success_overlay != NULL) return;

    // Dimmed backdrop
    login_success_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(login_success_overlay);
    lv_obj_set_size(login_success_overlay, 320, 240);
    lv_obj_set_style_bg_color(login_success_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(login_success_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(login_success_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(login_success_overlay);
    lv_obj_add_event_cb(login_success_overlay, login_success_dismiss_cb, LV_EVENT_CLICKED, NULL);

    // Glass card — green accent for success
    lv_obj_t *card = lv_obj_create(login_success_overlay);
    lv_obj_set_size(card, 250, 140);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 25, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Check icon
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 16);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Login Successful");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

    // Subtitle
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "CGM data will begin updating shortly");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, 220);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 88);

    // Hint
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Tap to dismiss");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Auto-dismiss after 4 seconds
    lv_timer_create(login_success_timer_cb, 4000, NULL);

    ESP_LOGI(TAG, "Login success overlay shown");
}

// ==================== Legal Disclaimer Overlay ====================

static lv_obj_t *disclaimer_overlay = NULL;
static lv_obj_t *disclaimer_checkbox = NULL;

static void disclaimer_agree_cb(lv_event_t *e) {
    (void)e;
    if (disclaimer_checkbox && lv_obj_has_state(disclaimer_checkbox, LV_STATE_CHECKED)) {
        nvs_set_disclaimer_accepted();
        ESP_LOGI(TAG, "Disclaimer accepted permanently (saved to NVS)");
    } else {
        ESP_LOGI(TAG, "Disclaimer accepted for this session");
    }

    if (disclaimer_overlay) {
        lv_obj_del(disclaimer_overlay);
        disclaimer_overlay = NULL;
        disclaimer_checkbox = NULL;
    }

    // Show welcome overlay on very first acceptance
    if (!nvs_get_welcome_shown()) {
        nvs_set_welcome_shown();
        show_welcome_overlay();
    }
}

void show_disclaimer_overlay(void) {
    if (disclaimer_overlay != NULL) return;

    // Full-screen dark backdrop (no dismiss on tap — must press button)
    disclaimer_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(disclaimer_overlay);
    lv_obj_set_size(disclaimer_overlay, 320, 240);
    lv_obj_set_style_bg_color(disclaimer_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(disclaimer_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(disclaimer_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(disclaimer_overlay);

    // Glass card — blue accent (informational)
    lv_obj_t *card = lv_obj_create(disclaimer_overlay);
    lv_obj_set_size(card, 300, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  // NEVER use shadows — causes device freeze
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Important Notice");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    // Divider
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 270, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 54);

    // Legal text
    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "This device is NOT a medical device.\n"
        "It is for informational purposes only\n"
        "and must NOT be used for medical\n"
        "decisions. Always consult your CGM\n"
        "and healthcare provider.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 280);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 62);

    // "Don't show again" checkbox
    disclaimer_checkbox = lv_checkbox_create(card);
    lv_checkbox_set_text(disclaimer_checkbox, "Don't show again");
    lv_obj_set_style_text_font(disclaimer_checkbox, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(disclaimer_checkbox, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_border_color(disclaimer_checkbox, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(disclaimer_checkbox, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(disclaimer_checkbox, LV_ALIGN_BOTTOM_LEFT, 16, -48);

    // "I Agree" button (ghost-green)
    lv_obj_t *btn_agree = lv_btn_create(card);
    lv_obj_set_size(btn_agree, 268, 36);
    lv_obj_align(btn_agree, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(btn_agree, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_agree, lv_color_hex(0x1a3a2a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_agree, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_agree, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_border_width(btn_agree, 1, 0);
    lv_obj_set_style_radius(btn_agree, 8, 0);
    lv_obj_set_style_shadow_width(btn_agree, 0, 0);
    lv_obj_t *lbl_agree = lv_label_create(btn_agree);
    lv_label_set_text(lbl_agree, LV_SYMBOL_OK "  I Understand & Agree");
    lv_obj_set_style_text_color(lbl_agree, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_center(lbl_agree);
    lv_obj_add_event_cb(btn_agree, disclaimer_agree_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Legal disclaimer overlay shown");
}

// ==================== Welcome Overlay (first-time setup guide) ====================

static lv_obj_t *welcome_overlay = NULL;

static void welcome_close_cb(lv_event_t *e) {
    (void)e;
    if (welcome_overlay) {
        lv_obj_del(welcome_overlay);
        welcome_overlay = NULL;
    }
}

void show_welcome_overlay(void) {
    if (welcome_overlay != NULL) return;

    // Full-screen dark backdrop (dismiss only via button)
    welcome_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(welcome_overlay);
    lv_obj_set_size(welcome_overlay, 320, 240);
    lv_obj_set_style_bg_color(welcome_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(welcome_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(welcome_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(welcome_overlay);

    // Glass card — blue accent (same style as disclaimer)
    lv_obj_t *card = lv_obj_create(welcome_overlay);
    lv_obj_set_size(card, 300, 232);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  // NEVER use shadows — causes device freeze
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Checkmark icon — signals successful setup acknowledgment
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Welcome to CYGM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    // Divider
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 270, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 48);

    // Instructions
    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "Scan the QR code below or visit\n"
        "cygm.me?guide\n"
        "to set up your device.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 280);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 56);

    // QR code — links to setup guide
    static const char *guide_url = "http://cygm.me?guide";
    lv_obj_t *qr = lv_qrcode_create(card, 90,
                                     lv_color_hex(COLOR_TEXT_WHITE),
                                     lv_color_hex(0x0d1520));
    lv_qrcode_update(qr, guide_url, strlen(guide_url));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_border_color(qr, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(qr, 2, 0);

    // "Get Started" button — ghost-green
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, 268, 28);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a3a2a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_OK "  Get Started");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn, welcome_close_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Welcome overlay shown (first-time setup)");
}

// Expand/collapse layout constants
#define NORMAL_LEFT_X       6
#define NORMAL_LEFT_W       150
#define NORMAL_RIGHT_X      164
#define NORMAL_RIGHT_W      150
#define EXPANDED_RIGHT_X    6
#define EXPANDED_RIGHT_W    308
#define CARD_H              220
#define ANIM_DURATION       350

// Glucose freshness arc + heartbeat update callback (1Hz)
static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_freshness_arc == NULL) {
        return;
    }

    // Freshness arc: depletes from 100 to 0 over 300 seconds (5 minutes)
    if (glucose_timestamp > 0) {
        time_t now;
        time(&now);
        int seconds_old = (int)difftime(now, glucose_timestamp);
        int arc_value = 100 - (seconds_old * 100 / 300);
        if (arc_value < 0) arc_value = 0;
        if (arc_value > 100) arc_value = 100;
        lv_arc_set_value(glucose_freshness_arc, arc_value);
    }

    // Fetch-in-progress: shift arc color to accent blue
    if (glucose_fetch_active) {
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    } else if (glucose_data_valid && current_glucose > 0) {
        // Color matches glucose zone
        int glu = current_glucose;
        int low_a = current_alarm_settings.low_alarm.threshold;
        int low_w = current_alarm_settings.low_warning.threshold;
        int high_w = current_alarm_settings.high_warning.threshold;
        int high_a = current_alarm_settings.high_alarm.threshold;
        uint32_t arc_color;
        if (glu <= low_a || glu >= high_a) arc_color = COLOR_RED;
        else if (glu <= low_w || glu >= high_w) arc_color = COLOR_YELLOW;
        else arc_color = COLOR_GREEN;
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);
    }

    // Ambient tint: recompute once per minute (every 60 ticks of 1Hz timer)
    {
        static uint8_t tint_counter = 0;
        tint_counter++;
        if (tint_counter >= 60) {
            tint_counter = 0;
            if (glucose_data_valid && current_glucose > 0) {
                lv_color_t tint = compute_ambient_tint(current_glucose);
                if (left_card != NULL) {
                    lv_obj_set_style_bg_color(left_card, tint, 0);
                }
                if (right_card != NULL) {
                    lv_obj_set_style_bg_color(right_card, tint, 0);
                }
            }
        }
    }

    // Heartbeat: toggle colon visibility every second (cheapest LVGL op — single flag bit)
    if (label_clock_colon != NULL) {
        static bool colon_visible = true;
        colon_visible = !colon_visible;
        if (colon_visible) {
            lv_obj_clear_flag(label_clock_colon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label_clock_colon, LV_OBJ_FLAG_HIDDEN);
        }
        // Expanded view: same colon toggle (only when CGM card is expanded)
        if (expanded_time_colon != NULL && cgm_expanded) {
            if (colon_visible) {
                lv_obj_clear_flag(expanded_time_colon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(expanded_time_colon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // =====================================================================
    // SAFETY WATCHDOG: Independent staleness check on the UI task
    // This runs every second regardless of whether the glucose task is alive.
    // If glucose data is old, force the display to show "---" immediately.
    // This is the LAST LINE OF DEFENSE against showing stale readings.
    // =====================================================================
    if (glucose_data_valid && glucose_timestamp > 0) {
        time_t stale_now;
        time(&stale_now);
        int stale_minutes = (int)difftime(stale_now, glucose_timestamp) / 60;

        if (stale_minutes > 15) {
            // Data is dangerously stale — force display update NOW
            // This catches cases where the glucose task died or got stuck
            update_glucose_display();

            // After 30 minutes, force invalid to show "---" + status
            if (stale_minutes > 30 && glucose_data_valid) {
                ESP_LOGW("WATCHDOG", "SAFETY: Glucose data %d mins old — forcing invalid", stale_minutes);
                glucose_data_valid = false;
                glucose_status = GLUCOSE_STATUS_SIGNAL_LOSS;
                update_glucose_display();
            }
        }
    }

    // Task health check: if glucose task handle exists but task is dead, restart it
    if (glucose_task_handle != NULL && eTaskGetState(glucose_task_handle) == eDeleted) {
        ESP_LOGE("WATCHDOG", "CRITICAL: Glucose task died! Restarting...");
        glucose_task_handle = NULL;
        xTaskCreate(glucose_update_task, "glucose_update", 5120, NULL, 5, &glucose_task_handle);
    }
}

// Draw custom lightbulb icon on canvas
static void draw_lightbulb_icon(lv_obj_t *canvas, uint32_t color) {
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);

    // Warm glow behind bulb
    r.bg_color = lv_color_hex(0xFFDD44);
    r.bg_opa = 30;
    r.border_width = 0;
    r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, 1, 0, 18, 16, &r);

    // Solid bulb body (filled circle)
    r.bg_color = lv_color_hex(0xFFD700);
    r.bg_opa = LV_OPA_COVER;
    r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, 4, 1, 12, 12, &r);

    // Bright inner highlight
    r.bg_color = lv_color_hex(0xFFEE88);
    r.bg_opa = 160;
    lv_canvas_draw_rect(canvas, 7, 3, 6, 6, &r);

    // Base/socket (lighter top, darker bottom)
    r.bg_color = lv_color_hex(0x999999);
    r.bg_opa = LV_OPA_COVER;
    r.radius = 2;
    lv_canvas_draw_rect(canvas, 6, 13, 8, 3, &r);

    r.bg_color = lv_color_hex(0x777777);
    lv_canvas_draw_rect(canvas, 6, 15, 8, 2, &r);

    // Socket detail band (darker)
    r.bg_color = lv_color_hex(0x555555);
    r.bg_opa = LV_OPA_COVER;
    r.radius = 1;
    lv_canvas_draw_rect(canvas, 7, 18, 6, 2, &r);

    // Light rays emanating from bulb
    lv_draw_line_dsc_t ray;
    lv_draw_line_dsc_init(&ray);
    ray.color = lv_color_hex(0xFFCC00);
    ray.width = 1;
    lv_point_t pts[2];

    // Upper-left ray
    pts[0].x = 4; pts[0].y = 2; pts[1].x = 1; pts[1].y = 0;
    lv_canvas_draw_line(canvas, pts, 2, &ray);
    // Upper-right ray
    pts[0].x = 16; pts[0].y = 2; pts[1].x = 19; pts[1].y = 0;
    lv_canvas_draw_line(canvas, pts, 2, &ray);
    // Left ray
    pts[0].x = 3; pts[0].y = 7; pts[1].x = 0; pts[1].y = 7;
    lv_canvas_draw_line(canvas, pts, 2, &ray);
    // Right ray
    pts[0].x = 17; pts[0].y = 7; pts[1].x = 20; pts[1].y = 7;
    lv_canvas_draw_line(canvas, pts, 2, &ray);
}

// Battery tap event: toggle percentage label visibility
static void battery_tap_event_cb(lv_event_t *e) {
    if (battery_percent_label == NULL) return;
    if (lv_obj_has_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// Draw battery icon with smooth fill based on actual percentage (0-100)
void draw_battery_icon(lv_obj_t *canvas, uint32_t color, int percent) {
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);

    // Battery body outline
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_hex(color);
    dsc.border_width = 2;
    dsc.radius = 3;
    lv_canvas_draw_rect(canvas, 0, 1, 24, 12, &dsc);

    // Terminal nub (right side)
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 1;
    lv_canvas_draw_rect(canvas, 24, 4, 4, 6, &dsc);

    // Smooth fill based on percentage (0-20px inner width)
    int fill_width = (percent * 20) / 100;
    if (fill_width < 1 && percent > 0) fill_width = 1;

    if (fill_width > 0) {
        // Gradient fill: darker shade on left, bright color on right
        uint32_t dark_color = battery_percent_to_dark_color(percent);

        dsc.bg_color = lv_color_hex(dark_color);
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_grad.dir = LV_GRAD_DIR_HOR;
        dsc.bg_grad.stops[0].color = lv_color_hex(dark_color);
        dsc.bg_grad.stops[0].frac = 0;
        dsc.bg_grad.stops[1].color = lv_color_hex(color);
        dsc.bg_grad.stops[1].frac = 255;
        dsc.bg_grad.stops_count = 2;
        dsc.border_width = 0;
        dsc.radius = 1;
        lv_canvas_draw_rect(canvas, 2, 3, fill_width, 8, &dsc);
    }
}

// Brightness control functions
static void brightness_icon_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    ESP_LOGI(TAG, "Brightness icon event: %d", code);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Brightness icon clicked - opening overlay");
        create_brightness_overlay();
    }
}

static void brightness_slider_event_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    screen_brightness_percent = (uint8_t)lv_slider_get_value(slider);

    // Update label
    if (brightness_value_label != NULL) {
        lv_label_set_text_fmt(brightness_value_label, "%d%%", screen_brightness_percent);
    }

    // Apply brightness in real-time
    display_set_brightness(screen_brightness_percent);
}

static void brightness_ok_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Brightness OK clicked - saving %d%%", screen_brightness_percent);

        // Save to NVS
        nvs_save_brightness(screen_brightness_percent);
        saved_brightness_percent = screen_brightness_percent;

        // Close overlay
        close_brightness_overlay();
    }
}

static void toast_timer_cb(lv_timer_t *timer) {
    lv_obj_t *toast = (lv_obj_t *)timer->user_data;
    if (toast != NULL) {
        lv_obj_del(toast);
    }
    lv_timer_del(timer);
}

static void show_no_data_toast(void) {
    // Glass-themed toast overlay
    lv_obj_t *toast = lv_obj_create(lv_scr_act());
    lv_obj_set_size(toast, 220, 70);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, 14, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(0x2A3A4A), 0);
    lv_obj_set_style_border_opa(toast, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(toast, 20, 0);
    lv_obj_set_style_shadow_color(toast, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(toast, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(toast, 10, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Icon
    lv_obj_t *icon = lv_label_create(toast);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

    // Message
    lv_obj_t *msg = lv_label_create(toast);
    lv_label_set_text(msg, "No trend data yet\nWaiting for readings...");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(msg, LV_ALIGN_LEFT_MID, 34, 0);

    // Auto-dismiss after 2 seconds
    lv_timer_create(toast_timer_cb, 2000, toast);
}

static void trend_arrow_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Trend arrow clicked - opening glucose history chart");

        int count = glucose_history_get_count();
        if (count == 0) {
            ESP_LOGW(TAG, "No glucose history available yet");
            show_no_data_toast();
            return;
        }

        if (lvgl_port_lock(1)) {
            create_glucose_chart_overlay();
            lvgl_port_unlock();
        }
    }
}

static void create_brightness_overlay(void) {
    if (brightness_overlay != NULL) {
        ESP_LOGI(TAG, "Brightness overlay already open");
        return;  // Already open
    }

    ESP_LOGI(TAG, "Creating brightness overlay");

    // Create semi-transparent overlay
    brightness_overlay = lv_obj_create(screen_home);
    lv_obj_set_size(brightness_overlay, 200, 150);
    lv_obj_center(brightness_overlay);
    lv_obj_set_style_bg_color(brightness_overlay, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(brightness_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(brightness_overlay, 2, 0);
    lv_obj_set_style_border_color(brightness_overlay, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(brightness_overlay, 12, 0);
    lv_obj_clear_flag(brightness_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    lv_obj_t *title = lv_label_create(brightness_overlay);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

    // Value label (percentage)
    brightness_value_label = lv_label_create(brightness_overlay);
    lv_label_set_text_fmt(brightness_value_label, "%d%%", screen_brightness_percent);
    lv_obj_set_style_text_font(brightness_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(brightness_value_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(brightness_value_label, LV_ALIGN_TOP_RIGHT, -10, 10);

    // Brightness slider (1-100%, minimum 1%)
    brightness_slider = lv_slider_create(brightness_overlay);
    lv_obj_set_size(brightness_slider, 170, 12);  // Shrunk horizontally to keep knob inside
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_MID, 0, 50);
    lv_slider_set_range(brightness_slider, 1, 100);  // Minimum 1%, maximum 100%
    lv_slider_set_value(brightness_slider, screen_brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_KNOB);
    lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(brightness_overlay);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);

    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok_btn, brightness_ok_event_cb, LV_EVENT_CLICKED, NULL);
}

static void close_brightness_overlay(void) {
    if (brightness_overlay != NULL) {
        lv_obj_del(brightness_overlay);
        brightness_overlay = NULL;
        brightness_slider = NULL;
        brightness_value_label = NULL;
    }
}

// --- CGM Expand/Collapse ---

static void home_gesture_event_cb(lv_event_t *e) {
    // Ignore gestures if brightness overlay is open
    if (brightness_overlay != NULL) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT && !cgm_expanded) {
        expand_cgm_card();
    } else if (dir == LV_DIR_RIGHT && cgm_expanded) {
        collapse_cgm_card();
    }
}

// Called when left card fade-out animation completes
static void expand_left_fade_done_cb(lv_anim_t *a) {
    lv_obj_add_flag(left_card, LV_OBJ_FLAG_HIDDEN);
}

// Draw a tiny weather condition icon on a 20x20 canvas
static void draw_mini_weather_icon(lv_obj_t *canvas, int weather_code) {
    if (canvas == NULL) return;
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.radius = LV_RADIUS_CIRCLE;

    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.width = 2;
    l.round_start = 1;
    l.round_end = 1;

    switch (weather_code) {
        case 0: // Clear — sun (yellow circle)
            r.bg_color = lv_color_hex(0xFBBF24);
            lv_canvas_draw_rect(canvas, 5, 5, 10, 10, &r);
            break;
        case 1: // Partly cloudy — small sun + cloud
            r.bg_color = lv_color_hex(0xFBBF24);
            lv_canvas_draw_rect(canvas, 2, 3, 8, 8, &r);
            r.bg_color = lv_color_hex(0xCBD5E1);
            lv_canvas_draw_rect(canvas, 7, 8, 11, 8, &r);
            break;
        case 2: // Cloudy — cloud
            r.bg_color = lv_color_hex(0x94A3B8);
            lv_canvas_draw_rect(canvas, 2, 4, 16, 10, &r);
            r.bg_color = lv_color_hex(0xCBD5E1);
            lv_canvas_draw_rect(canvas, 5, 2, 10, 10, &r);
            break;
        case 3: { // Rain — cloud + drops
            r.bg_color = lv_color_hex(0x94A3B8);
            lv_canvas_draw_rect(canvas, 2, 2, 16, 8, &r);
            l.color = lv_color_hex(0x60A5FA);
            lv_point_t d1[] = {{5, 12}, {4, 16}};
            lv_canvas_draw_line(canvas, d1, 2, &l);
            lv_point_t d2[] = {{10, 12}, {9, 16}};
            lv_canvas_draw_line(canvas, d2, 2, &l);
            lv_point_t d3[] = {{15, 12}, {14, 16}};
            lv_canvas_draw_line(canvas, d3, 2, &l);
            break;
        }
        case 4: // Snow — cloud + dots
            r.bg_color = lv_color_hex(0x94A3B8);
            lv_canvas_draw_rect(canvas, 2, 2, 16, 8, &r);
            r.bg_color = lv_color_hex(0xE2E8F0);
            r.radius = LV_RADIUS_CIRCLE;
            lv_canvas_draw_rect(canvas, 4, 13, 3, 3, &r);
            lv_canvas_draw_rect(canvas, 9, 13, 3, 3, &r);
            lv_canvas_draw_rect(canvas, 14, 13, 3, 3, &r);
            break;
        case 5: { // Thunderstorm — cloud + bolt
            r.bg_color = lv_color_hex(0x64748B);
            lv_canvas_draw_rect(canvas, 2, 2, 16, 8, &r);
            l.color = lv_color_hex(0xFBBF24);
            l.width = 2;
            lv_point_t b1[] = {{10, 10}, {8, 14}};
            lv_canvas_draw_line(canvas, b1, 2, &l);
            lv_point_t b2[] = {{8, 14}, {12, 14}};
            lv_canvas_draw_line(canvas, b2, 2, &l);
            lv_point_t b3[] = {{12, 14}, {9, 18}};
            lv_canvas_draw_line(canvas, b3, 2, &l);
            break;
        }
        case 6: { // Fog — horizontal lines
            l.color = lv_color_hex(0x94A3B8);
            l.width = 2;
            lv_point_t f1[] = {{2, 5}, {18, 5}};
            lv_canvas_draw_line(canvas, f1, 2, &l);
            lv_point_t f2[] = {{4, 10}, {16, 10}};
            lv_canvas_draw_line(canvas, f2, 2, &l);
            lv_point_t f3[] = {{2, 15}, {18, 15}};
            lv_canvas_draw_line(canvas, f3, 2, &l);
            break;
        }
        case 7: { // Freezing rain — cloud + blue drops
            r.bg_color = lv_color_hex(0x94A3B8);
            lv_canvas_draw_rect(canvas, 2, 2, 16, 8, &r);
            l.color = lv_color_hex(0x93C5FD);
            lv_point_t d1[] = {{5, 12}, {4, 16}};
            lv_canvas_draw_line(canvas, d1, 2, &l);
            lv_point_t d2[] = {{10, 12}, {9, 16}};
            lv_canvas_draw_line(canvas, d2, 2, &l);
            lv_point_t d3[] = {{15, 12}, {14, 16}};
            lv_canvas_draw_line(canvas, d3, 2, &l);
            break;
        }
        default: // Unknown — small cloud
            r.bg_color = lv_color_hex(0x94A3B8);
            lv_canvas_draw_rect(canvas, 3, 4, 14, 10, &r);
            break;
    }
}

static void expand_cgm_card(void) {
    if (left_card == NULL || right_card == NULL) return;
    cgm_expanded = true;

    ESP_LOGI(TAG, "Expanding CGM card");

    // Sync expanded time labels with current values
    if (expanded_time_h != NULL && label_clock_h != NULL) {
        lv_label_set_text(expanded_time_h, lv_label_get_text(label_clock_h));
    }
    if (expanded_time_m != NULL && label_clock_m != NULL) {
        lv_label_set_text(expanded_time_m, lv_label_get_text(label_clock_m));
    }
    if (expanded_temp_label != NULL && home_temp_label != NULL) {
        lv_label_set_text(expanded_temp_label, lv_label_get_text(home_temp_label));
    }

    // --- Card slide animation ---
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, ANIM_DURATION);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    // Fade out left card
    lv_anim_set_var(&a, left_card);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_ready_cb(&a, expand_left_fade_done_cb);
    lv_anim_start(&a);

    // Animate right card: x 164→6, width 150→308
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_set_var(&a, right_card);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, NORMAL_RIGHT_X, EXPANDED_RIGHT_X);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_width_cb);
    lv_anim_set_values(&a, NORMAL_RIGHT_W, EXPANDED_RIGHT_W);
    lv_anim_start(&a);

    // --- Fade in expanded top bar elements ---
    lv_anim_init(&a);
    lv_anim_set_time(&a, ANIM_DURATION / 2);
    lv_anim_set_delay(&a, ANIM_DURATION / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);

    // Fade in expanded time labels (hour, colon, minute)
    lv_obj_t *exp_time_labels[] = {expanded_time_h, expanded_time_colon, expanded_time_m};
    for (int i = 0; i < 3; i++) {
        if (exp_time_labels[i] != NULL) {
            lv_obj_clear_flag(exp_time_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(exp_time_labels[i], 0, 0);
            lv_anim_set_var(&a, exp_time_labels[i]);
            lv_anim_start(&a);
        }
    }
    if (expanded_temp_label != NULL) {
        lv_obj_clear_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(expanded_temp_label, 0, 0);
        lv_anim_set_var(&a, expanded_temp_label);
        lv_anim_start(&a);
    }
    if (expanded_weather_icon != NULL) {
        draw_mini_weather_icon(expanded_weather_icon, current_weather_code);
        lv_obj_clear_flag(expanded_weather_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(expanded_weather_icon, 0, 0);
        lv_anim_set_var(&a, expanded_weather_icon);
        lv_anim_start(&a);
    }

    // --- Reposition elements for expanded layout ---
    // Expanded card: 308px wide (288px usable), 220px tall (200px usable)
    //
    //  Top:  [🔋100%]    [08:16  72°F ☀]    [💡] [⚙]
    //
    //  Center:          150  [→]
    //                     mg/dL
    //
    //                   5 min ago
    //
    //  Bottom: [================ timer bar ================]

    // Switch to large font for glucose — centered at fixed position
    if (label_glucose != NULL) {
        lv_obj_set_style_text_font(label_glucose, &font_montserrat_120, 0);
        lv_obj_align(label_glucose, LV_ALIGN_CENTER, -40, -8);
    }

    // Resize trend arrow canvas to 80x80 to match large digits
    if (trend_canvas != NULL) {
        lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 80, 80, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
        draw_trend_arrow_sized(trend_canvas, current_trend, 80);
    }

    // Trend arrow: fixed position on right side of card (doesn't shift with digit count)
    if (trend_canvas != NULL) {
        lv_obj_align(trend_canvas, LV_ALIGN_RIGHT_MID, -16, -8);
    }

    // Unit "mg/dL": below glucose, centered under the value
    if (label_unit != NULL && label_glucose != NULL) {
        lv_obj_align_to(label_unit, label_glucose, LV_ALIGN_OUT_BOTTOM_RIGHT, 20, -4);
    }

    // Time ago: centered near bottom
    if (label_time_ago != NULL) {
        lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -22);
    }

    // Freshness arc: scale up for expanded card
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, 200, 200);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 40);
    }

    // Brightness: to the left of gear cog (TOP_RIGHT minus gear width and gap)
    if (brightness_btn_ref != NULL) {
        lv_obj_align(brightness_btn_ref, LV_ALIGN_TOP_RIGHT, -34, 0);
    }
}

static void collapse_cgm_card(void) {
    if (left_card == NULL || right_card == NULL) return;
    cgm_expanded = false;

    ESP_LOGI(TAG, "Collapsing CGM card");

    // Show left card with fade-in
    lv_obj_clear_flag(left_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(left_card, 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, ANIM_DURATION);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    // Fade in left card
    lv_anim_set_var(&a, left_card);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_start(&a);

    // Animate right card back: x 6→164, width 308→150
    lv_anim_set_var(&a, right_card);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, EXPANDED_RIGHT_X, NORMAL_RIGHT_X);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_width_cb);
    lv_anim_set_values(&a, EXPANDED_RIGHT_W, NORMAL_RIGHT_W);
    lv_anim_start(&a);

    // Hide expanded top bar elements
    if (expanded_time_h != NULL) lv_obj_add_flag(expanded_time_h, LV_OBJ_FLAG_HIDDEN);
    if (expanded_time_colon != NULL) lv_obj_add_flag(expanded_time_colon, LV_OBJ_FLAG_HIDDEN);
    if (expanded_time_m != NULL) lv_obj_add_flag(expanded_time_m, LV_OBJ_FLAG_HIDDEN);
    if (expanded_temp_label != NULL) {
        lv_obj_add_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (expanded_weather_icon != NULL) {
        lv_obj_add_flag(expanded_weather_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Restore all elements to normal positions ---
    if (label_glucose != NULL) {
        lv_obj_set_style_text_font(label_glucose, &lv_font_montserrat_48, 0);
        lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 20);
    }
    if (label_unit != NULL) {
        lv_obj_align(label_unit, LV_ALIGN_TOP_MID, 0, 75);
    }
    if (trend_canvas != NULL) {
        lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 60, 60, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
        draw_trend_arrow(trend_canvas, current_trend);
        lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, 30);
    }
    if (label_time_ago != NULL) {
        lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -25);
    }
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, 100, 100);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 14);
    }
    if (brightness_btn_ref != NULL) {
        lv_obj_align(brightness_btn_ref, LV_ALIGN_TOP_MID, 22, 0);
    }
}

void create_home_screen(void) {
    screen_home = lv_obj_create(NULL);
    lv_obj_add_style(screen_home, &style_bg, 0);
    
    // --- LEFT CARD: Time, Weather & WiFi Status ---
    left_card = lv_obj_create(screen_home);
    lv_obj_set_size(left_card, NORMAL_LEFT_W, CARD_H);
    lv_obj_set_pos(left_card, NORMAL_LEFT_X, 10);
    lv_obj_add_style(left_card, &style_card, 0);
    lv_obj_clear_flag(left_card, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi status icon (very top left corner, clickable to open WiFi setup)
    label_wifi_status = lv_label_create(left_card);
    lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(label_wifi_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_wifi_status, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_add_flag(label_wifi_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(label_wifi_status, 12);  // Extend tap target for small touchscreen
    lv_obj_add_event_cb(label_wifi_status, wifi_icon_event_cb, LV_EVENT_CLICKED, NULL);

    // Time (large, white, centered at top, clickable to toggle format)
    // Split into 3 labels (hour, colon, minute) so colon can blink without jitter.
    // Montserrat has PROPORTIONAL digits ("1"=284 vs "0"=512 advance width units).
    // Fixed-width labels + RIGHT/LEFT text align keep digits flush against colon.
    // All use lv_obj_align (parent-relative, persistent) — NOT lv_obj_align_to (one-shot).
    label_clock_colon = lv_label_create(left_card);
    lv_label_set_text(label_clock_colon, ":");
    lv_obj_set_style_text_font(label_clock_colon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_clock_colon, lv_color_hex(COLOR_TEXT_WHITE), 0);

    label_clock_h = lv_label_create(left_card);
    lv_label_set_text(label_clock_h, "00");
    lv_obj_set_style_text_font(label_clock_h, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_clock_h, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_add_flag(label_clock_h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_clock_h, clock_label_event_cb, LV_EVENT_CLICKED, NULL);

    label_clock_m = lv_label_create(left_card);
    lv_label_set_text(label_clock_m, "00");
    lv_obj_set_style_text_font(label_clock_m, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_clock_m, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_add_flag(label_clock_m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_clock_m, clock_label_event_cb, LV_EVENT_CLICKED, NULL);

    // Compute widths from rendered "00" (widest pair) then fix positions
    lv_obj_update_layout(left_card);
    lv_coord_t cw = lv_obj_get_width(label_clock_colon);
    lv_coord_t hw = lv_obj_get_width(label_clock_h);
    lv_obj_set_width(label_clock_h, hw);
    lv_obj_set_style_text_align(label_clock_h, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(label_clock_m, hw);
    lv_obj_set_style_text_align(label_clock_m, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label_clock_colon, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_align(label_clock_h, LV_ALIGN_TOP_MID, -(cw / 2 + hw / 2), 16);
    lv_obj_align(label_clock_m, LV_ALIGN_TOP_MID, (cw / 2 + hw / 2), 16);

    // AM/PM label (small, in top right corner like WiFi icon, 12hr mode only)
    label_ampm = lv_label_create(left_card);
    lv_label_set_text(label_ampm, "AM");
    lv_obj_set_style_text_font(label_ampm, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_ampm, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(label_ampm, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_flag(label_ampm, LV_OBJ_FLAG_HIDDEN);

    // Date (below time, just 2px spacing)
    label_date = lv_label_create(left_card);
    lv_label_set_text(label_date, "Mon, Jan 1");
    lv_obj_set_style_text_font(label_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_date, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(label_date, LV_ALIGN_TOP_MID, 0, 66);  // Time at 16, 48pt font + 2px spacing = 66

    // Weather icon (positioned below date, moved down another 6px)
    static lv_color_t weather_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(60, 50)];
    home_weather_canvas = lv_canvas_create(left_card);
    lv_canvas_set_buffer(home_weather_canvas, weather_buf, 60, 50, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_align(home_weather_canvas, LV_ALIGN_CENTER, 0, 11);  // Was 5, now 11 (+6px)
    lv_obj_set_style_bg_opa(home_weather_canvas, LV_OPA_TRANSP, 0);
    lv_canvas_fill_bg(home_weather_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    draw_weather_icon(home_weather_canvas, current_weather_code);

    // Temperature (below weather icon, moved down 16px total, moved left)
    home_temp_label = lv_label_create(left_card);
    char temp_buf[16];
    if (user_temp_celsius) {
        int temp_c = (current_temp_f - 32) * 5 / 9;
        snprintf(temp_buf, sizeof(temp_buf), "%d C", temp_c);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "%d F", current_temp_f);
    }
    lv_label_set_text(home_temp_label, temp_buf);
    lv_obj_set_style_text_font(home_temp_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(home_temp_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(home_temp_label, LV_ALIGN_CENTER, -35, 50);  // Was (-20, 34), now (-35, 50) = left 15px, down 16px
    // Make temperature label clickable to toggle F/C
    lv_obj_add_flag(home_temp_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(home_temp_label, temp_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    // Hi/Lo temperatures (stacked vertically to the right)
    home_hilo_label = lv_label_create(left_card);
    char hilo_buf[32];
    if (user_temp_celsius) {
        int high_c = (high_temp_f - 32) * 5 / 9;
        int low_c = (low_temp_f - 32) * 5 / 9;
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_c, low_c);
    } else {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_temp_f, low_temp_f);
    }
    lv_label_set_text(home_hilo_label, hilo_buf);
    lv_obj_set_style_text_font(home_hilo_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(home_hilo_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(home_hilo_label, LV_ALIGN_CENTER, 35, 50);  // Was (25, 34), now (35, 50) = stacked vertically

    // Weather condition text (below temperature, moved down 10px)
    home_condition_label = lv_label_create(left_card);
    lv_label_set_text(home_condition_label, get_weather_condition_text(current_weather_code));
    lv_obj_set_style_text_font(home_condition_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(home_condition_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(home_condition_label, LV_ALIGN_CENTER, 0, 72);  // Was (0, 54), now (0, 72) = down 18px (10+8 for temp move)

    // Location (above sunrise/sunset, centered, closer to bottom)
    home_location_label = lv_label_create(left_card);
    lv_label_set_text(home_location_label, strlen(user_location) > 0 ? user_location : "");
    lv_label_set_long_mode(home_location_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(home_location_label, 130);
    lv_obj_set_style_text_font(home_location_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(home_location_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(home_location_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(home_location_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Sunrise time with icon (very bottom left)
    static lv_color_t canvas_buf_sunrise[LV_CANVAS_BUF_SIZE_TRUE_COLOR(12, 12)];
    home_sunrise_canvas = lv_canvas_create(left_card);
    lv_canvas_set_buffer(home_sunrise_canvas, canvas_buf_sunrise, 12, 12, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(home_sunrise_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    draw_sunrise_icon(home_sunrise_canvas);
    lv_obj_align(home_sunrise_canvas, LV_ALIGN_BOTTOM_LEFT, 8, 6);

    home_sunrise_label = lv_label_create(left_card);
    lv_label_set_text(home_sunrise_label, "--:--");
    lv_obj_set_style_text_font(home_sunrise_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(home_sunrise_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(home_sunrise_label, LV_ALIGN_BOTTOM_LEFT, 22, 6);

    // Sunset time with icon (very bottom right)
    static lv_color_t canvas_buf_sunset[LV_CANVAS_BUF_SIZE_TRUE_COLOR(12, 12)];
    home_sunset_canvas = lv_canvas_create(left_card);
    lv_canvas_set_buffer(home_sunset_canvas, canvas_buf_sunset, 12, 12, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(home_sunset_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    draw_sunset_icon(home_sunset_canvas);
    lv_obj_align(home_sunset_canvas, LV_ALIGN_BOTTOM_RIGHT, -34, 6);

    home_sunset_label = lv_label_create(left_card);
    lv_label_set_text(home_sunset_label, "--:--");
    lv_obj_set_style_text_font(home_sunset_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(home_sunset_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(home_sunset_label, LV_ALIGN_BOTTOM_RIGHT, -8, 6);
    
    // --- RIGHT CARD: Glucose ---
    right_card = lv_obj_create(screen_home);
    lv_obj_set_size(right_card, NORMAL_RIGHT_W, CARD_H);
    lv_obj_set_pos(right_card, NORMAL_RIGHT_X, 10);
    lv_obj_add_style(right_card, &style_card, 0);
    lv_obj_clear_flag(right_card, LV_OBJ_FLAG_SCROLLABLE);

    // Battery icon (kept for compatibility - not displayed)
    battery_icon = lv_label_create(right_card);
    lv_label_set_text(battery_icon, "");
    lv_obj_add_flag(battery_icon, LV_OBJ_FLAG_HIDDEN);

    // Battery canvas - tappable to toggle percentage display
    static lv_color_t canvas_buf_battery[LV_CANVAS_BUF_SIZE_TRUE_COLOR(30, 14)];
    battery_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(battery_canvas, canvas_buf_battery, 30, 14, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(battery_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    draw_battery_icon(battery_canvas, COLOR_GREEN, 100);  // Start full
    lv_obj_align(battery_canvas, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_add_flag(battery_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(battery_canvas, 10);
    lv_obj_add_event_cb(battery_canvas, battery_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Battery percentage label (next to icon, visible by default - tap battery to toggle)
    battery_percent_label = lv_label_create(right_card);
    lv_label_set_text(battery_percent_label, "");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(battery_percent_label, battery_canvas, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    // Brightness icon (top center, shifted right) - custom lightbulb in clickable button
    brightness_btn_ref = lv_btn_create(right_card);
    lv_obj_t *brightness_btn = brightness_btn_ref;
    lv_obj_set_size(brightness_btn, 24, 24);
    lv_obj_align(brightness_btn, LV_ALIGN_TOP_MID, 22, 0);  // Shifted right of center
    lv_obj_set_style_bg_opa(brightness_btn, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(brightness_btn, 0, 0);
    lv_obj_set_style_shadow_width(brightness_btn, 0, 0);
    lv_obj_set_style_pad_all(brightness_btn, 0, 0);
    lv_obj_add_event_cb(brightness_btn, brightness_icon_event_cb, LV_EVENT_CLICKED, NULL);

    // Canvas for lightbulb drawing (inside button)
    static lv_color_t brightness_icon_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(20, 24)];
    brightness_icon = lv_canvas_create(brightness_btn);
    lv_canvas_set_buffer(brightness_icon, brightness_icon_buf, 20, 24, LV_IMG_CF_TRUE_COLOR);
    draw_lightbulb_icon(brightness_icon, COLOR_ACCENT_BLUE);
    lv_obj_center(brightness_icon);

    // Glucose value (huge, bright blue with glow effect)
    label_glucose = lv_label_create(right_card);
    lv_label_set_text(label_glucose, "---");
    lv_obj_set_style_text_font(label_glucose, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ACCENT_LIGHT), 0);

    // Make label background transparent for cleaner appearance
    lv_obj_set_style_bg_opa(label_glucose, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label_glucose, 0, 0);  // Remove padding

    lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 20);
    
    // Unit (stored for repositioning in expanded mode)
    label_unit = lv_label_create(right_card);
    lv_label_set_text(label_unit, "mg/dL");
    lv_obj_set_style_text_font(label_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_unit, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label_unit, LV_ALIGN_TOP_MID, 0, 75);
    
    // Trend arrow (canvas for custom drawing, buffer sized for expanded 80x80)
    trend_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 60, 60, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Transparent background
    lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, 30);

    // Make trend arrow clickable to open glucose history chart
    lv_obj_add_flag(trend_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(trend_canvas, trend_arrow_event_cb, LV_EVENT_CLICKED, NULL);

    // Time ago
    label_time_ago = lv_label_create(right_card);
    lv_label_set_text(label_time_ago, "-- min ago");
    lv_obj_set_style_text_font(label_time_ago, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -25);

    // Freshness arc: shows glucose reading age (full=fresh, empty=5min old)
    glucose_freshness_arc = lv_arc_create(right_card);
    lv_obj_set_size(glucose_freshness_arc, 100, 100);
    lv_obj_align(glucose_freshness_arc, LV_ALIGN_BOTTOM_MID, 0, 14);
    lv_arc_set_rotation(glucose_freshness_arc, 200);     // Start from bottom-left
    lv_arc_set_bg_angles(glucose_freshness_arc, 0, 140); // 140 degree sweep
    lv_arc_set_range(glucose_freshness_arc, 0, 100);
    lv_arc_set_value(glucose_freshness_arc, 100);
    lv_obj_remove_style(glucose_freshness_arc, NULL, LV_PART_KNOB);  // No knob
    lv_obj_clear_flag(glucose_freshness_arc, LV_OBJ_FLAG_CLICKABLE); // Not interactive

    // Arc indicator (the filled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);

    // Arc background track (the unfilled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_10, LV_PART_MAIN);
    
    // --- MENU BUTTON (top right) - transparent, just icon ---
    lv_obj_t *menu_btn = lv_btn_create(screen_home);
    lv_obj_set_size(menu_btn, 36, 36);
    lv_obj_align(menu_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_pad_all(menu_btn, 0, 0);

    lv_obj_t *menu_icon = lv_label_create(menu_btn);
    lv_label_set_text(menu_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(menu_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(menu_icon);

    // Attach event handler
    lv_obj_add_event_cb(menu_btn, menu_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Set initial WiFi status icon based on current connection state
    if (wifi_connected) {
        lv_label_set_recolor(label_wifi_status, false);
        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
    } else {
        lv_label_set_recolor(label_wifi_status, true);
        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
    }

    // Create glucose fetch timer (updates every 1 second)
    if (glucose_timer_update == NULL) {
        glucose_timer_update = lv_timer_create(glucose_timer_update_cb, 1000, NULL);
    }

    // Create weather icon animation timer (updates every 100ms for smooth animation)
    if (weather_icon_anim_timer == NULL) {
        weather_icon_anim_timer = lv_timer_create(weather_icon_animation_timer_cb, 100, NULL);
    }

    // --- EXPANDED VIEW TOP BAR (hidden, shown when CGM card is expanded) ---
    // Layout: [battery]    [time  temp ☀]    [💡] [⚙]
    // Time and temp shifted left of center; mini weather icon right of temp
    // Expanded time: split into 3 labels for jitter-free blink.
    // All use lv_obj_align (parent-relative, persistent) so they follow card resize during expand.
    // lv_obj_align_to is one-shot and breaks when the card animates from 150→308px.
    expanded_time_colon = lv_label_create(right_card);
    lv_label_set_text(expanded_time_colon, ":");
    lv_obj_set_style_text_font(expanded_time_colon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_time_colon, lv_color_hex(COLOR_TEXT_WHITE), 0);

    expanded_time_h = lv_label_create(right_card);
    lv_label_set_text(expanded_time_h, "00");
    lv_obj_set_style_text_font(expanded_time_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_time_h, lv_color_hex(COLOR_TEXT_WHITE), 0);

    expanded_time_m = lv_label_create(right_card);
    lv_label_set_text(expanded_time_m, "00");
    lv_obj_set_style_text_font(expanded_time_m, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_time_m, lv_color_hex(COLOR_TEXT_WHITE), 0);

    // Measure widths BEFORE hiding (hidden objects return width 0 in LVGL v8)
    lv_obj_update_layout(right_card);
    lv_coord_t ecw = lv_obj_get_width(expanded_time_colon);
    lv_coord_t ehw = lv_obj_get_width(expanded_time_h);
    lv_obj_set_width(expanded_time_h, ehw);
    lv_obj_set_style_text_align(expanded_time_h, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(expanded_time_m, ehw);
    lv_obj_set_style_text_align(expanded_time_m, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(expanded_time_colon, LV_ALIGN_TOP_MID, -30, 2);
    lv_obj_align(expanded_time_h, LV_ALIGN_TOP_MID, -30 - ecw / 2 - ehw / 2, 2);
    lv_obj_align(expanded_time_m, LV_ALIGN_TOP_MID, -30 + ecw / 2 + ehw / 2, 2);
    // Now hide — expand_cgm_card() will unhide + fade in
    lv_obj_add_flag(expanded_time_colon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(expanded_time_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(expanded_time_m, LV_OBJ_FLAG_HIDDEN);

    expanded_temp_label = lv_label_create(right_card);
    lv_label_set_text(expanded_temp_label, "-- F");
    lv_obj_set_style_text_font(expanded_temp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_temp_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(expanded_temp_label, LV_ALIGN_TOP_MID, 10, 2);
    lv_obj_add_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);

    // Mini weather condition icon (20x20 canvas, right of temp label)
    static lv_color_t mini_weather_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(20, 20)];
    expanded_weather_icon = lv_canvas_create(right_card);
    lv_canvas_set_buffer(expanded_weather_icon, mini_weather_buf, 20, 20, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(expanded_weather_icon, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    lv_obj_align(expanded_weather_icon, LV_ALIGN_TOP_MID, 42, 0);
    lv_obj_add_flag(expanded_weather_icon, LV_OBJ_FLAG_HIDDEN);

    // Register gesture handler on home screen for swipe expand/collapse
    lv_obj_add_event_cb(screen_home, home_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(screen_home, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Heartbeat: colon blink driven by glucose_timer_update_cb (1Hz).
    // Toggles LV_OBJ_FLAG_HIDDEN on label_clock_colon — cheapest LVGL op (single bit).
}
