/*
 * home_screen.c - the two home cards (time/weather and glucose), the boot and
 * status overlays, the display-settings sheet, and the night face.
 */

#include "home_screen.h"
#include "shared_state.h"
#include "main.h"
#include "features/weather_system.h"
#include "features/glucose_history.h"
#include "features/time_system.h"
#include "hardware/display.h"
#include "hardware/battery.h"
#include "hardware/screenshot.h"
#include "nvs_config.h"
#include "whats_new.h"
#include "ui/glucose_chart.h"
#include "ui/wifi_screens.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "sd_logger.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HOME_SCREEN";

// Large glucose font (digits only, generated via lv_font_conv)
extern const lv_font_t font_montserrat_120;

// File-scope references for expand/collapse repositioning
static lv_obj_t *brightness_btn_ref = NULL;
static lv_obj_t *expanded_weather_icon = NULL;  // Mini weather icon for expanded top bar
static lv_obj_t *serial_log_indicator = NULL;   // "Log On" indicator below right card
static lv_obj_t *menu_icon_ref = NULL;          // Gear glyph (recoloured by the night face)
static lv_obj_t *label_glucose_delta = NULL;    // Signed change vs the previous reading
static lv_obj_t *label_glucose_age = NULL;      // Age stamp shown once the value goes stale
static lv_color_t canvas_buf_trend[LV_CANVAS_BUF_SIZE_TRUE_COLOR(80, 80)];  // Sized for expanded 80x80

#define HOME_NIGHT_RED          0x8B0000  // Dim red: readable in the dark, few lit pixels
#define HOME_STALE_MIN          12        // Two missed 5-min fetch cycles
#define HOME_STALE_HIDE_MIN     15        // Past this the value itself is replaced with "---"
#define HOME_DELTA_MAX_GAP_MIN  12        // A wider hole than this makes the delta meaningless
#define HOME_NO_COLOR           0xFFFFFFFFu

// 1Hz change-detection state. Every style setter below invalidates its widget,
// so the timer applies one only when the value it would write actually moved.
typedef enum { ARC_LOOK_NONE, ARC_LOOK_FETCH, ARC_LOOK_ZONE } home_arc_look_t;
static home_arc_look_t cache_arc_look = ARC_LOOK_NONE;
static uint32_t cache_arc_zone_color = HOME_NO_COLOR;
static lv_color_t cache_border_color;
static bool cache_border_valid = false;
static bool cache_stale_active = false;

// Night face state. The swap is visibility + colour only: no object is created,
// deleted or reparented, so it costs no heap.
static bool night_face_active = false;
static uint32_t night_prev_hidden_mask = 0;

// ==================== Display Settings Overlay ====================
// Every backlight setting lives behind the bulb icon. Paged rather than
// scrolled: scroll momentum can schedule style transitions, and animations
// freeze this hardware. The root is the shared brightness_overlay global
// because the home gesture handler tests it to suppress expand/collapse.
#define DISP_PAGES          2
#define DISP_IDLE_CLOSE_MS  30000  // matches the sub-screen watchdog in background_tasks.c
#define NIGHT_STEP_MIN      30     // minutes per tap on a time stepper

static lv_obj_t *disp_page_body = NULL;     // holds only the current page's cards
static lv_obj_t *disp_page_header = NULL;   // top-right section caption
static lv_obj_t *disp_page_counter = NULL;  // "n / N"
static lv_timer_t *disp_idle_timer = NULL;  // full-screen overlay must not hide the reading forever
static int disp_page = 0;
static uint8_t disp_brightness_at_open = 0; // slider baseline, so a visit with no drag writes no flash

// Night-schedule widgets. Rebuilt with the page body, so every pointer is
// cleared before the body is cleaned and again when the overlay dies.
static lv_obj_t *night_start_label = NULL;
static lv_obj_t *night_end_label = NULL;
static lv_obj_t *night_level_btns[3] = {NULL, NULL, NULL};
static bool night_cfg_dirty = false;
static bool night_preview_active = false;

// Selectable night levels, dimmest first.
static const uint8_t night_levels[3] = {5, 15, 30};

// Forward declarations
static void brightness_icon_event_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);
static void create_brightness_overlay(void);
static void close_brightness_overlay(void);
static void draw_lightbulb_icon(lv_obj_t *canvas, uint32_t color);
static void battery_tap_event_cb(lv_event_t *e);
static void glucose_timer_update_cb(lv_timer_t *timer);
static void trend_arrow_event_cb(lv_event_t *e);
static void home_gesture_event_cb(lv_event_t *e);
static void expand_cgm_card(void);
static void collapse_cgm_card(void);

// ==================== Idempotent LVGL Setters ====================
// LVGL applies a style or label text unconditionally and invalidates the widget
// as it goes, so a 1Hz timer rewriting identical values repaints the screen
// every second. These wrappers compare first.

static void home_set_label_text(lv_obj_t *label, const char *txt) {
    if (label == NULL || txt == NULL) return;
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, txt) == 0) return;
    lv_label_set_text(label, txt);
}

static void home_set_hidden(lv_obj_t *obj, bool hidden) {
    if (obj == NULL) return;
    if (hidden == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// Compares against the value LVGL currently resolves, not a shadow copy, so it
// stays correct even when another module repaints the same widget.
static void home_set_text_color(lv_obj_t *obj, uint32_t hex) {
    if (obj == NULL) return;
    lv_color_t want = lv_color_hex(hex);
    if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full == want.full) return;
    lv_obj_set_style_text_color(obj, want, 0);
}

static void home_reset_change_caches(void) {
    cache_arc_look = ARC_LOOK_NONE;
    cache_arc_zone_color = HOME_NO_COLOR;
    cache_border_valid = false;
    cache_stale_active = false;
    night_face_active = false;
    night_prev_hidden_mask = 0;
}

// Pressed feedback for the bare clickable labels and canvases used here instead
// of buttons: the shared ghost border would box in the glucose digits and the
// trend arrow, so only its pressed half and the touch-target growth apply.
// ext_cap bounds that growth — the left card packs clickable labels close
// enough that an uncapped touch box would swallow a neighbour's taps.
#define HOME_EXT_UNCAPPED CYGM_HIT_TARGET_MIN
// The trend canvas already clears the minimum target, so this cap exists only
// to keep its touch box off the delta line 3px below it.
#define HOME_TREND_EXT_CAP 2

static void home_apply_tap_feedback(lv_obj_t *obj, lv_coord_t ext_cap) {
    if (obj == NULL) return;

    lv_obj_update_layout(obj);  // ext_click_area is computed from the drawn size
    lv_obj_add_style(obj, &style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, CYGM_BTN_RADIUS, 0);

    lv_coord_t ext = CYGM_BTN_EXT_CLICK;
    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);
    if (w > 0 && w < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - w + 1) / 2;
        if (need > ext) ext = need;
    }
    if (h > 0 && h < CYGM_HIT_TARGET_MIN) {
        lv_coord_t need = (CYGM_HIT_TARGET_MIN - h + 1) / 2;
        if (need > ext) ext = need;
    }
    if (ext > ext_cap) ext = ext_cap;
    lv_obj_set_ext_click_area(obj, ext);
}

// ==================== Zone Border ====================

// Zone border colour, blended across the thresholds:
// red <-> yellow <-> green <-> yellow <-> red.
static lv_color_t compute_zone_border_color(int glucose) {
    int low_a  = current_alarm_settings.low_alarm.threshold;
    int low_w  = current_alarm_settings.low_warning.threshold;
    int high_w = current_alarm_settings.high_warning.threshold;
    int high_a = current_alarm_settings.high_alarm.threshold;

    if (glucose <= low_a) {
        return lv_color_hex(COLOR_RED);
    } else if (glucose <= low_w) {
        // Gradient: red → yellow as glucose rises from low_alarm to low_warning
        int range = low_w - low_a;
        if (range <= 0) return lv_color_hex(COLOR_YELLOW);
        uint8_t mix = (uint8_t)((glucose - low_a) * 255 / range);
        return lv_color_mix(lv_color_hex(COLOR_YELLOW), lv_color_hex(COLOR_RED), mix);
    } else if (glucose < high_w) {
        return lv_color_hex(COLOR_GREEN);
    } else if (glucose < high_a) {
        // Gradient: green → yellow → red as glucose rises from high_warning to high_alarm
        int range = high_a - high_w;
        if (range <= 0) return lv_color_hex(COLOR_RED);
        uint8_t mix = (uint8_t)((glucose - high_w) * 255 / range);
        // First half: green → yellow. Second half: yellow → red.
        if (mix < 128) {
            return lv_color_mix(lv_color_hex(COLOR_YELLOW), lv_color_hex(COLOR_GREEN), mix * 2);
        } else {
            return lv_color_mix(lv_color_hex(COLOR_RED), lv_color_hex(COLOR_YELLOW), (mix - 128) * 2);
        }
    } else {
        return lv_color_hex(COLOR_RED);
    }
}

