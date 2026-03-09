/*
 * glucose_chart.c
 *
 * Glucose history trend chart overlay — glass-themed, multi-range
 * Supports 1h, 3h, 6h, 12h, 24h views with left/right cycling
 */

#include "glucose_chart.h"
#include "shared_state.h"
#include "features/glucose_history.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "GLUCOSE_CHART";

// ==================== Time Range Config ====================
typedef struct {
    int minutes;
    const char *label;
    int slot_count;      // Chart data points (time-proportional resolution)
    int label_count;     // Number of time axis labels
    int label_interval;  // Minutes between time labels
} chart_range_t;

static const chart_range_t RANGES[] = {
    {  60, "1h",  13, 5,  15 },   // 5-min slots, labels every 15 min
    { 180, "3h",  37, 7,  30 },   // 5-min slots, labels every 30 min
    { 360, "6h",  73, 7,  60 },   // 5-min slots, labels every 60 min
    { 720, "12h", 73, 7, 120 },   // 10-min slots, labels every 2h
    {1440, "24h", 73, 7, 240 },   // 20-min slots, labels every 4h
};
#define RANGE_COUNT 5
#define DEFAULT_RANGE_INDEX 2  // Start at 6h

// ==================== Chart Colors ====================
#define CHART_BG          0x0D1117  // Deep dark bg
#define CHART_GRID        0x1B2433  // Subtle blue-tinted grid
#define CHART_OVERLAY_BG  0x0F1419  // Glass overlay bg
#define CHART_OVERLAY_BORDER 0x2A3A4A // Subtle border
#define CHART_TARGET_GREEN 0x22C55E  // Target range band
#define CHART_LINE_GREEN  0x34D399  // In-range line (brighter green)
#define CHART_LINE_YELLOW 0xFBBF24  // Warning line
#define CHART_LINE_ORANGE 0xF59E0B  // High warning line
#define CHART_LINE_RED    0xEF4444  // Alarm line

// ==================== Layout Constants ====================
#define OVERLAY_W       310
#define OVERLAY_H       220
#define CHART_X_OFFSET  20   // Left margin for Y labels
#define CHART_W         268  // Chart drawing area width
#define CHART_H         130  // Chart drawing area height
#define CHART_TOP       30   // Top of chart area (below title row)
#define CHART_PAD       2    // Internal chart padding

#define Y_MIN  40
#define Y_MAX  400

// Max points we render (screen is 270px wide — more than this is wasteful)
#define MAX_CHART_POINTS 144

// ==================== Static State ====================
static lv_obj_t *chart_overlay = NULL;
static lv_obj_t *chart = NULL;
static lv_chart_series_t *glucose_series = NULL;
static lv_obj_t *info_popup = NULL;
static lv_obj_t *title_label = NULL;
static lv_obj_t *range_label = NULL;
static lv_obj_t *stats_label = NULL;
static lv_obj_t *time_labels[7] = {0};
static lv_obj_t *y_labels[5] = {0};
static lv_obj_t *no_data_label = NULL;
static int current_range_index = DEFAULT_RANGE_INDEX;

// Filtered data for current view
static glucose_history_point_t filtered_points[MAX_CHART_POINTS];
static int filtered_count = 0;

// ==================== Forward Declarations ====================
static void populate_chart(void);
static void update_time_labels(void);
static void left_btn_cb(lv_event_t *e);
static void right_btn_cb(lv_event_t *e);
static void close_btn_cb(lv_event_t *e);
static void chart_touch_event_cb(lv_event_t *e);
static void chart_draw_event_cb(lv_event_t *e);
static void show_reading_popup(int glucose, int64_t timestamp, lv_coord_t x, lv_coord_t y);
static void hide_reading_popup(void);
static uint32_t get_color_for_glucose(int glucose);

// ==================== Helper: ghost nav button ====================
static lv_obj_t *create_nav_button(lv_obj_t *parent, const char *symbol,
                                    lv_align_t align, lv_coord_t x_ofs,
                                    lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 32, 26);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ==================== Main Create ====================
