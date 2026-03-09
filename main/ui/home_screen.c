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
#include "nvs_config.h"
#include "ui/glucose_chart.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "HOME_SCREEN";

// File-scope references for expand/collapse repositioning
static lv_obj_t *brightness_btn_ref = NULL;
static lv_obj_t *timer_bar_overlay_ref = NULL;

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

// Expand/collapse layout constants
#define NORMAL_LEFT_X       6
#define NORMAL_LEFT_W       150
#define NORMAL_RIGHT_X      164
#define NORMAL_RIGHT_W      150
#define EXPANDED_RIGHT_X    6
#define EXPANDED_RIGHT_W    308
#define CARD_H              220
#define ANIM_DURATION       350

// Glucose fetch timer update callback (updates progress bar)
static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_fetch_timer_bar == NULL) {
        return;
    }

    // Calculate elapsed time since last fetch (in milliseconds)
    int64_t now = esp_timer_get_time() / 1000;  // Convert microseconds to milliseconds
    int64_t elapsed = now - last_glucose_fetch_time;

    // Glucose task runs every 90 seconds (90000 ms)
    const int64_t FETCH_INTERVAL_MS = 90000;

    // Calculate percentage (0-100%)
    int percentage = (int)((elapsed * 100) / FETCH_INTERVAL_MS);
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    // Update bar
    lv_bar_set_value(glucose_fetch_timer_bar, percentage, LV_ANIM_OFF);

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
        xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
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
        uint32_t dark_color;
        if (color == COLOR_GREEN) dark_color = 0x15803D;
        else if (color == COLOR_RED) dark_color = 0x991B1B;
        else dark_color = 0xB45309;  // dark orange

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

        if (lvgl_port_lock(0)) {
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

static void expand_cgm_card(void) {
    if (left_card == NULL || right_card == NULL) return;
    cgm_expanded = true;

    ESP_LOGI(TAG, "Expanding CGM card");

    // Sync expanded labels with current values
    if (expanded_time_label != NULL && label_clock != NULL) {
        lv_label_set_text(expanded_time_label, lv_label_get_text(label_clock));
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

    // --- Fade in time/temp labels ---
    lv_anim_init(&a);
    lv_anim_set_time(&a, ANIM_DURATION / 2);
    lv_anim_set_delay(&a, ANIM_DURATION / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);

    if (expanded_time_label != NULL) {
        lv_obj_clear_flag(expanded_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(expanded_time_label, 0, 0);
        lv_anim_set_var(&a, expanded_time_label);
        lv_anim_start(&a);
    }
    if (expanded_temp_label != NULL) {
        lv_obj_clear_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(expanded_temp_label, 0, 0);
        lv_anim_set_var(&a, expanded_temp_label);
        lv_anim_start(&a);
    }

    // --- Reposition elements for expanded layout ---
    // Expanded card: 308px wide (288px usable), 220px tall (200px usable)
    //
    //  Top:    [battery]  [12:30  72°F]                  [💡]
    //
    //  Left:   [trend ↑]  120
    //                     mg/dL
    //                     5 min ago
    //
    //  Bottom: [=============== timer bar 75% ===============]

    // Glucose: left side, vertically centered, shifted up for mg/dL + time_ago below
    if (label_glucose != NULL) {
        lv_obj_align(label_glucose, LV_ALIGN_LEFT_MID, 85, -20);
    }

    // Unit "mg/dL": bottom-right of glucose number
    if (label_unit != NULL && label_glucose != NULL) {
        lv_obj_align_to(label_unit, label_glucose, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
    }

    // Trend arrow: left of glucose, vertically centered
    if (trend_canvas != NULL) {
        lv_obj_align(trend_canvas, LV_ALIGN_LEFT_MID, 10, -10);
    }

    // Time ago: below unit label
    if (label_time_ago != NULL && label_glucose != NULL) {
        lv_obj_align_to(label_time_ago, label_glucose, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    }

    // Timer bar: expand to 75% of card width (~216px)
    if (glucose_fetch_timer_bar != NULL) {
        lv_obj_set_width(glucose_fetch_timer_bar, 216);
    }
    if (timer_bar_overlay_ref != NULL) {
        lv_obj_set_width(timer_bar_overlay_ref, 236);
    }

    // Brightness: shift right but stay clear of settings gear (gear at screen TOP_RIGHT, -10)
    if (brightness_btn_ref != NULL) {
        lv_obj_align(brightness_btn_ref, LV_ALIGN_TOP_RIGHT, -40, 0);
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

    // Hide expanded time/temp labels
    if (expanded_time_label != NULL) {
        lv_obj_add_flag(expanded_time_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (expanded_temp_label != NULL) {
        lv_obj_add_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Restore all elements to normal positions ---
    if (label_glucose != NULL) {
        lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 20);
    }
    if (label_unit != NULL) {
        lv_obj_align(label_unit, LV_ALIGN_TOP_MID, 0, 75);
    }
    if (trend_canvas != NULL) {
        lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, 30);
    }
    if (label_time_ago != NULL) {
        lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -25);
    }
    if (glucose_fetch_timer_bar != NULL) {
        lv_obj_set_width(glucose_fetch_timer_bar, 100);
    }
    if (timer_bar_overlay_ref != NULL) {
        lv_obj_set_width(timer_bar_overlay_ref, 120);
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
    lv_obj_add_event_cb(label_wifi_status, wifi_icon_event_cb, LV_EVENT_CLICKED, NULL);

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
    lv_obj_add_event_cb(battery_canvas, battery_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Battery percentage label (next to icon, hidden by default - tap battery to show)
    battery_percent_label = lv_label_create(right_card);
    lv_label_set_text(battery_percent_label, "");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(battery_percent_label, battery_canvas, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);

    // Battery charging icon (lightning bolt, hidden by default)
    battery_charging_icon = lv_label_create(right_card);
    lv_label_set_text(battery_charging_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(battery_charging_icon, lv_color_hex(COLOR_YELLOW), 0);
    lv_obj_set_style_text_font(battery_charging_icon, &lv_font_montserrat_14, 0);
    lv_obj_align_to(battery_charging_icon, battery_percent_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_flag(battery_charging_icon, LV_OBJ_FLAG_HIDDEN);  // Hidden until charging detected

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
    
    // Trend arrow
    // Trend arrow (canvas for custom drawing, 60x60 for memory optimization)
    static lv_color_t canvas_buf_trend[LV_CANVAS_BUF_SIZE_TRUE_COLOR(60, 60)];
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

    // Glucose fetch countdown bar (shows progress to next fetch, tap to force fetch)
    // Card width is 150px with 10px padding = 130px usable, 75% = ~98px
    glucose_fetch_timer_bar = lv_bar_create(right_card);
    lv_obj_set_size(glucose_fetch_timer_bar, 100, 12);  // Increased from 8 to 12 pixels for easier tapping
    lv_obj_align(glucose_fetch_timer_bar, LV_ALIGN_BOTTOM_MID, 0, -5);  // Closer to bottom
    lv_bar_set_range(glucose_fetch_timer_bar, 0, 100);
    lv_bar_set_value(glucose_fetch_timer_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(0x2C2C2E), LV_PART_MAIN);  // Darker background
    lv_obj_set_style_bg_opa(glucose_fetch_timer_bar, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(glucose_fetch_timer_bar, 4, 0);  // More rounded
    lv_obj_set_style_shadow_width(glucose_fetch_timer_bar, 8, LV_PART_INDICATOR);  // Add glow
    lv_obj_set_style_shadow_spread(glucose_fetch_timer_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(glucose_fetch_timer_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(glucose_fetch_timer_bar, LV_OPA_60, LV_PART_INDICATOR);

    // Make timer bar clickable to force immediate glucose fetch
    // Note: Bars might not respond to clicks reliably, so create an invisible button overlay
    lv_obj_add_flag(glucose_fetch_timer_bar, LV_OBJ_FLAG_CLICKABLE);
    extern void glucose_timer_bar_event_cb(lv_event_t *e);
    lv_obj_add_event_cb(glucose_fetch_timer_bar, glucose_timer_bar_event_cb, LV_EVENT_CLICKED, NULL);

    // Create invisible button overlay for more reliable touch detection (30px tall for easier tapping)
    timer_bar_overlay_ref = lv_btn_create(right_card);
    lv_obj_t *timer_bar_touch_overlay = timer_bar_overlay_ref;
    lv_obj_set_size(timer_bar_touch_overlay, 120, 30);  // Larger touch target
    lv_obj_align(timer_bar_touch_overlay, LV_ALIGN_BOTTOM_MID, 0, -2);  // Cover bar area
    lv_obj_set_style_bg_opa(timer_bar_touch_overlay, LV_OPA_TRANSP, 0);  // Completely invisible
    lv_obj_set_style_border_width(timer_bar_touch_overlay, 0, 0);
    lv_obj_set_style_shadow_width(timer_bar_touch_overlay, 0, 0);
    lv_obj_add_event_cb(timer_bar_touch_overlay, glucose_timer_bar_event_cb, LV_EVENT_CLICKED, NULL);
    
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

    // --- EXPANDED VIEW LABELS (hidden, shown in CGM card top bar when expanded) ---
    // These are children of right_card, positioned between battery (left) and brightness (right)
    // Expanded view: time + temp in top bar between battery and brightness
    // In expanded mode (308px card), TOP_MID is at ~144px. Brightness at TOP_MID+22 = 166.
    // Position labels left-of-center to avoid brightness overlap.
    expanded_time_label = lv_label_create(right_card);
    lv_label_set_text(expanded_time_label, "00:00");
    lv_obj_set_style_text_font(expanded_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_time_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(expanded_time_label, LV_ALIGN_TOP_MID, -60, 2);
    lv_obj_add_flag(expanded_time_label, LV_OBJ_FLAG_HIDDEN);

    expanded_temp_label = lv_label_create(right_card);
    lv_label_set_text(expanded_temp_label, "-- F");
    lv_obj_set_style_text_font(expanded_temp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(expanded_temp_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(expanded_temp_label, LV_ALIGN_TOP_MID, 10, 2);
    lv_obj_add_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);

    // Register gesture handler on home screen for swipe expand/collapse
    lv_obj_add_event_cb(screen_home, home_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(screen_home, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