// Called on a 10s cadence from the 1Hz timer; the cache keeps both card borders
// from being invalidated for a colour that has not moved.
void update_ambient_tint(void) {
    if (!glucose_data_valid || current_glucose <= 0) return;
    if (night_face_active) return;  // borders are dissolved into the black face

    lv_color_t border = compute_zone_border_color(current_glucose);
    if (cache_border_valid && border.full == cache_border_color.full) return;
    cache_border_color = border;
    cache_border_valid = true;

    if (left_card != NULL) {
        lv_obj_set_style_border_color(left_card, border, 0);
    }
    if (right_card != NULL) {
        lv_obj_set_style_border_color(right_card, border, 0);
    }
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

// SAFETY: defaults to "No" after 30s. The device must NEVER stop fetching
// glucose because the user was asleep and did not answer this overlay.
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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  // NEVER use shadows — causes device freeze
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
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
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
    lv_obj_set_size(btn_yes, 110, CYGM_BTN_H);
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 20, -14);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(COLOR_PRESSED_GREEN), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_yes, lv_color_hex(COLOR_GREEN), 0);
    cygm_apply_ghost_btn(btn_yes);
    lv_obj_t *lbl_yes = lv_label_create(btn_yes);
    lv_label_set_text(lbl_yes, "Yes");
    lv_obj_set_style_text_color(lbl_yes, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_center(lbl_yes);
    lv_obj_add_event_cb(btn_yes, nodata_yes_cb, LV_EVENT_CLICKED, NULL);

    // No button (red outline)
    lv_obj_t *btn_no = lv_btn_create(card);
    lv_obj_set_size(btn_no, 110, CYGM_BTN_H);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(COLOR_PRESSED_RED), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_no, lv_color_hex(COLOR_RED), 0);
    cygm_apply_ghost_btn(btn_no);
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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  // NEVER use shadows — causes device freeze
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
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 88);

    // "WiFi Settings" button (ghost-blue, primary action)
    lv_obj_t *btn_settings = lv_btn_create(card);
    lv_obj_set_size(btn_settings, 115, 34);
    lv_obj_align(btn_settings, LV_ALIGN_BOTTOM_LEFT, 15, -12);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(COLOR_PRESSED_BLUE), LV_STATE_PRESSED);
    cygm_apply_ghost_btn(btn_settings);
    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_color(lbl_settings, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl_settings);
    lv_obj_add_event_cb(btn_settings, wifi_disc_settings_cb, LV_EVENT_CLICKED, NULL);

    // "Dismiss" button (ghost-gray, secondary)
    lv_obj_t *btn_dismiss = lv_btn_create(card);
    lv_obj_set_size(btn_dismiss, 100, 34);
    lv_obj_align(btn_dismiss, LV_ALIGN_BOTTOM_RIGHT, -15, -12);
    lv_obj_set_style_border_color(btn_dismiss, lv_color_hex(COLOR_TEXT_DIM), 0);
    cygm_apply_ghost_btn(btn_dismiss);
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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);  // NEVER use shadows — causes device freeze
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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
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
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
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
    lv_obj_set_size(btn_agree, 268, CYGM_BTN_H);
    lv_obj_align(btn_agree, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(btn_agree, lv_color_hex(COLOR_PRESSED_GREEN), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_agree, lv_color_hex(COLOR_GREEN), 0);
    cygm_apply_ghost_btn(btn_agree);
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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
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
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
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
    lv_obj_set_style_border_color(qr, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(qr, 2, 0);

    // "Get Started" button — ghost-green
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, 268, 28);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PRESSED_GREEN), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_update_layout(btn);  // 28px tall — the touch box grows from the drawn size
    cygm_apply_ghost_btn(btn);
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

// Collapsed trend-arrow block. The canvas and its freshness arc share a centre
// so the arc reads as a halo, boxed in by the unit line above and the delta
// below. Only the arc's top semicircle paints, but that crown sits at the top
// of its bounding box, so the diameter decides its clearance from the unit:
//   centre = 98 + HOME_TREND_Y_OFS = 122
//   arc drawn top = 122 - HOME_ARC_SIZE/2 = 84   (3px under the unit ink)
//   canvas bottom = 122 + 30            = 152   (3px over the delta ink)
#define HOME_TREND_Y_OFS    24
#define HOME_ARC_SIZE       76

// ==================== Night Face ====================
// During the scheduled night window the home screen collapses to a clock, the
// glucose value and the trend arrow, dim red on black. Nothing is created or
// destroyed — the same widgets are hidden and recoloured, so the swap costs no
// heap and survives repeated entry and exit.

#define HOME_NIGHT_HIDE_MAX 23

static bool night_prev_expanded = false;  // layout to give back at dawn

// The glow pass bakes the canvas fill colour into its pixels, so the fill must
// match what is behind it: card navy by day, pure black on the night face
// (where the navy reads as a dirty box around the arrow).
static lv_color_t home_trend_bg(void) {
    return lv_color_hex(night_face_active ? 0x000000 : COLOR_CARD_BG);
}

bool home_night_face_is_active(void) {
    return night_face_active;
}

// Zone colour for a glucose value, same thresholds the arc and border use.
static uint32_t home_zone_color_for(int glu) {
    int low_a  = current_alarm_settings.low_alarm.threshold;
    int low_w  = current_alarm_settings.low_warning.threshold;
    int high_w = current_alarm_settings.high_warning.threshold;
    int high_a = current_alarm_settings.high_alarm.threshold;
    if (glu <= low_a || glu >= high_a) return COLOR_RED;
    if (glu <= low_w || glu >= high_w) return COLOR_YELLOW;
    return COLOR_GREEN;
}

// Fills the hide set. Index order is the bit order of night_prev_hidden_mask,
// which is what lets each object return to its own pre-night visibility.
static unsigned home_night_hide_set(lv_obj_t **objs) {
    unsigned n = 0;
    objs[n++] = label_wifi_status;
    objs[n++] = label_ampm;
    objs[n++] = label_date;
    objs[n++] = home_weather_canvas;
    objs[n++] = home_temp_label;
    objs[n++] = home_hilo_label;
    objs[n++] = home_condition_label;
    objs[n++] = home_location_label;
    objs[n++] = home_sunrise_canvas;
    objs[n++] = home_sunrise_label;
    objs[n++] = home_sunset_canvas;
    objs[n++] = home_sunset_label;
    objs[n++] = battery_canvas;
    objs[n++] = battery_percent_label;
    // brightness_btn_ref deliberately NOT here: Night Dim's only off-switch
    // lives behind the bulb, so hiding it would lock the user out of night
    // settings until dawn. It stays visible, dimmed.
    // The expanded top bar's weather bits go dark, but its small clock stays —
    // that clock IS the night face's time display.
    objs[n++] = expanded_temp_label;
    objs[n++] = expanded_weather_icon;
    objs[n++] = label_unit;
    objs[n++] = label_time_ago;
    objs[n++] = glucose_freshness_arc;
    objs[n++] = label_glucose_delta;
    objs[n++] = label_glucose_age;
    objs[n++] = serial_log_indicator;
    return n;
}