void create_glucose_chart_overlay(void) {
    if (chart_overlay != NULL) {
        ESP_LOGW(TAG, "Chart overlay already open");
        return;
    }

    int total_count = glucose_history_get_count();
    if (total_count == 0) {
        ESP_LOGW(TAG, "No glucose history available");
        return;
    }

    current_range_index = DEFAULT_RANGE_INDEX;
    ESP_LOGI(TAG, "Creating glucose chart (%d total readings)", total_count);

    // ---- Glass overlay container ----
    chart_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(chart_overlay, OVERLAY_W, OVERLAY_H);
    lv_obj_center(chart_overlay);
    lv_obj_set_style_bg_color(chart_overlay, lv_color_hex(CHART_OVERLAY_BG), 0);
    lv_obj_set_style_bg_opa(chart_overlay, LV_OPA_90, 0);
    lv_obj_set_style_radius(chart_overlay, 16, 0);
    lv_obj_set_style_border_width(chart_overlay, 1, 0);
    lv_obj_set_style_border_color(chart_overlay, lv_color_hex(CHART_OVERLAY_BORDER), 0);
    lv_obj_set_style_border_opa(chart_overlay, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(chart_overlay, 25, 0);
    lv_obj_set_style_shadow_color(chart_overlay, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(chart_overlay, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(chart_overlay, 8, 0);
    lv_obj_clear_flag(chart_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Title row: [<]  "Glucose Trend — 6h"  [>] ----
    lv_obj_t *title_row = lv_obj_create(chart_overlay);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, 294, 26);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    create_nav_button(title_row, LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, 0, left_btn_cb);
    create_nav_button(title_row, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, 0, right_btn_cb);

    // Title text
    title_label = lv_label_create(title_row);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);

    // ---- Chart widget ----
    chart = lv_chart_create(chart_overlay);
    lv_obj_set_size(chart, CHART_W, CHART_H);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 8, CHART_TOP);

    // Chart background
    lv_obj_set_style_bg_color(chart, lv_color_hex(CHART_BG), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_top(chart, 6, 0);
    lv_obj_set_style_pad_bottom(chart, 4, 0);
    lv_obj_set_style_pad_left(chart, 4, 0);
    lv_obj_set_style_pad_right(chart, 4, 0);

    // Grid lines
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(CHART_GRID), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);

    // Y-axis range
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, Y_MIN, Y_MAX);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);

    // Line styling — 2px, no dot markers
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);

    // Add series
    glucose_series = lv_chart_add_series(chart, lv_color_hex(CHART_LINE_GREEN),
                                          LV_CHART_AXIS_PRIMARY_Y);

    // Draw event for target band + per-segment coloring
    lv_obj_add_event_cb(chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    // Touch interaction
    lv_obj_add_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chart, chart_touch_event_cb, LV_EVENT_CLICKED, NULL);

    // ---- Y-axis labels (left of chart) ----
    static const int y_vals[] = {400, 300, 200, 100, 40};
    for (int i = 0; i < 5; i++) {
        y_labels[i] = lv_label_create(chart_overlay);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", y_vals[i]);
        lv_label_set_text(y_labels[i], buf);
        lv_obj_set_style_text_font(y_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(y_labels[i], lv_color_hex(COLOR_TEXT_DIM), 0);

        // Map value to pixel Y within chart area
        lv_coord_t y_frac = CHART_TOP + 6 + (CHART_H - 10) * (Y_MAX - y_vals[i]) / (Y_MAX - Y_MIN);
        lv_obj_align(y_labels[i], LV_ALIGN_TOP_LEFT, 0, y_frac - 5);
    }

    // ---- Time labels (below chart) ----
    for (int i = 0; i < 7; i++) {
        time_labels[i] = lv_label_create(chart_overlay);
        lv_label_set_text(time_labels[i], "");
        lv_obj_set_style_text_font(time_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(time_labels[i], lv_color_hex(COLOR_TEXT_DIM), 0);
    }

    // ---- Range indicator (top-right of chart area, subtle badge) ----
    range_label = lv_label_create(chart_overlay);
    lv_obj_set_style_text_font(range_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(range_label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(range_label, LV_ALIGN_TOP_RIGHT, -6, CHART_TOP + 4);

    // ---- No-data label (hidden by default) ----
    no_data_label = lv_label_create(chart_overlay);
    lv_label_set_text(no_data_label, "Not enough data\nfor this range");
    lv_obj_set_style_text_font(no_data_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(no_data_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(no_data_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(no_data_label, LV_ALIGN_CENTER, 8, -10);
    lv_obj_add_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);

    // ---- Bottom row: stats + close ----
    stats_label = lv_label_create(chart_overlay);
    lv_obj_set_style_text_font(stats_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(stats_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(stats_label, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    // Close button (ghost style, right side)
    lv_obj_t *close_btn = lv_btn_create(chart_overlay);
    lv_obj_set_size(close_btn, 50, 28);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_opa(close_btn, LV_OPA_40, 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);

    // ---- Populate with initial range data ----
    populate_chart();

    ESP_LOGI(TAG, "Glucose chart created (range: %s)", RANGES[current_range_index].label);
}

// ==================== Populate Chart With Current Range ====================
static void populate_chart(void) {
    hide_reading_popup();

    const chart_range_t *range = &RANGES[current_range_index];

    // Update title
    lv_label_set_text(title_label, "Glucose Trend");

    // Update range indicator badge: "· · 6h · ·"
    char range_buf[64];
    char *p = range_buf;
    for (int i = 0; i < RANGE_COUNT; i++) {
        if (i > 0) {
            p += snprintf(p, sizeof(range_buf) - (p - range_buf), "  ");
        }
        if (i == current_range_index) {
            p += snprintf(p, sizeof(range_buf) - (p - range_buf), "%s", RANGES[i].label);
        } else {
            p += snprintf(p, sizeof(range_buf) - (p - range_buf), "\xE2\x80\xA2");  // bullet •
        }
    }
    lv_label_set_text(range_label, range_buf);

    // Fetch raw readings from history
    glucose_history_point_t raw_points[MAX_CHART_POINTS];
    int raw_count = glucose_history_get_range(range->minutes, raw_points, MAX_CHART_POINTS);

    ESP_LOGI(TAG, "Range %s: %d raw readings", range->label, raw_count);

    if (raw_count < 2) {
        lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(stats_label, "");
        for (int i = 0; i < 7; i++) {
            lv_label_set_text(time_labels[i], "");
        }
        filtered_count = 0;
        return;
    }

    lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);

    // Compute stats from raw data (before resampling)
    int min_g = 999, max_g = 0;
    long sum = 0;
    int valid_n = 0;
    for (int i = 0; i < raw_count; i++) {
        if (raw_points[i].valid) {
            int g = raw_points[i].glucose;
            if (g < min_g) min_g = g;
            if (g > max_g) max_g = g;
            sum += g;
            valid_n++;
        }
    }
    if (valid_n > 0) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Lo %d  Avg %d  Hi %d", min_g, (int)(sum / valid_n), max_g);
        lv_label_set_text(stats_label, buf);
    } else {
        lv_label_set_text(stats_label, "No data");
    }

    // Resample to time-proportional slots so chart pixels map linearly to time
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t start_ms = now_ms - (int64_t)range->minutes * 60 * 1000;
    int64_t span_ms = now_ms - start_ms;

    int slot_count = range->slot_count;
    int64_t half_slot_ms = span_ms / (slot_count - 1) / 2;  // tolerance

    filtered_count = slot_count;

    int search_start = 0;  // Sliding pointer (raw data is chronological)
    for (int s = 0; s < slot_count; s++) {
        int64_t slot_time = start_ms + (int64_t)s * span_ms / (slot_count - 1);

        // Find closest raw reading within tolerance
        int best_idx = -1;
        int64_t best_diff = INT64_MAX;
        for (int j = search_start; j < raw_count; j++) {
            if (!raw_points[j].valid) continue;
            int64_t diff = llabs(raw_points[j].timestamp - slot_time);
            if (diff < best_diff && diff <= half_slot_ms) {
                best_diff = diff;
                best_idx = j;
            }
            // Past the tolerance window — stop searching
            if (raw_points[j].timestamp > slot_time + half_slot_ms) break;
        }

        if (best_idx >= 0) {
            filtered_points[s] = raw_points[best_idx];
            if (best_idx > 0) search_start = best_idx - 1;
        } else {
            filtered_points[s].valid = false;
            filtered_points[s].glucose = 0;
            filtered_points[s].timestamp = slot_time;
        }
    }

    ESP_LOGI(TAG, "Resampled to %d time-proportional slots", filtered_count);

    // Set chart data
    lv_chart_set_point_count(chart, filtered_count);
    for (int i = 0; i < filtered_count; i++) {
        if (filtered_points[i].valid) {
            lv_chart_set_next_value(chart, glucose_series, filtered_points[i].glucose);
        } else {
            lv_chart_set_next_value(chart, glucose_series, LV_CHART_POINT_NONE);
        }
    }

    lv_chart_refresh(chart);
    update_time_labels();
}

// ==================== Time Labels Along Bottom ====================
static void update_time_labels(void) {
    if (filtered_count < 2) return;

    const chart_range_t *range = &RANGES[current_range_index];
    int label_count = range->label_count;

    time_t now;
    time(&now);

    // Chart data area within overlay content area:
    // Chart at TOP_MID+8, content_w=294, chart_w=268, pad_left=4, pad_right=4
    // data_left = (294-268)/2 + 8 + 4 = 25, data_right = 25 + 260 = 285
    lv_coord_t data_left = 25;
    lv_coord_t data_span = 260;
    lv_coord_t y_pos = CHART_TOP + CHART_H + 2;

    for (int i = 0; i < 7; i++) {
        if (i >= label_count) {
            lv_label_set_text(time_labels[i], "");
            continue;
        }

        // Time for this label: evenly spaced from oldest (left) to now (right)
        int mins_back = range->minutes - (i * range->label_interval);
        if (mins_back < 0) mins_back = 0;
        time_t label_time = now - (mins_back * 60);

        struct tm ti;
        localtime_r(&label_time, &ti);

        char buf[12];
        if (range->minutes <= 180) {
            strftime(buf, sizeof(buf), "%I:%M", &ti);
            if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
        } else {
            int hour = ti.tm_hour % 12;
            if (hour == 0) hour = 12;
            const char *ampm = ti.tm_hour >= 12 ? "p" : "a";
            snprintf(buf, sizeof(buf), "%d%s", hour, ampm);
        }

        lv_label_set_text(time_labels[i], buf);

        // Position along chart data area
        lv_coord_t x = data_left + (i * data_span) / (label_count - 1);
        lv_obj_align(time_labels[i], LV_ALIGN_TOP_LEFT, x - 8, y_pos);
    }
}

// ==================== Draw Event: Target Band + Line Colors ====================
static void chart_draw_event_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    // --- Draw target range band on first horizontal div line ---
    if (dsc->part == LV_PART_MAIN && dsc->type == LV_CHART_DRAW_PART_DIV_LINE_HOR) {
        if (dsc->id == 0) {
            lv_obj_t *obj = lv_event_get_target(e);
            lv_coord_t pad_t = lv_obj_get_style_pad_top(obj, 0);
            lv_coord_t pad_b = lv_obj_get_style_pad_bottom(obj, 0);
            lv_coord_t pad_l = lv_obj_get_style_pad_left(obj, 0);
            lv_coord_t pad_r = lv_obj_get_style_pad_right(obj, 0);
            lv_coord_t h = lv_obj_get_height(obj) - pad_t - pad_b;
            lv_coord_t w_area = lv_obj_get_width(obj) - pad_l - pad_r;

            // Use alarm thresholds for target band (default 70-180)
            int target_low = current_alarm_settings.low_warning.threshold;
            int target_high = current_alarm_settings.high_warning.threshold;
            if (target_low < Y_MIN) target_low = Y_MIN;
            if (target_high > Y_MAX) target_high = Y_MAX;

            // Convert to pixel coords
            lv_coord_t y_high = obj->coords.y1 + pad_t +
                                h - ((target_high - Y_MIN) * h / (Y_MAX - Y_MIN));
            lv_coord_t y_low  = obj->coords.y1 + pad_t +
                                h - ((target_low - Y_MIN) * h / (Y_MAX - Y_MIN));

            // Draw semi-transparent green band
            lv_draw_rect_dsc_t rect_dsc;
            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.bg_color = lv_color_hex(CHART_TARGET_GREEN);
            rect_dsc.bg_opa = LV_OPA_10;
            rect_dsc.border_width = 0;
            rect_dsc.radius = 0;

            lv_area_t band;
            band.x1 = obj->coords.x1 + pad_l;
            band.x2 = obj->coords.x1 + pad_l + w_area;
            band.y1 = y_high;
            band.y2 = y_low;
            lv_draw_rect(dsc->draw_ctx, &rect_dsc, &band);

            // Draw thin border lines at target edges
            lv_draw_rect_dsc_t line_dsc;
            lv_draw_rect_dsc_init(&line_dsc);
            line_dsc.bg_color = lv_color_hex(CHART_TARGET_GREEN);
            line_dsc.bg_opa = LV_OPA_30;
            line_dsc.border_width = 0;
            line_dsc.radius = 0;

            // Top line (high threshold)
            lv_area_t top_line = { band.x1, y_high, band.x2, y_high + 1 };
            lv_draw_rect(dsc->draw_ctx, &line_dsc, &top_line);

            // Bottom line (low threshold)
            lv_area_t bot_line = { band.x1, y_low - 1, band.x2, y_low };
            lv_draw_rect(dsc->draw_ctx, &line_dsc, &bot_line);
        }
    }

    // --- Per-segment line coloring based on glucose value ---
    if (dsc->part == LV_PART_ITEMS) {
        // Color the line segment based on the Y value
        if (dsc->p1 && dsc->p2) {
            // Use the endpoint glucose value to determine color
            // Map pixel Y back to glucose value
            lv_obj_t *obj = lv_event_get_target(e);
            lv_coord_t pad_t = lv_obj_get_style_pad_top(obj, 0);
            lv_coord_t pad_b = lv_obj_get_style_pad_bottom(obj, 0);
            lv_coord_t h = lv_obj_get_height(obj) - pad_t - pad_b;

            // Average Y of the two points
            lv_coord_t avg_y = (dsc->p1->y + dsc->p2->y) / 2;
            lv_coord_t y_top = obj->coords.y1 + pad_t;
            int glucose_val = Y_MAX - ((avg_y - y_top) * (Y_MAX - Y_MIN) / h);

            uint32_t color = get_color_for_glucose(glucose_val);
            dsc->line_dsc->color = lv_color_hex(color);
        }
    }
}

// ==================== Glucose → Color Mapping ====================
static uint32_t get_color_for_glucose(int glucose) {
    if (glucose >= current_alarm_settings.high_alarm.threshold) {
        return COLOR_RED;
    } else if (glucose >= current_alarm_settings.high_warning.threshold) {
        return CHART_LINE_ORANGE;
    } else if (glucose <= current_alarm_settings.low_alarm.threshold) {
        return COLOR_RED;
    } else if (glucose <= current_alarm_settings.low_warning.threshold) {
        return CHART_LINE_YELLOW;
    }
    return CHART_LINE_GREEN;
}

// ==================== Navigation Callbacks ====================
static void left_btn_cb(lv_event_t *e) {
    (void)e;
    if (current_range_index > 0) {
        current_range_index--;
        populate_chart();
    }
}

static void right_btn_cb(lv_event_t *e) {
    (void)e;
    if (current_range_index < RANGE_COUNT - 1) {
        current_range_index++;
        populate_chart();
    }
}

static void close_btn_cb(lv_event_t *e) {
    (void)e;
    close_glucose_chart_overlay();
}

// ==================== Touch: Tap-to-Inspect ====================
static void chart_touch_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (filtered_count == 0) return;

    lv_point_t touch_point;
    lv_indev_get_point(lv_indev_get_act(), &touch_point);

    // Convert to chart data-area-relative X (accounting for padding)
    lv_coord_t pad_l = lv_obj_get_style_pad_left(chart, 0);
    lv_coord_t pad_r = lv_obj_get_style_pad_right(chart, 0);
    lv_coord_t data_x = touch_point.x - chart->coords.x1 - pad_l;
    lv_coord_t data_width = lv_obj_get_width(chart) - pad_l - pad_r;

    int point_index = (data_x * filtered_count) / data_width;
    if (point_index < 0) point_index = 0;
    if (point_index >= filtered_count) point_index = filtered_count - 1;

    if (filtered_points[point_index].valid) {
        show_reading_popup(filtered_points[point_index].glucose,
                          filtered_points[point_index].timestamp,
                          touch_point.x, touch_point.y);
    }
}

// ==================== Reading Popup (glass tooltip) ====================
static void show_reading_popup(int glucose, int64_t timestamp,
                                lv_coord_t x, lv_coord_t y) {
    hide_reading_popup();

    info_popup = lv_obj_create(chart_overlay);
    lv_obj_set_size(info_popup, 110, 50);

    // Position above touch point with boundary clamping
    lv_coord_t popup_x = x - lv_obj_get_x(chart_overlay) - 55;
    lv_coord_t popup_y = y - lv_obj_get_y(chart_overlay) - 65;
    if (popup_x < 4) popup_x = 4;
    if (popup_x > OVERLAY_W - 118) popup_x = OVERLAY_W - 118;
    if (popup_y < 4) popup_y = CHART_TOP + 4;

    lv_obj_set_pos(info_popup, popup_x, popup_y);

    // Glass tooltip style
    lv_obj_remove_style_all(info_popup);
    lv_obj_set_style_bg_color(info_popup, lv_color_hex(0x1a2332), 0);
    lv_obj_set_style_bg_opa(info_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(info_popup, 10, 0);
    lv_obj_set_style_pad_all(info_popup, 6, 0);
    lv_obj_set_style_shadow_width(info_popup, 12, 0);
    lv_obj_set_style_shadow_color(info_popup, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(info_popup, LV_OPA_60, 0);
    lv_obj_clear_flag(info_popup, LV_OBJ_FLAG_SCROLLABLE);

    // Colored left accent bar
    uint32_t gcolor = get_color_for_glucose(glucose);

    lv_obj_t *accent = lv_obj_create(info_popup);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 3, 36);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(gcolor), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Glucose value
    lv_obj_t *g_label = lv_label_create(info_popup);
    char g_text[20];
    snprintf(g_text, sizeof(g_text), "%d mg/dL", glucose);
    lv_label_set_text(g_label, g_text);
    lv_obj_set_style_text_font(g_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_label, lv_color_hex(gcolor), 0);
    lv_obj_align(g_label, LV_ALIGN_TOP_LEFT, 10, 2);

    // Timestamp
    lv_obj_t *t_label = lv_label_create(info_popup);
    time_t time_sec = (time_t)(timestamp / 1000);
    struct tm ti;
    localtime_r(&time_sec, &ti);
    char t_text[16];
    strftime(t_text, sizeof(t_text), "%I:%M %p", &ti);
    // Remove leading zero
    if (t_text[0] == '0') memmove(t_text, t_text + 1, strlen(t_text));
    lv_label_set_text(t_label, t_text);
    lv_obj_set_style_text_font(t_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(t_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(t_label, LV_ALIGN_BOTTOM_LEFT, 10, -2);
}

static void hide_reading_popup(void) {
    if (info_popup != NULL) {
        lv_obj_del(info_popup);
        info_popup = NULL;
    }
}

// ==================== Close Overlay ====================
void close_glucose_chart_overlay(void) {
    if (chart_overlay != NULL) {
        hide_reading_popup();
        lv_obj_del(chart_overlay);
        chart_overlay = NULL;
        chart = NULL;
        glucose_series = NULL;
        title_label = NULL;
        range_label = NULL;
        stats_label = NULL;
        no_data_label = NULL;
        for (int i = 0; i < 7; i++) time_labels[i] = NULL;
        for (int i = 0; i < 5; i++) y_labels[i] = NULL;
        filtered_count = 0;
        ESP_LOGI(TAG, "Glucose chart overlay closed");
    }
}
