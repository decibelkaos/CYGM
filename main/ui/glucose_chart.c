/*
 * glucose_chart.c
 *
 * Glucose history chart overlay with LVGL chart widget
 */

#include "glucose_chart.h"
#include "shared_state.h"
#include "features/glucose_history.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "GLUCOSE_CHART";

// UI objects
static lv_obj_t *chart_overlay = NULL;
static lv_obj_t *chart = NULL;
static lv_chart_series_t *glucose_series = NULL;
static lv_obj_t *info_popup = NULL;

// Forward declarations
static void chart_ok_btn_event_cb(lv_event_t *e);
static void chart_touch_event_cb(lv_event_t *e);
static void show_reading_popup(int glucose, int64_t timestamp, lv_coord_t x, lv_coord_t y);
static void hide_reading_popup(void);
static uint32_t get_alarm_color_for_glucose(int glucose);

void create_glucose_chart_overlay(void) {
    if (chart_overlay != NULL) {
        ESP_LOGW(TAG, "Chart overlay already open");
        return;
    }

    int count = glucose_history_get_count();

    if (count == 0) {
        ESP_LOGW(TAG, "No glucose history available");
        return;
    }

    ESP_LOGI(TAG, "Creating glucose chart with %d readings", count);

    // Create overlay
    chart_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(chart_overlay, 310, 220);
    lv_obj_center(chart_overlay);
    lv_obj_add_style(chart_overlay, &style_card, 0);
    lv_obj_set_style_bg_color(chart_overlay, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_radius(chart_overlay, 15, 0);
    lv_obj_set_style_pad_all(chart_overlay, 10, 0);
    lv_obj_clear_flag(chart_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(chart_overlay);
    lv_label_set_text(title, "6-Hour Glucose Trend");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // Create chart
    chart = lv_chart_create(chart_overlay);
    lv_obj_set_size(chart, 270, 120);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 25);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, count);

    // Chart styling
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_pad_all(chart, 5, 0);

    // Grid lines
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN);

    // Set Y-axis range
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 40, 400);

    // Create series with green line color (data points will be colored)
    glucose_series = lv_chart_add_series(chart, lv_color_hex(COLOR_GREEN), LV_CHART_AXIS_PRIMARY_Y);

    // Populate chart with glucose history
    for (int i = 0; i < count; i++) {
        const glucose_history_point_t *point = glucose_history_get_at(i);
        if (point != NULL && point->valid) {
            lv_chart_set_next_value(chart, glucose_series, point->glucose);
        } else {
            lv_chart_set_next_value(chart, glucose_series, LV_CHART_POINT_NONE);
        }
    }

    // Make chart clickable for tap-to-view
    lv_obj_add_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chart, chart_touch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_chart_refresh(chart);

    // Y-axis labels
    lv_obj_t *label_high = lv_label_create(chart_overlay);
    lv_label_set_text(label_high, "400");
    lv_obj_set_style_text_font(label_high, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_high, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_high, LV_ALIGN_TOP_LEFT, 2, 28);

    lv_obj_t *label_low = lv_label_create(chart_overlay);
    lv_label_set_text(label_low, "40");
    lv_obj_set_style_text_font(label_low, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_low, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_low, LV_ALIGN_TOP_LEFT, 5, 135);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(chart_overlay);
    lv_obj_set_size(ok_btn, 80, 35);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Target range label (2 lines) - aligned with button
    lv_obj_t *label_range = lv_label_create(chart_overlay);
    lv_label_set_text(label_range, "Target:\n70-180 mg/dL");
    lv_obj_set_style_text_font(label_range, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_range, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(label_range, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(label_range, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_style(ok_btn, &style_card, 0);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, chart_ok_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Glucose chart created successfully");
}

static uint32_t get_alarm_color_for_glucose(int glucose) {
    // Use alarm threshold settings to determine color
    if (glucose >= current_alarm_settings.high_alarm.threshold) {
        return current_alarm_settings.high_alarm.text_color;  // High alarm
    } else if (glucose >= current_alarm_settings.high_warning.threshold) {
        return current_alarm_settings.high_warning.text_color;  // High warning
    } else if (glucose <= current_alarm_settings.low_alarm.threshold) {
        return current_alarm_settings.low_alarm.text_color;  // Low alarm
    } else if (glucose <= current_alarm_settings.low_warning.threshold) {
        return current_alarm_settings.low_warning.text_color;  // Low warning
    }

    return COLOR_GREEN;  // In range (fallback)
}

void close_glucose_chart_overlay(void) {
    if (chart_overlay != NULL) {
        hide_reading_popup();
        lv_obj_del(chart_overlay);
        chart_overlay = NULL;
        chart = NULL;
        glucose_series = NULL;
        ESP_LOGI(TAG, "Glucose chart overlay closed");
    }
}

static void chart_ok_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        close_glucose_chart_overlay();
    }
}