static void home_enter_night_face(void) {
    // The night face IS the expanded card: the big glucose value alone, with
    // the time small in the top bar, so a sleepy reader can never mistake the
    // clock for a reading. Expand FIRST — it force-shows the top-bar chrome
    // that the hide loop below then puts back to sleep.
    night_prev_expanded = cgm_expanded;
    if (!cgm_expanded) expand_cgm_card();

    lv_obj_t *objs[HOME_NIGHT_HIDE_MAX];
    unsigned n = home_night_hide_set(objs);
    night_prev_hidden_mask = 0;
    for (unsigned i = 0; i < n; i++) {
        if (objs[i] != NULL && lv_obj_has_flag(objs[i], LV_OBJ_FLAG_HIDDEN)) {
            night_prev_hidden_mask |= (1u << i);
        }
        home_set_hidden(objs[i], true);
    }

    // The card dissolves into the black background the screen style already
    // paints. The left card stays hidden: expand hid it, and the point of this
    // face is that no big clock shows.
    if (right_card != NULL) {
        lv_obj_set_style_bg_opa(right_card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(right_card, LV_OPA_TRANSP, 0);
    }

    // The trend arrow is drawn with fixed colours, so it is dimmed rather than
    // recoloured — redrawing it in red would mean a second copy of the artwork.
    if (trend_canvas != NULL) {
        lv_obj_set_style_img_opa(trend_canvas, LV_OPA_40, 0);
    }
    if (brightness_btn_ref != NULL) {
        lv_obj_set_style_opa(brightness_btn_ref, LV_OPA_40, 0);
    }

    night_face_active = true;
    cache_border_valid = false;
    cache_arc_look = ARC_LOOK_NONE;

    // Repaint on a black canvas — see home_trend_bg().
    if (trend_canvas != NULL) {
        lv_canvas_fill_bg(trend_canvas, lv_color_hex(0x000000), LV_OPA_0);
        draw_trend_arrow_sized(trend_canvas, current_trend, 80);
    }

    ESP_LOGI(TAG, "Night face on");
}

static void home_exit_night_face(void) {
    night_face_active = false;

    lv_obj_t *objs[HOME_NIGHT_HIDE_MAX];
    unsigned n = home_night_hide_set(objs);
    for (unsigned i = 0; i < n; i++) {
        home_set_hidden(objs[i], (night_prev_hidden_mask & (1u << i)) != 0);
    }

    // Give back the layout the user had before night fell.
    if (!night_prev_expanded && cgm_expanded) collapse_cgm_card();

    // Dropping the local override hands the card back to style_card.
    if (right_card != NULL) {
        lv_obj_remove_local_style_prop(right_card, LV_STYLE_BG_OPA, 0);
        lv_obj_remove_local_style_prop(right_card, LV_STYLE_BORDER_OPA, 0);
    }
    if (trend_canvas != NULL) {
        lv_obj_remove_local_style_prop(trend_canvas, LV_STYLE_IMG_OPA, 0);
    }
    if (brightness_btn_ref != NULL) {
        lv_obj_remove_local_style_prop(brightness_btn_ref, LV_STYLE_OPA, 0);
    }

    home_set_text_color(label_clock_h, COLOR_TEXT_WHITE);
    home_set_text_color(label_clock_colon, COLOR_TEXT_WHITE);
    home_set_text_color(label_clock_m, COLOR_TEXT_WHITE);
    home_set_text_color(menu_icon_ref, COLOR_TEXT_WHITE);

    cache_border_valid = false;
    cache_arc_look = ARC_LOOK_NONE;
    cache_stale_active = false;
    update_ambient_tint();
    update_glucose_display();  // repaints value colour, arrow and age from live state

    ESP_LOGI(TAG, "Night face off");
}

// Re-asserted every tick because the time and weather tasks repaint some of
// these widgets on their own schedule. Each setter compares first, so a stable
// night face does no LVGL work.
static void home_night_reassert(void) {
    lv_obj_t *objs[HOME_NIGHT_HIDE_MAX];
    unsigned n = home_night_hide_set(objs);
    for (unsigned i = 0; i < n; i++) {
        home_set_hidden(objs[i], true);
    }

    home_set_text_color(label_clock_h, HOME_NIGHT_RED);
    home_set_text_color(label_clock_colon, HOME_NIGHT_RED);
    home_set_text_color(label_clock_m, HOME_NIGHT_RED);
    home_set_text_color(menu_icon_ref, HOME_NIGHT_RED);
    // The top-bar clock stays its created white — small and dim enough at
    // the night backlight level, and unmistakably not a glucose value.

    // The value keeps its zone colour at night: green/amber/red is the safety
    // read at 3am, and it cannot be confused with the small red clock above.
    home_set_text_color(label_glucose,
                        (glucose_data_valid && current_glucose > 0)
                            ? home_zone_color_for(current_glucose)
                            : HOME_NIGHT_RED);
}

// ==================== Stale Value Language ====================

// Change against the previous ring-buffer reading. Fails closed: too few
// points, an invalid slot or a hole in the series show no delta at all rather
// than a misleading one.
static bool home_delta_from_history(int *delta_out) {
    int count = glucose_history_get_count();
    if (count < 2) return false;

    const glucose_history_point_t *newest = glucose_history_get_at(count - 1);
    const glucose_history_point_t *prev   = glucose_history_get_at(count - 2);
    if (newest == NULL || prev == NULL || !newest->valid || !prev->valid) return false;

    int64_t gap_ms = newest->timestamp - prev->timestamp;
    if (gap_ms <= 0 || gap_ms > (int64_t)HOME_DELTA_MAX_GAP_MIN * 60000) return false;

    *delta_out = newest->glucose - prev->glucose;
    return true;
}

// Grey + strikethrough once the newest reading is two fetch cycles old, which
// lands well before the existing cutoff that replaces the number with "---".
static void home_update_stale_visuals(bool night) {
    if (label_glucose == NULL) return;

    int age_min = -1;
    if (glucose_data_valid && glucose_timestamp > 0) {
        time_t now;
        time(&now);
        age_min = (int)difftime(now, glucose_timestamp) / 60;
    }
    bool stale = (age_min >= HOME_STALE_MIN && age_min <= HOME_STALE_HIDE_MIN);

    // The strikethrough is applied on the night face too: a number nobody can
    // trust must not read as current, whatever the hour.
    lv_text_decor_t want_decor = stale ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE;
    if (lv_obj_get_style_text_decor(label_glucose, LV_PART_MAIN) != want_decor) {
        lv_obj_set_style_text_decor(label_glucose, want_decor, 0);
    }

    if (!night) {
        if (stale) {
            home_set_text_color(label_glucose, COLOR_TEXT_DIM);
        } else if (cache_stale_active) {
            update_glucose_display();  // back in range of trust — restore the zone colour now
        }
    }
    cache_stale_active = stale;

    if (night) return;  // the age stamp and the delta stay off the night face

    if (label_glucose_age != NULL) {
        if (stale) {
            char buf[20];
            snprintf(buf, sizeof(buf), "%d min", age_min);
            home_set_label_text(label_glucose_age, buf);
        }
        home_set_hidden(label_glucose_age, !stale);
    }

    if (label_glucose_delta != NULL) {
        int delta = 0;
        bool show = (!stale && glucose_data_valid && age_min >= 0 &&
                     age_min <= HOME_STALE_HIDE_MIN && home_delta_from_history(&delta));
        if (show) {
            // cygm_format_glucose carries the mmol conversion; the sign is
            // applied to the magnitude so a negative never reaches its printf.
            char mag[12];
            char buf[20];
            cygm_format_glucose(delta < 0 ? -delta : delta, mag, sizeof(mag));
            snprintf(buf, sizeof(buf), "%c%s", delta < 0 ? '-' : '+', mag);
            home_set_label_text(label_glucose_delta, buf);
        }
        home_set_hidden(label_glucose_delta, !show);
    }
}

// Glucose freshness arc + heartbeat update callback (1Hz)
// ==================== Post-Update "What's New" Card ====================
// Shown once per firmware version, on the first calm home tick after an
// update: it yields to the Display sheet, the night face, and any alarm, and
// the version is stamped as seen only when the user taps OK — a reboot before
// that shows the card again. Bullets live in whats_new.h (one line each).

static lv_obj_t *whats_new_overlay = NULL;
static bool whats_new_resolved = false;
static bool whats_new_pending = false;

static void whats_new_delete_cb(lv_event_t *e) {
    (void)e;
    whats_new_overlay = NULL;
}

static void whats_new_ok_cb(lv_event_t *e) {
    (void)e;
    nvs_save_seen_version(CYGM_VERSION_STRING);
    whats_new_pending = false;
    if (whats_new_overlay != NULL) {
        lv_obj_del(whats_new_overlay);  // delete cb NULLs the static
    }
    ESP_LOGI(TAG, "What's New card dismissed for %s", CYGM_VERSION_STRING);
}

static void whats_new_show(void) {
    whats_new_overlay = lv_obj_create(screen_home);
    lv_obj_set_size(whats_new_overlay, 320, 240);
    lv_obj_set_pos(whats_new_overlay, 0, 0);
    lv_obj_set_style_bg_color(whats_new_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(whats_new_overlay, LV_OPA_60, 0);  // scrim; absorbs taps
    lv_obj_set_style_border_width(whats_new_overlay, 0, 0);
    lv_obj_set_style_radius(whats_new_overlay, 0, 0);
    lv_obj_set_style_pad_all(whats_new_overlay, 0, 0);
    lv_obj_clear_flag(whats_new_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(whats_new_overlay, whats_new_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *panel = lv_obj_create(whats_new_overlay);
    lv_obj_set_size(panel, 292, 206);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char title_text[32];
    snprintf(title_text, sizeof(title_text), "Updated to v%d.%d.%d",
             CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *caption = lv_label_create(panel);
    lv_label_set_text(caption, "WHAT'S NEW");
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(caption, 2, 0);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 34);

    for (int i = 0; i < (int)WHATS_NEW_COUNT && i < 5; i++) {
        char line[64];
        snprintf(line, sizeof(line), "\xE2\x80\xA2  %s", whats_new_bullets[i]);
        lv_obj_t *bullet = lv_label_create(panel);
        lv_label_set_long_mode(bullet, LV_LABEL_LONG_CLIP);  // one line, no wrap
        lv_obj_set_width(bullet, 292 - 32);
        lv_label_set_text(bullet, line);
        lv_obj_set_style_text_font(bullet, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(bullet, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(bullet, LV_ALIGN_TOP_LEFT, 16, 54 + i * 20);
    }

    lv_obj_t *ok_btn = lv_btn_create(panel);
    lv_obj_set_size(ok_btn, 120, 36);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_update_layout(ok_btn);
    cygm_apply_ghost_btn(ok_btn);
    lv_obj_add_event_cb(ok_btn, whats_new_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(ok_lbl);

    ESP_LOGI(TAG, "What's New card shown for %s", CYGM_VERSION_STRING);
}

static void whats_new_maybe_show(void) {
    if (whats_new_overlay != NULL) return;

    if (!whats_new_resolved) {
        whats_new_resolved = true;
        char seen[48];
        esp_err_t err = nvs_load_seen_version(seen, sizeof(seen));
        if (err != ESP_OK || seen[0] == '\0') {
            // No stamp yet. WiFi credentials mean a device that UPDATED from
            // firmware older than the stamp itself — that user gets the card.
            // A true fresh install is stamped silently.
            whats_new_pending = nvs_has_wifi_credentials();
        } else {
            whats_new_pending = (strcmp(seen, CYGM_VERSION_STRING) != 0);
        }
        if (!whats_new_pending) {
            nvs_save_seen_version(CYGM_VERSION_STRING);
        }
    }

    if (!whats_new_pending || visual_alarm_active) return;
    whats_new_show();
}

static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_freshness_arc == NULL) {
        return;
    }

    // The Display sheet owns the screen and the backlight while open: it covers
    // everything this tick would paint, and a night-face swap underneath would
    // re-dim the backlight out from under the settings being edited.
    if (brightness_overlay != NULL) {
        return;
    }

    // Night face: polled here, swapped only on the edges.
    bool night = cygm_night_active();
    if (night != night_face_active) {
        if (night) {
            home_enter_night_face();
        } else {
            home_exit_night_face();
        }
    }

    if (night_face_active) {
        home_night_reassert();
    } else {
        whats_new_maybe_show();  // one-shot; yields to sheet/night/alarm above

        // Countdown to the next CGM pull. The glucose task arms a deadline on
        // every wait, so the arc drains across exactly one poll interval.
        // Empty with no deadline armed means a pull is due or the link is down.
        {
            int64_t remain_ms = glucose_next_fetch_ms - esp_timer_get_time() / 1000;
            int period_s = glucose_fetch_period_s;
            if (period_s < 1) period_s = 1;
            int arc_value = 0;
            if (remain_ms > 0) {
                arc_value = (int)(remain_ms / (period_s * 10));  // = ms * 100 / (s * 1000)
                if (arc_value > 100) arc_value = 100;
            }
            lv_arc_set_value(glucose_freshness_arc, arc_value);  // LVGL skips an unchanged value
        }

        // Fetch-in-progress: shift arc color to accent blue
        if (glucose_fetch_active) {
            if (cache_arc_look != ARC_LOOK_FETCH) {
                lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
                lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_COVER, LV_PART_INDICATOR);
                cache_arc_look = ARC_LOOK_FETCH;
            }
        } else if (glucose_data_valid && current_glucose > 0) {
            uint32_t arc_color = home_zone_color_for(current_glucose);
            if (cache_arc_look != ARC_LOOK_ZONE || cache_arc_zone_color != arc_color) {
                lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_INDICATOR);
                lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(arc_color), LV_PART_MAIN);
                lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);
                cache_arc_look = ARC_LOOK_ZONE;
                cache_arc_zone_color = arc_color;
            }
        }

        // Zone border: recompute every 10 seconds
        {
            static uint8_t tint_counter = 0;
            tint_counter++;
            if (tint_counter >= 10) {
                tint_counter = 0;
                update_ambient_tint();
            }
        }
    }

    // Stale-value language: greys and strikes the number through before the
    // existing cutoff hides it, and carries the reading-to-reading change.
    home_update_stale_visuals(night_face_active);

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

    // SAFETY WATCHDOG: staleness check on the UI task, independent of whether
    // the glucose task is alive. Last line of defence against a stale reading
    // being displayed as current.
    if (glucose_data_valid && glucose_timestamp > 0) {
        time_t stale_now;
        time(&stale_now);
        int stale_minutes = (int)difftime(stale_now, glucose_timestamp) / 60;

        if (stale_minutes > 15) {
            // Catches a glucose task that died or hung.
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
        ensure_tasks_running();  // Canonical stack/prio/core — never recreate ad hoc
    }

    // Serial log indicator: show/hide based on current capture state.
    // The night face owns this widget's visibility while it is up.
    if (!night_face_active) {
        home_set_hidden(serial_log_indicator, !sd_serial_capture_get());
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
    if (battery_percent_label == NULL || !battery_present) return;
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

// Mains plug with a cable tail, drawn in place of the gauge when no cell is fitted.
void draw_plug_icon(lv_obj_t *canvas, uint32_t color) {
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius = 0;

    // Two prongs pointing left
    lv_canvas_draw_rect(canvas, 0, 3, 5, 2, &dsc);
    lv_canvas_draw_rect(canvas, 0, 9, 5, 2, &dsc);

    // Neck between body and cable
    lv_canvas_draw_rect(canvas, 16, 5, 4, 4, &dsc);

    // Plug body, outlined like the battery gauge
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_hex(color);
    dsc.border_width = 2;
    dsc.radius = 3;
    lv_canvas_draw_rect(canvas, 5, 0, 11, 14, &dsc);

    // Cable tail curving down and away
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(color);
    line.width = 2;
    line.round_start = 1;
    line.round_end = 1;
    lv_point_t tail[] = { {20, 7}, {24, 7}, {27, 9}, {29, 12} };
    lv_canvas_draw_line(canvas, tail, 4, &line);
}

// A swipe starting on a clickable child still ends in LV_EVENT_CLICKED on that
// child: LVGL suppresses the click for scrolling, not for gestures. gesture_dir
// survives until the next press, so it still names the gesture just released.
static bool home_press_was_gesture(void) {
    lv_indev_t *indev = lv_indev_get_act();
    return indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

// Brightness control functions
static void brightness_icon_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (home_press_was_gesture()) return;
        ESP_LOGI(TAG, "Brightness icon clicked - opening display settings");
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

// The slider live-applies but commits only on the way out, so a drag costs one
// flash write per visit. The baseline is captured at open, not read from
// saved_brightness_percent — an alarm rewrites that mid-visit as its restore
// target.
static void brightness_commit(void) {
    if (screen_brightness_percent == disp_brightness_at_open) return;
    ESP_LOGI(TAG, "Saving brightness %d%%", screen_brightness_percent);
    nvs_save_brightness(screen_brightness_percent);
    saved_brightness_percent = screen_brightness_percent;
    disp_brightness_at_open = screen_brightness_percent;
}

static void brightness_dim_charge_sw_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    dim_while_charging = lv_obj_has_state(sw, LV_STATE_CHECKED);
    nvs_save_dim_while_charging(dim_while_charging);
    ESP_LOGI(TAG, "Dim while charging: %s", dim_while_charging ? "on" : "off");
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
    lv_obj_set_style_bg_color(toast, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, 14, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_opa(toast, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(toast, 0, 0);  // NEVER use shadows — causes device freeze
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
        // A swipe starting on the arrow already expanded the card; opening the
        // chart on the same release would stack two actions on one gesture.
        if (home_press_was_gesture()) return;

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

// ---- Shared card looks ----
// Each lv_obj_set_style_* call allocates a per-object style from the 36KB LVGL
// pool. These looks repeat across both pages, so they are shared file statics —
// which must outlive every object using them, as LVGL keeps the pointer.
static lv_style_t ds_card;      // settings card shell
static lv_style_t ds_accent;    // 3px accent bar (colour stays per-object)
static lv_style_t ds_rule;      // 1px hairline divider
static lv_style_t ds_section;   // 12pt dim all-caps section header
static lv_style_t ds_value;     // 14pt white value text
static lv_style_t ds_time;      // 16pt accent-light centred time value
static bool ds_styles_ready = false;

static void disp_init_styles(void) {
    if (ds_styles_ready) return;
    ds_styles_ready = true;

    lv_style_init(&ds_card);
    lv_style_set_bg_color(&ds_card, lv_color_hex(COLOR_CARD_BG));
    lv_style_set_bg_opa(&ds_card, LV_OPA_COVER);
    lv_style_set_radius(&ds_card, 12);
    lv_style_set_border_width(&ds_card, 1);
    lv_style_set_border_color(&ds_card, lv_color_hex(COLOR_MODAL_BORDER));
    lv_style_set_border_opa(&ds_card, LV_OPA_30);
    lv_style_set_shadow_width(&ds_card, 0);
    lv_style_set_pad_all(&ds_card, 0);

    lv_style_init(&ds_accent);
    lv_style_set_bg_opa(&ds_accent, LV_OPA_COVER);
    lv_style_set_radius(&ds_accent, 2);

    lv_style_init(&ds_rule);
    lv_style_set_bg_color(&ds_rule, lv_color_hex(COLOR_DIVIDER));
    lv_style_set_bg_opa(&ds_rule, LV_OPA_COVER);

    lv_style_init(&ds_section);
    lv_style_set_text_font(&ds_section, &lv_font_montserrat_12);
    lv_style_set_text_color(&ds_section, lv_color_hex(COLOR_TEXT_DIM));
    lv_style_set_text_letter_space(&ds_section, 2);

    lv_style_init(&ds_value);
    lv_style_set_text_font(&ds_value, &lv_font_montserrat_14);
    lv_style_set_text_color(&ds_value, lv_color_hex(COLOR_TEXT_WHITE));

    lv_style_init(&ds_time);
    lv_style_set_text_font(&ds_time, &lv_font_montserrat_16);
    lv_style_set_text_color(&ds_time, lv_color_hex(COLOR_ACCENT_LIGHT));
    lv_style_set_text_align(&ds_time, LV_TEXT_ALIGN_CENTER);
}

// ---- Widget helpers ----

// A control whose label IS the object — one pool object instead of a button
// plus a child label. A label draws its first line at the top of its content
// box, so vertical centring is pad_top, less the row the ghost border eats.
static lv_obj_t *disp_chip(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                           uint32_t color, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *chip = lv_label_create(parent);
    lv_label_set_long_mode(chip, LV_LABEL_LONG_CLIP);
    lv_label_set_text(chip, txt);
    lv_obj_set_size(chip, w, h);
    lv_obj_set_style_text_font(chip, font, 0);
    lv_obj_set_style_text_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(chip, LV_TEXT_ALIGN_CENTER, 0);
    lv_coord_t pad = (h - lv_font_get_line_height(font)) / 2 - CYGM_BTN_BORDER_W;
    lv_obj_set_style_pad_top(chip, pad > 0 ? pad : 0, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    cygm_apply_ghost_btn(chip);
    return chip;
}

// The stock theme's switch styles carry a state transition, and animations
// freeze this hardware, so they are stripped before the look is rebuilt.
static lv_obj_t *disp_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_PRESSED), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_PRESSED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, 12);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *disp_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 4, 4);
    cygm_apply_ghost_btn(btn);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ---- Night schedule ----
// features/time_system.c owns night_cfg and the fades; this page only edits
// the struct in place and writes the blob back.

static void night_cfg_flush(void) {
    if (!night_cfg_dirty) return;
    night_cfg_dirty = false;
    esp_err_t err = nvs_save_night_cfg(&night_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save night schedule: %s", esp_err_to_name(err));
    }
}

// Undo the level preview. The daytime target is the live slider value, not the
// engine's cached day level, which the user may have just moved past.
static void night_preview_end(void) {
    if (!night_preview_active) return;
    night_preview_active = false;
    if (visual_alarm_active) return;  // an active alarm owns the backlight
    display_set_brightness(cygm_night_active() ? night_cfg.night_brightness
                                               : screen_brightness_percent);
}

static void night_refresh_time_labels(void) {
    char buf[12];
    if (night_start_label != NULL) {
        cygm_format_clock(buf, sizeof(buf), night_cfg.start_hour, night_cfg.start_min);
        lv_label_set_text(night_start_label, buf);
    }
    if (night_end_label != NULL) {
        cygm_format_clock(buf, sizeof(buf), night_cfg.end_hour, night_cfg.end_min);
        lv_label_set_text(night_end_label, buf);
    }
}

static void night_refresh_level_btns(void) {
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = night_level_btns[i];
        if (btn == NULL) continue;
        bool active = (night_cfg.night_brightness == night_levels[i]);

        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(btn, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        // The chip IS the label — there is no child to recolour.
        lv_obj_set_style_text_color(btn,
            lv_color_hex(active ? COLOR_TEXT_WHITE : COLOR_TEXT_DIM), 0);
    }
}

// code: +1/-1 = start time later/earlier, +2/-2 = end time later/earlier
static void night_time_step_cb(lv_event_t *e) {
    lv_event_code_t ev = lv_event_get_code(e);
    if (ev != LV_EVENT_CLICKED && ev != LV_EVENT_LONG_PRESSED_REPEAT) return;

    int code = (int)(intptr_t)lv_event_get_user_data(e);
    bool is_start = (code == 1 || code == -1);
    int delta = (code < 0) ? -NIGHT_STEP_MIN : NIGHT_STEP_MIN;

    uint8_t *hour = is_start ? &night_cfg.start_hour : &night_cfg.end_hour;
    uint8_t *min  = is_start ? &night_cfg.start_min  : &night_cfg.end_min;

    int total = ((*hour % 24) * 60 + (*min % 60) + delta + 24 * 60) % (24 * 60);
    *hour = (uint8_t)(total / 60);
    *min  = (uint8_t)(total % 60);

    night_cfg_dirty = true;
    night_refresh_time_labels();

    // Hold-to-repeat only marks dirty; one flash write per gesture, not per tick.
    if (ev == LV_EVENT_CLICKED) night_cfg_flush();
}

static void night_enable_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    night_cfg.enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    night_cfg_dirty = true;
    night_cfg_flush();
    ESP_LOGI(TAG, "Night dim %s", night_cfg.enabled ? "enabled" : "disabled");
}

static void night_level_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= 3) return;

    night_cfg.night_brightness = night_levels[idx];
    night_cfg_dirty = true;
    night_cfg_flush();
    night_refresh_level_btns();

    // Preview the level rather than describe it. An active alarm owns the
    // backlight at 100%, so never fight it.
    if (!visual_alarm_active) {
        night_preview_active = true;
        display_set_brightness(night_cfg.night_brightness);
    }
    ESP_LOGI(TAG, "Night brightness set to %d%%", night_cfg.night_brightness);
}

// One time stepper: [-] HH:MM [+]. Returns the value label.
static lv_obj_t *night_add_time_row(lv_obj_t *card, const char *name, int y, int code_base) {
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_add_style(name_lbl, &ds_value, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 16, y + 6);

    lv_obj_t *minus = disp_chip(card, LV_SYMBOL_MINUS, &lv_font_montserrat_14,
                                COLOR_ACCENT_LIGHT, 30, 28);
    lv_obj_align(minus, LV_ALIGN_TOP_RIGHT, -110, y);
    // Steppers sit two rows deep; the default touch-box growth would let the
    // Start row swallow taps meant for End.
    lv_obj_set_ext_click_area(minus, 6);
    lv_obj_add_event_cb(minus, night_time_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(-code_base));
    lv_obj_add_event_cb(minus, night_time_step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)(-code_base));

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "--:--");
    lv_obj_add_style(value, &ds_time, 0);
    lv_obj_set_width(value, 60);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -50, y + 5);

    lv_obj_t *plus = disp_chip(card, LV_SYMBOL_PLUS, &lv_font_montserrat_14,
                               COLOR_ACCENT_LIGHT, 30, 28);
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, -12, y);
    lv_obj_set_ext_click_area(plus, 6);
    lv_obj_add_event_cb(plus, night_time_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)code_base);
    lv_obj_add_event_cb(plus, night_time_step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)code_base);

    return value;
}

