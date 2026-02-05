/*
 * home_screen.c
 *
 * Main home screen implementation
 */

#include "home_screen.h"
#include "shared_state.h"
#include "main.h"
#include "features/weather_system.h"
#include "hardware/display.h"
#include "nvs_config.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "HOME_SCREEN";

// Forward declarations
static void brightness_icon_event_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);
static void brightness_ok_event_cb(lv_event_t *e);
static void create_brightness_overlay(void);
static void close_brightness_overlay(void);

// Brightness control functions
static void brightness_icon_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Brightness icon clicked");
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

static void create_brightness_overlay(void) {
    if (brightness_overlay != NULL) {
        return;  // Already open
    }

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

    // Brightness slider
    brightness_slider = lv_slider_create(brightness_overlay);
    lv_obj_set_size(brightness_slider, 170, 12);  // Shrunk horizontally to keep knob inside
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_MID, 0, 50);
    lv_slider_set_range(brightness_slider, 0, 100);
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

void create_home_screen(void) {
    screen_home = lv_obj_create(NULL);
    lv_obj_add_style(screen_home, &style_bg, 0);
    
    // --- LEFT CARD: Time, Weather & WiFi Status ---
    lv_obj_t *left_card = lv_obj_create(screen_home);
    lv_obj_set_size(left_card, 150, 220);
    lv_obj_set_pos(left_card, 6, 10);
    lv_obj_add_style(left_card, &style_card, 0);
    lv_obj_clear_flag(left_card, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi status icon (very top left corner)
    label_wifi_status = lv_label_create(left_card);
    lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(label_wifi_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_wifi_status, LV_ALIGN_TOP_LEFT, 2, 2);

    // Time (large, white, centered at top, clickable to toggle format)
    label_clock = lv_label_create(left_card);
    lv_label_set_text(label_clock, "00:00");
    lv_obj_set_style_text_font(label_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_clock, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label_clock, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_add_flag(label_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_clock, clock_label_event_cb, LV_EVENT_CLICKED, NULL);

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

    // Sunset time with icon (very bottom right, icon closer to time)
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
    lv_obj_t *right_card = lv_obj_create(screen_home);
    lv_obj_set_size(right_card, 150, 220);
    lv_obj_set_pos(right_card, 164, 10);
    lv_obj_add_style(right_card, &style_card, 0);
    lv_obj_clear_flag(right_card, LV_OBJ_FLAG_SCROLLABLE);

    // Battery icon (top left corner)
    battery_icon = lv_label_create(right_card);
    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(battery_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(battery_icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(battery_icon, LV_ALIGN_TOP_LEFT, 2, 2);

    // Battery percent (to the right of icon, 4px spacing)
    battery_percent_label = lv_label_create(right_card);
    lv_label_set_text(battery_percent_label, "100%");
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(battery_percent_label, LV_ALIGN_TOP_LEFT, 20, 2);  // Icon width ~14px + 4px spacing

    // Brightness icon (top center) - lightbulb symbol
    brightness_icon = lv_label_create(right_card);
    lv_label_set_text(brightness_icon, LV_SYMBOL_BULB);
    lv_obj_set_style_text_font(brightness_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(brightness_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(brightness_icon, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(brightness_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(brightness_icon, brightness_icon_event_cb, LV_EVENT_CLICKED, NULL);

    // Glucose value (huge, bright blue)
    label_glucose = lv_label_create(right_card);
    lv_label_set_text(label_glucose, "---");
    lv_obj_set_style_text_font(label_glucose, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 20);
    
    // Unit
    lv_obj_t *unit = lv_label_create(right_card);
    lv_label_set_text(unit, "mg/dL");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 75);
    
    // Trend arrow
    // Trend arrow (canvas for custom drawing, 60x60 for memory optimization)
    static lv_color_t canvas_buf_trend[LV_CANVAS_BUF_SIZE_TRUE_COLOR(60, 60)];
    trend_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 60, 60, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(trend_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);  // Transparent background
    lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, 30);
    
    // Time ago
    label_time_ago = lv_label_create(right_card);
    lv_label_set_text(label_time_ago, "-- min ago");
    lv_obj_set_style_text_font(label_time_ago, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -10);
    
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
}