static void chart_touch_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_point_t touch_point;
        lv_indev_get_point(lv_indev_get_act(), &touch_point);

        // Convert to chart-relative coordinates
        lv_coord_t chart_x = touch_point.x - chart->coords.x1;
        lv_coord_t chart_width = lv_obj_get_width(chart);

        int count = glucose_history_get_count();
        if (count == 0) return;

        // Find nearest data point
        int point_index = (chart_x * count) / chart_width;
        if (point_index < 0) point_index = 0;
        if (point_index >= count) point_index = count - 1;

        const glucose_history_point_t *point = glucose_history_get_at(point_index);
        if (point != NULL && point->valid) {
            ESP_LOGI(TAG, "Touched point %d: %d mg/dL", point_index, point->glucose);
            show_reading_popup(point->glucose, point->timestamp, touch_point.x, touch_point.y);
        }
    }
}

static void show_reading_popup(int glucose, int64_t timestamp, lv_coord_t x, lv_coord_t y) {
    hide_reading_popup();

    // Create popup
    info_popup = lv_obj_create(chart_overlay);
    lv_obj_set_size(info_popup, 120, 55);

    // Position with boundary checking
    lv_coord_t popup_x = x - 60;
    lv_coord_t popup_y = y - 75;
    if (popup_x < 5) popup_x = 5;
    if (popup_x > 190) popup_x = 190;
    if (popup_y < 5) popup_y = 5;

    lv_obj_set_pos(info_popup, popup_x, popup_y);
    lv_obj_add_style(info_popup, &style_card, 0);
    lv_obj_set_style_bg_color(info_popup, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(info_popup, 2, 0);
    lv_obj_set_style_radius(info_popup, 10, 0);
    lv_obj_set_style_pad_all(info_popup, 8, 0);
    lv_obj_set_style_shadow_width(info_popup, 10, 0);
    lv_obj_set_style_shadow_color(info_popup, lv_color_black(), 0);
    lv_obj_clear_flag(info_popup, LV_OBJ_FLAG_SCROLLABLE);

    // Border color matches alarm color
    uint32_t alarm_color = get_alarm_color_for_glucose(glucose);
    lv_obj_set_style_border_color(info_popup, lv_color_hex(alarm_color), 0);

    // Glucose value
    lv_obj_t *glucose_label = lv_label_create(info_popup);
    char glucose_text[20];
    snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", glucose);
    lv_label_set_text(glucose_label, glucose_text);
    lv_obj_set_style_text_font(glucose_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(glucose_label, lv_color_hex(alarm_color), 0);
    lv_obj_align(glucose_label, LV_ALIGN_TOP_MID, 0, 0);

    // Timestamp - display in local time
    lv_obj_t *time_label = lv_label_create(info_popup);
    time_t time_sec = (time_t)(timestamp / 1000);  // Convert ms to seconds
    struct tm timeinfo;
    localtime_r(&time_sec, &timeinfo);  // Convert UTC to local timezone (uses system TZ)

    char time_text[32];
    strftime(time_text, sizeof(time_text), "%I:%M %p", &timeinfo);
    lv_label_set_text(time_label, time_text);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void hide_reading_popup(void) {
    if (info_popup != NULL) {
        lv_obj_del(info_popup);
        info_popup = NULL;
    }
}