// ---- Page cards ----

// Page 1, top card. Rows within the 294x90 content box:
//   accent bar        6..30   | BRIGHTNESS cap 8..23 | percent value 4..26
//   divider          38..39
//   slider track     56..70   (knob ink spans 53..73)
static void create_brightness_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 92);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_style(card, &ds_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_add_style(accent, &ds_accent, 0);
    lv_obj_set_size(accent, 3, 24);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "BRIGHTNESS");
    lv_obj_add_style(title, &ds_section, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 8);

    brightness_value_label = lv_label_create(card);
    lv_label_set_text_fmt(brightness_value_label, "%d%%", screen_brightness_percent);
    lv_obj_set_style_text_font(brightness_value_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(brightness_value_label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(brightness_value_label, LV_ALIGN_TOP_RIGHT, -14, 4);

    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_add_style(divider, &ds_rule, 0);
    lv_obj_set_size(divider, 264, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 38);

    brightness_slider = lv_slider_create(card);
    lv_obj_set_size(brightness_slider, 244, 14);
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_MID, 0, 56);
    lv_slider_set_range(brightness_slider, 1, 100);  // 0% would black the panel out
    lv_slider_set_value(brightness_slider, screen_brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(brightness_slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    // Pinned rather than inherited: the stock knob padding is theme-dependent
    // and the card only has 10 rows of slack under the track.
    lv_obj_set_style_pad_all(brightness_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(brightness_slider, 0, LV_PART_KNOB);
    // A 14px track is well under the minimum touch target; nothing clickable
    // sits within 12px of it on this card.
    lv_obj_set_ext_click_area(brightness_slider, 12);
    lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

// Page 1, bottom card. Content box 294x46: one row, label left, switch right.
static void create_dim_charge_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 48);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_add_style(card, &ds_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_add_style(accent, &ds_accent, 0);
    lv_obj_set_size(accent, 3, 24);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_GREEN), 0);

    // Dim-on-charger opt-in: by default a plugged-in device never idle-dims.
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, "Dim when plugged in");
    lv_obj_add_style(label, &ds_value, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *sw = disp_switch(card, dim_while_charging, brightness_dim_charge_sw_cb);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
}

// Page 2, sole card. Rows within the 294x164 content box:
//   accent bar        6..30   | NIGHT DIM caption 8..23 | switch 4..26
//   divider          32..33
//   Start row        40..68     End row  82..110
//   NIGHT LEVEL cap 118..133    chips   134..160
static void create_night_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 166);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_style(card, &ds_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_add_style(accent, &ds_accent, 0);
    lv_obj_set_size(accent, 3, 24);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_LIGHT), 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "NIGHT DIM");
    lv_obj_add_style(title, &ds_section, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 8);

    lv_obj_t *sw = disp_switch(card, night_cfg.enabled != 0, night_enable_cb);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -12, 4);

    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_add_style(divider, &ds_rule, 0);
    lv_obj_set_size(divider, 264, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 32);

    night_start_label = night_add_time_row(card, "Start", 40, 1);
    night_end_label   = night_add_time_row(card, "End",   82, 2);
    night_refresh_time_labels();

    lv_obj_t *level_hdr = lv_label_create(card);
    lv_label_set_text(level_hdr, "NIGHT LEVEL");
    lv_obj_add_style(level_hdr, &ds_section, 0);
    lv_obj_align(level_hdr, LV_ALIGN_TOP_LEFT, 16, 118);

    const int lvl_w = 82;
    const int lvl_start_x = (296 - (lvl_w * 3 + 8 * 2)) / 2;
    for (int i = 0; i < 3; i++) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)night_levels[i]);
        lv_obj_t *btn = disp_chip(card, pct, &lv_font_montserrat_14,
                                  COLOR_TEXT_DIM, lvl_w, 26);
        lv_obj_set_pos(btn, lvl_start_x + i * (lvl_w + 8), 134);
        // Three abutting options: keep the touch boxes from overlapping.
        lv_obj_set_ext_click_area(btn, 4);

        night_level_btns[i] = btn;
        lv_obj_add_event_cb(btn, night_level_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    night_refresh_level_btns();
}

// ---- Pager ----

// Cleared before every lv_obj_clean() of the page body so a stale callback can
// never write into freed pool memory.
static void disp_clear_refs(void) {
    brightness_slider = NULL;
    brightness_value_label = NULL;
    night_start_label = NULL;
    night_end_label = NULL;
    night_level_btns[0] = NULL;
    night_level_btns[1] = NULL;
    night_level_btns[2] = NULL;
}

static const char *disp_page_title(int page) {
    return (page == 0) ? "BRIGHTNESS" : "NIGHT DIM";
}

static void disp_build_page(void) {
    if (disp_page_body == NULL) return;

    disp_clear_refs();
    lv_obj_clean(disp_page_body);

    if (disp_page == 0) {
        create_brightness_card(disp_page_body);
        create_dim_charge_card(disp_page_body);
    } else {
        create_night_card(disp_page_body);
    }
}

static void disp_goto_page(int page) {
    if (page < 0) page = DISP_PAGES - 1;
    if (page >= DISP_PAGES) page = 0;
    disp_page = page;

    // Leaving the night page ends its level preview, so the backlight always
    // matches whichever page is on screen.
    night_preview_end();
    night_cfg_flush();
    disp_build_page();

    if (disp_page_header != NULL) {
        lv_label_set_text(disp_page_header, disp_page_title(disp_page));
    }
    if (disp_page_counter != NULL) {
        char page_text[16];
        snprintf(page_text, sizeof(page_text), "%d / %d", disp_page + 1, DISP_PAGES);
        lv_label_set_text(disp_page_counter, page_text);
    }
}

static void disp_prev_page_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    disp_goto_page(disp_page - 1);
}

static void disp_next_page_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    disp_goto_page(disp_page + 1);
}

static void disp_back_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    close_brightness_overlay();
}

// ---- Lifecycle ----

// Everything the overlay owns beyond its widget tree: pending flash writes, the
// backlight preview, the idle timer, and the pointers callbacks reach through.
// Safe to run twice.
static void disp_release_state(void) {
    if (disp_idle_timer != NULL) {
        lv_timer_t *t = disp_idle_timer;
        disp_idle_timer = NULL;
        lv_timer_del(t);
    }
    night_preview_end();
    night_cfg_flush();
    brightness_commit();
    // Hand the backlight back to the schedule: night level if night dim is
    // (still) enabled and in-window, otherwise the day brightness just set.
    if (!visual_alarm_active) {
        display_set_brightness(cygm_current_scheduled_brightness());
    }
    disp_clear_refs();
    disp_page_body = NULL;
    disp_page_header = NULL;
    disp_page_counter = NULL;
    disp_page = 0;
}

// Reached only for a teardown this file did not initiate (screen_home freed
// under it): close_brightness_overlay() clears the global first, so the pointer
// comparison makes this a no-op on the normal path.
static void disp_overlay_delete_cb(lv_event_t *e) {
    if (lv_event_get_target(e) != brightness_overlay) return;
    brightness_overlay = NULL;
    disp_release_state();
}

// A full-screen sheet must never hide the reading indefinitely.
static void disp_idle_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (brightness_overlay == NULL) return;
    // While the device is driven over serial there is no touch activity, so
    // this timeout would close the sheet mid-session.
    if (cygm_ui_hold_active()) return;
    if (lv_disp_get_inactive_time(NULL) < DISP_IDLE_CLOSE_MS) return;

    ESP_LOGI(TAG, "Display settings idle timeout - closing");
    close_brightness_overlay();
}

// Full-screen sheet on screen_home, paged because the panel cannot hold every
// setting legibly and nothing here may scroll. Screen rows:
//   back chevron     4..40    | title 8..29 | page caption 14..25
//   page body       40..206
//   pager row      206..234
static void create_brightness_overlay(void) {
    if (brightness_overlay != NULL) {
        ESP_LOGI(TAG, "Display settings already open");
        return;
    }
    if (screen_home == NULL) return;

    ESP_LOGI(TAG, "Creating display settings overlay");

    disp_init_styles();
    disp_page = 0;
    disp_brightness_at_open = screen_brightness_percent;

    // Open at the day level the slider shows: during night dim the backlight
    // may be at 5%, and this sheet is the escape hatch so it must be readable.
    // The engine takes the backlight back on close.
    if (!visual_alarm_active) {
        display_set_brightness(screen_brightness_percent);
    }

    brightness_overlay = lv_obj_create(screen_home);
    lv_obj_set_size(brightness_overlay, 320, 240);
    lv_obj_set_pos(brightness_overlay, 0, 0);
    lv_obj_set_style_bg_color(brightness_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(brightness_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(brightness_overlay, 0, 0);
    lv_obj_set_style_radius(brightness_overlay, 0, 0);
    lv_obj_set_style_pad_all(brightness_overlay, 0, 0);
    lv_obj_set_style_shadow_width(brightness_overlay, 0, 0);
    lv_obj_clear_flag(brightness_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(brightness_overlay, disp_overlay_delete_cb, LV_EVENT_DELETE, NULL);

    disp_std_back_btn(brightness_overlay, disp_back_cb);

    lv_obj_t *title = lv_label_create(brightness_overlay);
    lv_label_set_text(title, "Display");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *cat_lbl = lv_label_create(brightness_overlay);
    lv_label_set_text(cat_lbl, disp_page_title(disp_page));
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cat_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cat_lbl, 2, 0);
    lv_obj_align(cat_lbl, LV_ALIGN_TOP_RIGHT, -12, 14);

    // Only the cards live here, so a page change rebuilds two cards rather than
    // the whole sheet — largest-free-block is the scarce resource.
    disp_page_body = lv_obj_create(brightness_overlay);
    lv_obj_remove_style_all(disp_page_body);
    lv_obj_set_size(disp_page_body, 300, 166);
    lv_obj_align(disp_page_body, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_clear_flag(disp_page_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev_btn = lv_btn_create(brightness_overlay);
    lv_obj_set_size(prev_btn, 44, 28);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 14, -6);
    lv_obj_update_layout(prev_btn);
    cygm_apply_ghost_btn(prev_btn);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, disp_prev_page_cb, LV_EVENT_CLICKED, NULL);

    char page_text[16];
    snprintf(page_text, sizeof(page_text), "%d / %d", disp_page + 1, DISP_PAGES);
    lv_obj_t *page_lbl = lv_label_create(brightness_overlay);
    lv_label_set_text(page_lbl, page_text);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t *next_btn = lv_btn_create(brightness_overlay);
    lv_obj_set_size(next_btn, 44, 28);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -14, -6);
    lv_obj_update_layout(next_btn);
    cygm_apply_ghost_btn(next_btn);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, disp_next_page_cb, LV_EVENT_CLICKED, NULL);

    disp_page_header = cat_lbl;
    disp_page_counter = page_lbl;
    disp_build_page();

    if (disp_idle_timer == NULL) {
        disp_idle_timer = lv_timer_create(disp_idle_timer_cb, 1000, NULL);
    }
}

static void close_brightness_overlay(void) {
    lv_obj_t *overlay = brightness_overlay;
    if (overlay == NULL) return;

    // Cleared before the delete so the DELETE handler's guard sees a mismatch
    // and leaves the teardown to this path.
    brightness_overlay = NULL;
    disp_release_state();
    lv_obj_del(overlay);
}

// --- CGM Expand/Collapse ---

static void home_gesture_event_cb(lv_event_t *e) {
    if (brightness_overlay != NULL) return;
    // The night face is a fixed layout; expanding would surface the top-bar
    // widgets it deliberately hides.
    if (night_face_active) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT && !cgm_expanded) {
        expand_cgm_card();
    } else if (dir == LV_DIR_RIGHT && cgm_expanded) {
        collapse_cgm_card();
    }
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

    // Instant switch, NO lv_anim: animations freeze this hardware. Snap
    // straight to the final expanded geometry.
    lv_obj_add_flag(left_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_x(right_card, EXPANDED_RIGHT_X);
    lv_obj_set_width(right_card, EXPANDED_RIGHT_W);

    // Show expanded top bar elements at full opacity
    lv_obj_t *exp_time_labels[] = {expanded_time_h, expanded_time_colon, expanded_time_m};
    for (int i = 0; i < 3; i++) {
        if (exp_time_labels[i] != NULL) {
            lv_obj_set_style_opa(exp_time_labels[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(exp_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (expanded_temp_label != NULL) {
        lv_obj_set_style_opa(expanded_temp_label, LV_OPA_COVER, 0);
        lv_obj_clear_flag(expanded_temp_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (expanded_weather_icon != NULL) {
        draw_mini_weather_icon(expanded_weather_icon, current_weather_code);
        lv_obj_set_style_opa(expanded_weather_icon, LV_OPA_COVER, 0);
        lv_obj_clear_flag(expanded_weather_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Reposition elements for expanded layout ---
    // Card is 308x220 (288x200 usable):
    //   top     battery | time + temp + weather | bulb, gear
    //   centre  glucose value, trend arrow, unit line
    //   bottom  delta, then the age stamp

    // Switch to large font for glucose — centered at fixed position
    if (label_glucose != NULL) {
        lv_obj_set_style_text_font(label_glucose, &font_montserrat_120, 0);
        lv_obj_align(label_glucose, LV_ALIGN_CENTER, -40, -8);
    }

    // Resize trend arrow canvas to 80x80 to match large digits
    if (trend_canvas != NULL) {
        lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 80, 80, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(trend_canvas, home_trend_bg(), LV_OPA_0);
        draw_trend_arrow_sized(trend_canvas, current_trend, 80);
    }

    // Trend arrow: fixed position on right side of card
    if (trend_canvas != NULL) {
        lv_obj_align(trend_canvas, LV_ALIGN_RIGHT_MID, -6, -8);
    }

    // Unit "mg/dL": below glucose, centered under the value
    if (label_unit != NULL && label_glucose != NULL) {
        lv_obj_align_to(label_unit, label_glucose, LV_ALIGN_OUT_BOTTOM_RIGHT, 20, -4);
    }

    // Time ago: centered near bottom
    if (label_time_ago != NULL) {
        lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    // Delta keeps the same spot it holds collapsed — centred one line above the
    // age stamp, clear of the 120pt digits and well left of the arrow column.
    if (label_glucose_delta != NULL) {
        lv_obj_align(label_glucose_delta, LV_ALIGN_BOTTOM_MID, 0, -26);
    }

    // Freshness arc: semicircle around expanded trend arrow
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, 100, 100);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_RIGHT_MID, 9, -8);  // Shifted right from trend arrow
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

    // Instant switch, NO lv_anim — see expand_cgm_card().
    lv_obj_set_style_opa(left_card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(left_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_x(right_card, NORMAL_RIGHT_X);
    lv_obj_set_width(right_card, NORMAL_RIGHT_W);

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
        lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 16);
    }
    if (label_unit != NULL) {
        lv_obj_align(label_unit, LV_ALIGN_TOP_MID, 0, 65);
    }
    if (trend_canvas != NULL) {
        lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 60, 60, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(trend_canvas, home_trend_bg(), LV_OPA_0);
        draw_trend_arrow(trend_canvas, current_trend);
        lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, HOME_TREND_Y_OFS);
    }
    if (label_time_ago != NULL) {
        lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
    if (label_glucose_delta != NULL) {
        lv_obj_align(label_glucose_delta, LV_ALIGN_BOTTOM_MID, 0, -26);
    }
    if (glucose_freshness_arc != NULL) {
        lv_obj_set_size(glucose_freshness_arc, HOME_ARC_SIZE, HOME_ARC_SIZE);
        lv_obj_align(glucose_freshness_arc, LV_ALIGN_CENTER, 0, HOME_TREND_Y_OFS);  // Same center as trend arrow
    }
    if (brightness_btn_ref != NULL) {
        lv_obj_align(brightness_btn_ref, LV_ALIGN_TOP_MID, 22, 0);
    }
}

void create_home_screen(void) {
    screen_home = lv_obj_create(NULL);
    lv_obj_add_style(screen_home, &style_bg, 0);
    lv_obj_clear_flag(screen_home, LV_OBJ_FLAG_SCROLLABLE);

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
    home_apply_tap_feedback(label_wifi_status, 12);  // Capped: the clock sits directly below

    // Split into hour/colon/minute so the colon can blink without jitter.
    // Montserrat digits are PROPORTIONAL, so fixed-width labels with RIGHT/LEFT
    // align are what keep them flush against the colon. Positioned with
    // lv_obj_align (parent-relative, persistent), never the one-shot align_to.
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
    // Tap feedback goes on last: the touch box is derived from the final size.
    // Capped — the date label sits immediately under these digits.
    home_apply_tap_feedback(label_clock_h, CYGM_BTN_EXT_CLICK);
    home_apply_tap_feedback(label_clock_m, CYGM_BTN_EXT_CLICK);

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
    lv_obj_align(label_date, LV_ALIGN_TOP_MID, 0, 66);  // 2px under the 48pt clock at y=16
    // Tap the date to switch month-day <-> day-month ordering
    lv_obj_add_flag(label_date, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_date, date_label_event_cb, LV_EVENT_CLICKED, NULL);
    home_apply_tap_feedback(label_date, CYGM_BTN_EXT_CLICK);  // Capped: clock digits above

    // Weather icon, below the date
    static lv_color_t weather_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(60, 50)];
    home_weather_canvas = lv_canvas_create(left_card);
    lv_canvas_set_buffer(home_weather_canvas, weather_buf, 60, 50, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_align(home_weather_canvas, LV_ALIGN_CENTER, 0, 11);
    lv_obj_set_style_bg_opa(home_weather_canvas, LV_OPA_TRANSP, 0);
    lv_canvas_fill_bg(home_weather_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    draw_weather_icon(home_weather_canvas, current_weather_code);

    // Temperature, below the weather icon
    home_temp_label = lv_label_create(left_card);
    char temp_buf[16];
    if (user_temp_celsius) {
        snprintf(temp_buf, sizeof(temp_buf), "%d C", weather_f_to_c(current_temp_f));
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "%d F", current_temp_f);
    }
    lv_label_set_text(home_temp_label, temp_buf);
    lv_obj_set_style_text_font(home_temp_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(home_temp_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(home_temp_label, LV_ALIGN_CENTER, -35, 50);
    // Tap to toggle F/C
    lv_obj_add_flag(home_temp_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(home_temp_label, temp_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    home_apply_tap_feedback(home_temp_label, HOME_EXT_UNCAPPED);

    // Hi/Lo temperatures (stacked vertically to the right)
    home_hilo_label = lv_label_create(left_card);
    char hilo_buf[32];
    if (user_temp_celsius) {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d",
                 weather_f_to_c(high_temp_f), weather_f_to_c(low_temp_f));
    } else {
        snprintf(hilo_buf, sizeof(hilo_buf), "H:%d\nL:%d", high_temp_f, low_temp_f);
    }
    lv_label_set_text(home_hilo_label, hilo_buf);
    lv_obj_set_style_text_font(home_hilo_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(home_hilo_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(home_hilo_label, LV_ALIGN_CENTER, 35, 50);

    // Weather condition text, below the temperature
    home_condition_label = lv_label_create(left_card);
    lv_label_set_text(home_condition_label, get_weather_condition_text(current_weather_code));
    lv_obj_set_style_text_font(home_condition_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(home_condition_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    // Sits high in the slack under the hi/lo column so the location line and
    // the sun-times row below both keep breathing room.
    lv_obj_align(home_condition_label, LV_ALIGN_CENTER, 0, 68);

    // Location (above sunrise/sunset, centered, closer to bottom)
    home_location_label = lv_label_create(left_card);
    lv_label_set_text(home_location_label, strlen(user_location) > 0 ? user_location : "");
    lv_label_set_long_mode(home_location_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(home_location_label, 130);
    lv_obj_set_style_text_font(home_location_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(home_location_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(home_location_label, LV_TEXT_ALIGN_CENTER, 0);
    // -11 leaves 5px between this line's descenders and the sun-times row;
    // any less and the two merge.
    lv_obj_align(home_location_label, LV_ALIGN_BOTTOM_MID, 0, -11);

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

    // Battery canvas - gauge, or the plug icon when no cell is fitted.
    // Tappable to toggle the percentage display.
    static lv_color_t canvas_buf_battery[LV_CANVAS_BUF_SIZE_TRUE_COLOR(30, 14)];
    battery_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(battery_canvas, canvas_buf_battery, 30, 14, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(battery_canvas, lv_color_hex(COLOR_CARD_BG), LV_OPA_0);
    if (battery_present) {
        draw_battery_icon(battery_canvas, COLOR_GREEN, 100);  // Start full
    } else {
        draw_plug_icon(battery_canvas, COLOR_TEXT_GRAY);
    }
    lv_obj_align(battery_canvas, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_add_flag(battery_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(battery_canvas, battery_tap_event_cb, LV_EVENT_CLICKED, NULL);
    home_apply_tap_feedback(battery_canvas, HOME_EXT_UNCAPPED);

    // Battery percentage label (next to icon, visible by default - tap battery to toggle)
    battery_percent_label = lv_label_create(right_card);
    lv_label_set_text(battery_percent_label, "");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(battery_percent_label, battery_canvas, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    if (!battery_present) lv_obj_add_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);

    // Brightness icon — custom lightbulb inside a clickable button
    brightness_btn_ref = lv_btn_create(right_card);
    lv_obj_t *brightness_btn = brightness_btn_ref;
    lv_obj_set_size(brightness_btn, 24, 24);
    lv_obj_align(brightness_btn, LV_ALIGN_TOP_MID, 22, 0);
    // Borderless icon button: these local props outrank the shared ghost look
    // at the default state, so only its pressed half and touch box show up.
    lv_obj_set_style_bg_opa(brightness_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brightness_btn, 0, 0);
    lv_obj_set_style_shadow_width(brightness_btn, 0, 0);
    lv_obj_set_style_pad_all(brightness_btn, 0, 0);
    lv_obj_add_event_cb(brightness_btn, brightness_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(brightness_btn);  // 24x24 — the touch box must grow past it
    cygm_apply_ghost_btn(brightness_btn);

    // Canvas for lightbulb drawing (inside button)
    static lv_color_t brightness_icon_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(20, 24)];
    brightness_icon = lv_canvas_create(brightness_btn);
    lv_canvas_set_buffer(brightness_icon, brightness_icon_buf, 20, 24, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_bg_opa(brightness_icon, LV_OPA_TRANSP, 0);
    draw_lightbulb_icon(brightness_icon, COLOR_ACCENT_BLUE);
    lv_obj_center(brightness_icon);

    // Glucose value
    label_glucose = lv_label_create(right_card);
    lv_label_set_text(label_glucose, "---");
    lv_obj_set_style_text_font(label_glucose, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_set_style_bg_opa(label_glucose, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label_glucose, 0, 0);
    lv_obj_align(label_glucose, LV_ALIGN_TOP_MID, 0, 16);

    // Unit — directly under glucose number
    label_unit = lv_label_create(right_card);
    lv_label_set_text(label_unit, cygm_glucose_unit());
    lv_obj_set_style_text_font(label_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_unit, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label_unit, LV_ALIGN_TOP_MID, 0, 65);
    // Tap the unit label to switch mg/dL <-> mmol/L (mirrors temp C/F tap toggle)
    lv_obj_add_flag(label_unit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_unit, glucose_unit_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    home_apply_tap_feedback(label_unit, HOME_EXT_UNCAPPED);

    // Trend arrow. Collapsed stack, in right-card content coordinates (0..196):
    //   unit 65..81 | arc drawn 84..124 | canvas 92..152 | delta 155..170
    //   age line 176..192
    // The arc is concentric with the canvas and paints only its top semicircle,
    // so its crown must clear the unit label while its inert lower half is free
    // to overlap the canvas and the delta.
    trend_canvas = lv_canvas_create(right_card);
    lv_canvas_set_buffer(trend_canvas, canvas_buf_trend, 60, 60, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_bg_opa(trend_canvas, LV_OPA_TRANSP, 0);
    lv_canvas_fill_bg(trend_canvas, home_trend_bg(), LV_OPA_0);
    lv_obj_align(trend_canvas, LV_ALIGN_CENTER, 0, HOME_TREND_Y_OFS);

    // Tap opens the history chart. The canvas already clears the minimum touch
    // target, so the box is capped tight — uncapped growth would reach down
    // over the delta label between the arrow and the age stamp.
    lv_obj_add_flag(trend_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(trend_canvas, trend_arrow_event_cb, LV_EVENT_CLICKED, NULL);
    home_apply_tap_feedback(trend_canvas, HOME_TREND_EXT_CAP);

    // Time ago (at bottom)
    label_time_ago = lv_label_create(right_card);
    lv_label_set_text(label_time_ago, "-- min ago");
    lv_obj_set_style_text_font(label_time_ago, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_time_ago, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(label_time_ago, LV_ALIGN_BOTTOM_MID, 0, -4);

    // Signed change vs the previous reading, plus the age stamp shown once the
    // value goes stale. The delta rides directly above the age line in both
    // layouts; the arc's box reaches past it but paints nothing below its own
    // centre line, so no pixel of it lands here.
    label_glucose_delta = lv_label_create(right_card);
    lv_label_set_text(label_glucose_delta, "");
    lv_obj_set_style_text_font(label_glucose_delta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_glucose_delta, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(label_glucose_delta, LV_ALIGN_BOTTOM_MID, 0, -26);  // 6px above "x min ago"
    lv_obj_add_flag(label_glucose_delta, LV_OBJ_FLAG_HIDDEN);

    label_glucose_age = lv_label_create(right_card);
    lv_label_set_text(label_glucose_age, "");
    lv_obj_set_style_text_font(label_glucose_age, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_glucose_age, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(label_glucose_age, LV_ALIGN_TOP_RIGHT, -2, 2);   // Corner both layouts leave free
    lv_obj_add_flag(label_glucose_age, LV_OBJ_FLAG_HIDDEN);

    // Countdown arc: semicircle halo around the trend arrow, drains toward
    // the next CGM pull (blue while the pull is running)
    glucose_freshness_arc = lv_arc_create(right_card);
    lv_obj_set_size(glucose_freshness_arc, HOME_ARC_SIZE, HOME_ARC_SIZE);
    lv_obj_align(glucose_freshness_arc, LV_ALIGN_CENTER, 0, HOME_TREND_Y_OFS);  // Same center as trend arrow
    lv_arc_set_rotation(glucose_freshness_arc, 0);
    lv_arc_set_bg_angles(glucose_freshness_arc, 180, 360); // Top semicircle only
    lv_arc_set_range(glucose_freshness_arc, 0, 100);
    lv_arc_set_value(glucose_freshness_arc, 100);
    lv_obj_remove_style(glucose_freshness_arc, NULL, LV_PART_KNOB);  // No knob
    lv_obj_clear_flag(glucose_freshness_arc, LV_OBJ_FLAG_CLICKABLE); // Not interactive
    lv_obj_set_style_pad_all(glucose_freshness_arc, 0, 0);

    // Arc indicator (the filled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_70, LV_PART_INDICATOR);

    // Arc background track (the unfilled part)
    lv_obj_set_style_arc_width(glucose_freshness_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(glucose_freshness_arc, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(glucose_freshness_arc, LV_OPA_10, LV_PART_MAIN);
    
    // --- MENU BUTTON (top right) ---
    lv_obj_t *menu_btn = lv_btn_create(screen_home);
    lv_obj_set_size(menu_btn, 36, 36);
    lv_obj_align(menu_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    // Borderless icon button — see the brightness button for why the local
    // transparent props are kept alongside the shared ghost style.
    lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_pad_all(menu_btn, 0, 0);

    menu_icon_ref = lv_label_create(menu_btn);
    lv_label_set_text(menu_icon_ref, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(menu_icon_ref, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(menu_icon_ref);

    // Attach event handler
    lv_obj_add_event_cb(menu_btn, menu_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(menu_btn);
    cygm_apply_ghost_btn(menu_btn);

    // --- Serial log indicator (below right card, only visible when logging) ---
    serial_log_indicator = lv_label_create(screen_home);
    lv_label_set_text(serial_log_indicator, "Log On");
    lv_obj_set_style_text_font(serial_log_indicator, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(serial_log_indicator, lv_color_hex(COLOR_ORANGE), 0);
    // 229 is the lowest row that still renders the whole 11px line before the
    // panel ends at 240, and the right card's edge clears this font's ink.
    lv_obj_set_pos(serial_log_indicator, 218, 229);
    if (!sd_serial_capture_get()) {
        lv_obj_add_flag(serial_log_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    // Set initial WiFi status icon based on current connection state
    if (wifi_connected) {
        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
    } else {
        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " " LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
    }

    if (glucose_timer_update == NULL) {
        glucose_timer_update = lv_timer_create(glucose_timer_update_cb, 1000, NULL);
    }

    if (weather_icon_anim_timer == NULL) {
        weather_icon_anim_timer = lv_timer_create(weather_icon_animation_timer_cb, 100, NULL);
    }

    // --- EXPANDED VIEW TOP BAR (hidden until the CGM card is expanded) ---
    // Split into three labels for a jitter-free blink, and positioned with
    // lv_obj_align (parent-relative) so they follow the card through the resize
    // — the one-shot lv_obj_align_to would strand them at the collapsed width.
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
    // Hide only now — expand_cgm_card() unhides these.
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

    // Fresh widgets carry their creation-time styles, so every 1Hz cache has to
    // start empty or the timer will skip the first real update.
    home_reset_change_caches();
}
