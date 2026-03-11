/*
 * alarm_screens.c
 *
 * Alarm configuration screens implementation
 */

#include "alarm_screens.h"
#include "shared_state.h"
#include "main.h"
#include "hardware/buzzer.h"
#include "nvs_config.h"
#include "sd_logger.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "ui/menu_screen.h"

static const char *TAG = "ALARM_SCREENS";

// Tone picker pagination
static int tone_picker_current_page = 0;
static alarm_config_t *tone_picker_alarm_ref = NULL;

// Tone names (full and short)
static const char *tone_display_names[] = {
    "Beep 1 (Single)", "Beep 2 (Double)", "Beep 3 (Triple)", "Chime (Gentle)",
    "Twinkle Twinkle", "Fur Elise", "Dixie Horn", "When the Saints",
    "Siren (Urgent)", "Ascending Scale", "Doorbell", "Big Ben (Westminster)"
};
static const char *tone_short_names[] = {
    "Beep 1", "Beep 2", "Beep 3", "Chime",
    "Twinkle", "Fur Elise", "Dixie", "Saints",
    "Siren", "Ascending", "Doorbell", "Big Ben"
};
#define TONES_PER_PAGE 4
#define TONE_PAGES 3

// Forward declarations for internal event callbacks
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_card_click_event_cb(lv_event_t *e);
static void alarm_threshold_slider_event_cb(lv_event_t *e);
static void alarm_volume_slider_event_cb(lv_event_t *e);
static void alarm_audio_toggle_event_cb(lv_event_t *e);
static void alarm_visual_toggle_event_cb(lv_event_t *e);
static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e);
static void alarm_settings_back_event_cb(lv_event_t *e);
static void tone_picker_prev_page_event_cb(lv_event_t *e);
static void tone_picker_next_page_event_cb(lv_event_t *e);
static void tone_select_btn_event_cb(lv_event_t *e);
static void tone_preview_btn_event_cb(lv_event_t *e);
static void tone_picker_close_event_cb(lv_event_t *e);
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);
static void alarm_snooze_event_cb(lv_event_t *e);
static void alarm_preview_back_event_cb(lv_event_t *e);
static void alarm_inactivity_timer_cb(lv_timer_t *timer);

static lv_timer_t *alarm_inactivity_timer = NULL;
#define ALARM_UI_INACTIVITY_RETURN_MS 30000
#define ALARM_VERBOSE_LOG 0

#if ALARM_VERBOSE_LOG
static const char *alarm_event_code_to_str(lv_event_code_t code) {
    switch (code) {
        case LV_EVENT_PRESSED: return "PRESSED";
        case LV_EVENT_PRESSING: return "PRESSING";
        case LV_EVENT_CLICKED: return "CLICKED";
        case LV_EVENT_VALUE_CHANGED: return "VALUE_CHANGED";
        case LV_EVENT_RELEASED: return "RELEASED";
        default: return "OTHER";
    }
}

static void alarm_trace_event(const char *cb_name, lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);
    void *ud = lv_event_get_user_data(e);
    uint32_t inactive = lv_disp_get_inactive_time(NULL);
    ESP_LOGI(TAG, "TRACE %s code=%s(%d) target=%p user=%p inactive=%lu heap=%lu",
             cb_name,
             alarm_event_code_to_str(code),
             (int)code,
             (void *)target,
             ud,
             (unsigned long)inactive,
             (unsigned long)esp_get_free_heap_size());
}
#else
#define alarm_trace_event(cb_name, e) ((void)0)
#endif

static int alarm_index_from_ptr(const alarm_config_t *alarm) {
    if (alarm == &current_alarm_settings.high_alarm) return 0;
    if (alarm == &current_alarm_settings.high_warning) return 1;
    if (alarm == &current_alarm_settings.low_warning) return 2;
    if (alarm == &current_alarm_settings.low_alarm) return 3;
    return -1;
}

// ==================== Alarm Settings Screen ====================

void create_alarm_settings_screen(void) {
    ESP_LOGI(TAG, "Creating alarm settings screen - free heap: %lu bytes", esp_get_free_heap_size());

    // Enforce off-home resource policy while in alarm UI
    home_screen_active = false;
    pause_background_tasks = true;

    if (alarm_inactivity_timer == NULL) {
        alarm_inactivity_timer = lv_timer_create(alarm_inactivity_timer_cb, 1000, NULL);
    }

    screen_alarm_settings = lv_obj_create(NULL);
    if (screen_alarm_settings == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm settings screen - out of memory!");
        return;
    }
    lv_obj_add_style(screen_alarm_settings, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_settings);
    lv_label_set_text(header, "Alarms");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 6);

    // Back button (ghost style - transparent with press highlight)
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_settings);
    lv_obj_set_size(back_btn, 40, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_settings_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Alarm cards with section grouping
    #define ALARM_CARD_W     290
    #define ALARM_CARD_H     42

    // HIGH section label (right-aligned, clear of back button)
    lv_obj_t *high_section = lv_label_create(screen_alarm_settings);
    lv_label_set_text(high_section, "HIGH ALERTS");
    lv_obj_set_style_text_font(high_section, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(high_section, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(high_section, 2, 0);
    lv_obj_align(high_section, LV_ALIGN_TOP_RIGHT, -18, 36);

    create_alarm_card(screen_alarm_settings, "High Alarm", &current_alarm_settings.high_alarm, 48, ALARM_CARD_H);
    create_alarm_card(screen_alarm_settings, "High Warning", &current_alarm_settings.high_warning, 94, ALARM_CARD_H);

    // LOW section label (right-aligned)
    lv_obj_t *low_section = lv_label_create(screen_alarm_settings);
    lv_label_set_text(low_section, "LOW ALERTS");
    lv_obj_set_style_text_font(low_section, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(low_section, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(low_section, 2, 0);
    lv_obj_align(low_section, LV_ALIGN_TOP_RIGHT, -18, 139);

    create_alarm_card(screen_alarm_settings, "Low Warning", &current_alarm_settings.low_warning, 151, ALARM_CARD_H);
    create_alarm_card(screen_alarm_settings, "Low Alarm", &current_alarm_settings.low_alarm, 197, ALARM_CARD_H);

    ESP_LOGI(TAG, "Alarm settings screen created - free heap: %lu bytes", esp_get_free_heap_size());
}

// ==================== Alarm Card ====================

void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm,
                              int y_pos, int height) {
    if (alarm == NULL || parent == NULL) {
        ESP_LOGE(TAG, "create_alarm_card: NULL parameter!");
        return;
    }

    // Card row (lv_btn for press highlight, tap to edit)
    lv_obj_t *card = lv_btn_create(parent);
    lv_obj_set_size(card, ALARM_CARD_W, height);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y_pos);

    // Card styling (matches menu cards)
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, 14, 0);
    lv_obj_set_style_pad_right(card, 12, 0);
    // Press highlight
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

    // Tap card to open detail editor
    lv_obj_add_event_cb(card, alarm_card_click_event_cb, LV_EVENT_CLICKED, alarm);

    // Colored bell icon (alarm accent color)
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(icon, lv_color_hex(alarm->text_color), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    // Alarm name
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, title);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 24, -7);

    // Threshold value (in alarm color, below name)
    char threshold_text[16];
    snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", alarm->threshold);
    lv_obj_t *thresh_lbl = lv_label_create(card);
    lv_label_set_text(thresh_lbl, threshold_text);
    lv_obj_set_style_text_font(thresh_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(thresh_lbl, lv_color_hex(alarm->text_color), 0);
    lv_obj_align(thresh_lbl, LV_ALIGN_LEFT_MID, 24, 8);

    // Right arrow (edit indicator)
    lv_obj_t *arrow = lv_label_create(card);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

    // Toggle switch (right side, before arrow)
    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_remove_style_all(sw);  // Strip ALL theme styles (prevents transition animation hangs)
    lv_obj_set_size(sw, 40, 20);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -22, 0);

    // Track (main background)
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2a3545), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 10, LV_PART_MAIN);

    // Indicator (colored fill when checked)
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2a3545), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 10, LV_PART_INDICATOR);

    // Knob (sliding circle)
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);

    if (alarm->enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, alarm_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);
}

// ==================== Detail Editor Screen ====================

void create_alarm_detail_editor_screen(alarm_config_t *alarm) {
    ESP_LOGI(TAG, "Creating alarm detail editor screen - free heap: %lu bytes", esp_get_free_heap_size());

    if (alarm == NULL) {
        ESP_LOGE(TAG, "create_alarm_detail_editor_screen: alarm is NULL!");
        return;
    }

    screen_alarm_detail_editor = lv_obj_create(NULL);
    if (screen_alarm_detail_editor == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm detail editor screen - out of memory!");
        return;
    }
    lv_obj_add_style(screen_alarm_detail_editor, &style_bg, 0);

    // Determine alarm name for header
    const char *alarm_name = "Alarm";
    if (alarm == &current_alarm_settings.high_alarm) alarm_name = "High Alarm";
    else if (alarm == &current_alarm_settings.high_warning) alarm_name = "High Warning";
    else if (alarm == &current_alarm_settings.low_warning) alarm_name = "Low Warning";
    else if (alarm == &current_alarm_settings.low_alarm) alarm_name = "Low Alarm";

    // --- Header: colored bell icon + alarm name ---
    lv_obj_t *header = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(header, alarm_name);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 8, 8);

    lv_obj_t *header_icon = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(header_icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(header_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header_icon, lv_color_hex(alarm->text_color), 0);
    lv_obj_align_to(header_icon, header, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    // Back button (ghost style)
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_detail_editor);
    lv_obj_set_size(back_btn, 40, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_detail_editor_back_event_cb, LV_EVENT_CLICKED, NULL);

    // ---- Threshold Card ----
    lv_obj_t *thresh_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(thresh_card, 290, 54);
    lv_obj_align(thresh_card, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(thresh_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(thresh_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thresh_card, 12, 0);
    lv_obj_set_style_border_width(thresh_card, 0, 0);
    lv_obj_set_style_pad_all(thresh_card, 0, 0);
    lv_obj_clear_flag(thresh_card, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent bar (alarm color)
    lv_obj_t *thresh_accent = lv_obj_create(thresh_card);
    lv_obj_remove_style_all(thresh_accent);
    lv_obj_set_size(thresh_accent, 3, 38);
    lv_obj_align(thresh_accent, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(thresh_accent, lv_color_hex(alarm->text_color), 0);
    lv_obj_set_style_bg_opa(thresh_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thresh_accent, 2, 0);

    lv_obj_t *thresh_label = lv_label_create(thresh_card);
    lv_label_set_text(thresh_label, "Threshold");
    lv_obj_set_style_text_font(thresh_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(thresh_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(thresh_label, LV_ALIGN_TOP_LEFT, 16, 8);

    // Threshold value display (prominent, in alarm color)
    lv_obj_t *thresh_value = lv_label_create(thresh_card);
    char threshold_text[16];
    snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", alarm->threshold);
    lv_label_set_text(thresh_value, threshold_text);
    lv_obj_set_style_text_font(thresh_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(thresh_value, lv_color_hex(alarm->text_color), 0);
    lv_obj_align(thresh_value, LV_ALIGN_TOP_RIGHT, -12, 7);
    alarm_threshold_value_label = thresh_value;

    // Threshold slider
    lv_obj_t *thresh_slider = lv_slider_create(thresh_card);
    lv_obj_set_size(thresh_slider, 260, 12);
    lv_obj_align(thresh_slider, LV_ALIGN_TOP_MID, 0, 32);
    lv_slider_set_range(thresh_slider, 50, 400);
    lv_slider_set_value(thresh_slider, alarm->threshold, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(thresh_slider, lv_color_hex(0x0d1520), LV_PART_MAIN);
    lv_obj_set_style_radius(thresh_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_border_color(thresh_slider, lv_color_hex(0x1e2d3d), LV_PART_MAIN);
    lv_obj_set_style_border_width(thresh_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(thresh_slider, lv_color_hex(alarm->text_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(thresh_slider, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(thresh_slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(thresh_slider, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(thresh_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_pad_all(thresh_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(thresh_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(thresh_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(thresh_slider, LV_OPA_30, LV_PART_KNOB);

    lv_obj_set_user_data(thresh_slider, thresh_value);
    lv_obj_add_event_cb(thresh_slider, alarm_threshold_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // ---- Audio Card ----
    lv_obj_t *audio_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(audio_card, 290, 82);
    lv_obj_align(audio_card, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(audio_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(audio_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(audio_card, 12, 0);
    lv_obj_set_style_border_width(audio_card, 0, 0);
    lv_obj_set_style_pad_all(audio_card, 0, 0);
    lv_obj_clear_flag(audio_card, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent bar (blue)
    lv_obj_t *audio_accent = lv_obj_create(audio_card);
    lv_obj_remove_style_all(audio_accent);
    lv_obj_set_size(audio_accent, 3, 66);
    lv_obj_align(audio_accent, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(audio_accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(audio_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(audio_accent, 2, 0);

    // Speaker icon
    lv_obj_t *audio_icon = lv_label_create(audio_card);
    lv_label_set_text(audio_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(audio_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(audio_icon, LV_ALIGN_TOP_LEFT, 16, 9);

    // Audio Alert label + switch
    lv_obj_t *audio_label = lv_label_create(audio_card);
    lv_label_set_text(audio_label, "Audio Alert");
    lv_obj_set_style_text_font(audio_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(audio_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(audio_label, LV_ALIGN_TOP_LEFT, 34, 8);

    lv_obj_t *audio_sw = lv_switch_create(audio_card);
    lv_obj_remove_style_all(audio_sw);
    lv_obj_set_size(audio_sw, 40, 20);
    lv_obj_align(audio_sw, LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_set_style_bg_opa(audio_sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(audio_sw, lv_color_hex(0x2a3545), LV_PART_MAIN);
    lv_obj_set_style_radius(audio_sw, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(audio_sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(audio_sw, lv_color_hex(0x2a3545), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(audio_sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(audio_sw, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(audio_sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(audio_sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(audio_sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(audio_sw, -3, LV_PART_KNOB);
    if (alarm->audio_enabled) lv_obj_add_state(audio_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(audio_sw, alarm_audio_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Divider line
    lv_obj_t *audio_div = lv_obj_create(audio_card);
    lv_obj_remove_style_all(audio_div);
    lv_obj_set_size(audio_div, 262, 1);
    lv_obj_align(audio_div, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(audio_div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(audio_div, LV_OPA_COVER, 0);

    // Tone selection button
    int tone_idx = (alarm->tone >= 0 && alarm->tone < ALARM_TONE_COUNT) ? alarm->tone : 0;
    char tone_text[32];
    snprintf(tone_text, sizeof(tone_text), "Tone: %s", tone_short_names[tone_idx]);

    lv_obj_t *tone_btn = lv_btn_create(audio_card);
    lv_obj_set_size(tone_btn, 150, 26);
    lv_obj_align(tone_btn, LV_ALIGN_TOP_LEFT, 10, 36);
    lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x2a3a4a), 0);
    lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x3a4a5a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tone_btn, 8, 0);
    lv_obj_set_style_border_width(tone_btn, 0, 0);
    lv_obj_set_style_shadow_width(tone_btn, 0, 0);

    lv_obj_t *tone_lbl = lv_label_create(tone_btn);
    lv_label_set_text(tone_lbl, tone_text);
    lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(tone_lbl);
    lv_obj_add_event_cb(tone_btn, alarm_tone_btn_event_cb, LV_EVENT_CLICKED, alarm);

    // Volume label + value
    lv_obj_t *vol_label = lv_label_create(audio_card);
    lv_label_set_text(vol_label, "Vol");
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(vol_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(vol_label, LV_ALIGN_TOP_LEFT, 170, 44);

    lv_obj_t *vol_value = lv_label_create(audio_card);
    char vol_text[8];
    snprintf(vol_text, sizeof(vol_text), "%d%%", alarm->volume);
    lv_label_set_text(vol_value, vol_text);
    lv_obj_set_style_text_font(vol_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(vol_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(vol_value, LV_ALIGN_TOP_RIGHT, -12, 44);

    // Volume slider
    lv_obj_t *vol_slider = lv_slider_create(audio_card);
    lv_obj_set_size(vol_slider, 260, 10);
    lv_obj_align(vol_slider, LV_ALIGN_TOP_MID, 0, 64);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, alarm->volume, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x0d1520), LV_PART_MAIN);
    lv_obj_set_style_radius(vol_slider, 5, LV_PART_MAIN);
    lv_obj_set_style_border_color(vol_slider, lv_color_hex(0x1e2d3d), LV_PART_MAIN);
    lv_obj_set_style_border_width(vol_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_slider, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(vol_slider, 5, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol_slider, 1, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(vol_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(vol_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(vol_slider, LV_OPA_30, LV_PART_KNOB);

    lv_obj_set_user_data(vol_slider, vol_value);
    lv_obj_add_event_cb(vol_slider, alarm_volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // ---- Visual / Repeat Card ----
    lv_obj_t *alerts_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(alerts_card, 290, 56);
    lv_obj_align(alerts_card, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_set_style_bg_color(alerts_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(alerts_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(alerts_card, 12, 0);
    lv_obj_set_style_border_width(alerts_card, 0, 0);
    lv_obj_set_style_pad_all(alerts_card, 0, 0);
    lv_obj_clear_flag(alerts_card, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent bar (green)
    lv_obj_t *alerts_accent = lv_obj_create(alerts_card);
    lv_obj_remove_style_all(alerts_accent);
    lv_obj_set_size(alerts_accent, 3, 40);
    lv_obj_align(alerts_accent, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(alerts_accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(alerts_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(alerts_accent, 2, 0);

    // Eye icon
    lv_obj_t *vis_icon = lv_label_create(alerts_card);
    lv_label_set_text(vis_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(vis_icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(vis_icon, LV_ALIGN_TOP_LEFT, 16, 7);

    // Visual Alert label + switch
    lv_obj_t *vis_label = lv_label_create(alerts_card);
    lv_label_set_text(vis_label, "Visual Alert");
    lv_obj_set_style_text_font(vis_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vis_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(vis_label, LV_ALIGN_TOP_LEFT, 34, 6);

    lv_obj_t *vis_sw = lv_switch_create(alerts_card);
    lv_obj_remove_style_all(vis_sw);
    lv_obj_set_size(vis_sw, 40, 20);
    lv_obj_align(vis_sw, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_obj_set_style_bg_opa(vis_sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vis_sw, lv_color_hex(0x2a3545), LV_PART_MAIN);
    lv_obj_set_style_radius(vis_sw, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vis_sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vis_sw, lv_color_hex(0x2a3545), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vis_sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(vis_sw, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vis_sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(vis_sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(vis_sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vis_sw, -3, LV_PART_KNOB);
    if (alarm->visual_enabled) lv_obj_add_state(vis_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(vis_sw, alarm_visual_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Divider line
    lv_obj_t *alerts_div = lv_obj_create(alerts_card);
    lv_obj_remove_style_all(alerts_div);
    lv_obj_set_size(alerts_div, 262, 1);
    lv_obj_align(alerts_div, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(alerts_div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(alerts_div, LV_OPA_COVER, 0);

    // Loop icon
    lv_obj_t *repeat_icon = lv_label_create(alerts_card);
    lv_label_set_text(repeat_icon, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(repeat_icon, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(repeat_icon, LV_ALIGN_TOP_LEFT, 16, 34);

    // Repeat label + switch
    lv_obj_t *repeat_label = lv_label_create(alerts_card);
    lv_label_set_text(repeat_label, "Repeat");
    lv_obj_set_style_text_font(repeat_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(repeat_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(repeat_label, LV_ALIGN_TOP_LEFT, 34, 33);

    lv_obj_t *repeat_sw = lv_switch_create(alerts_card);
    lv_obj_remove_style_all(repeat_sw);
    lv_obj_set_size(repeat_sw, 40, 20);
    lv_obj_align(repeat_sw, LV_ALIGN_TOP_RIGHT, -12, 31);
    lv_obj_set_style_bg_opa(repeat_sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(repeat_sw, lv_color_hex(0x2a3545), LV_PART_MAIN);
    lv_obj_set_style_radius(repeat_sw, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(repeat_sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(repeat_sw, lv_color_hex(0x2a3545), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(repeat_sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(repeat_sw, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(repeat_sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(repeat_sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(repeat_sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(repeat_sw, -3, LV_PART_KNOB);
    if (alarm->audio_repeat) lv_obj_add_state(repeat_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(repeat_sw, alarm_audio_repeat_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    ESP_LOGI(TAG, "Alarm detail editor screen created - free heap: %lu bytes", esp_get_free_heap_size());
}

// ==================== Tone Picker (Modal Overlay) ====================

void create_tone_picker_screen(alarm_config_t *alarm) {
    tone_picker_alarm_ref = alarm;

    // Dimmed backdrop (fullscreen, clickable to dismiss)
    screen_tone_picker = lv_obj_create(lv_scr_act());
    if (screen_tone_picker == NULL) {
        ESP_LOGE(TAG, "Failed to create tone picker overlay!");
        return;
    }
    lv_obj_remove_style_all(screen_tone_picker);
    lv_obj_set_size(screen_tone_picker, 320, 240);
    lv_obj_set_pos(screen_tone_picker, 0, 0);
    lv_obj_set_style_bg_color(screen_tone_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_tone_picker, LV_OPA_60, 0);
    lv_obj_clear_flag(screen_tone_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen_tone_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen_tone_picker, tone_picker_close_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Large centered card (fills most of screen) ---
    lv_obj_t *card = lv_obj_create(screen_tone_picker);
    lv_obj_set_size(card, 296, 224);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Close button (X) — top-right
    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_radius(close_btn, 16, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, tone_picker_close_event_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Select Tone");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

    // Category label (changes per page)
    const char *category;
    if (tone_picker_current_page == 0) category = "Basic Tones";
    else if (tone_picker_current_page == 1) category = "Melodies";
    else category = "Alerts & Signals";
    lv_obj_t *cat_lbl = lv_label_create(card);
    lv_label_set_text(cat_lbl, category);
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cat_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(cat_lbl, LV_ALIGN_TOP_LEFT, 16, 32);

    // --- Tone rows (4 per page) ---
    int start_idx = tone_picker_current_page * TONES_PER_PAGE;
    int end_idx = start_idx + TONES_PER_PAGE;
    if (end_idx > ALARM_TONE_COUNT) end_idx = ALARM_TONE_COUNT;

    #define TONE_ROW_Y_START  46
    #define TONE_ROW_H        33
    #define TONE_ROW_GAP      4
    #define TONE_ROW_W        266

    for (int i = start_idx; i < end_idx; i++) {
        int row_idx = i - start_idx;
        int row_y = TONE_ROW_Y_START + row_idx * (TONE_ROW_H + TONE_ROW_GAP);

        lv_obj_t *tone_btn = lv_btn_create(card);
        lv_obj_set_size(tone_btn, TONE_ROW_W, TONE_ROW_H);
        lv_obj_align(tone_btn, LV_ALIGN_TOP_MID, 0, row_y);
        lv_obj_set_style_radius(tone_btn, 10, 0);
        lv_obj_set_style_border_width(tone_btn, 0, 0);
        lv_obj_set_style_shadow_width(tone_btn, 0, 0);
        lv_obj_set_style_pad_left(tone_btn, 12, 0);
        lv_obj_set_style_pad_right(tone_btn, 6, 0);

        // Selected tone gets blue bg; others get dark
        if (alarm->tone == i) {
            lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        } else {
            lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x1e2836), 0);
        }
        lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x3a4a5a), LV_STATE_PRESSED);

        // Checkmark for selected item
        if (alarm->tone == i) {
            lv_obj_t *check = lv_label_create(tone_btn);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(check, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_align(check, LV_ALIGN_LEFT_MID, 0, 0);
        }

        // Tone name
        lv_obj_t *tone_name = lv_label_create(tone_btn);
        lv_label_set_text(tone_name, tone_display_names[i]);
        lv_obj_set_style_text_font(tone_name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(tone_name, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(tone_name, LV_ALIGN_LEFT_MID, (alarm->tone == i) ? 22 : 0, 0);

        // Preview play button (green circle)
        lv_obj_t *play_btn = lv_btn_create(tone_btn);
        lv_obj_set_size(play_btn, 30, 28);
        lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1a9a4a), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(play_btn, 0, 0);
        lv_obj_set_style_shadow_width(play_btn, 0, 0);
        lv_obj_set_style_radius(play_btn, 8, 0);

        lv_obj_t *play_lbl = lv_label_create(play_btn);
        lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(play_lbl);
        lv_obj_add_event_cb(play_btn, tone_preview_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Select tone on row click
        lv_obj_add_event_cb(tone_btn, tone_select_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // --- Bottom navigation bar ---
    // Last row bottom: 48 + 3*(34+4) + 34 = 48 + 114 + 34 = 196
    // Nav area starts at y=202

    // Prev arrow
    lv_obj_t *prev_btn = lv_btn_create(card);
    lv_obj_set_size(prev_btn, 44, 26);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 16, -4);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x1e2836), 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x3a4a5a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(prev_btn, 8, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, tone_picker_prev_page_event_cb, LV_EVENT_CLICKED, NULL);

    // Page dots (centered)
    char page_text[16];
    snprintf(page_text, sizeof(page_text), "%d / %d", tone_picker_current_page + 1, TONE_PAGES);
    lv_obj_t *page_lbl = lv_label_create(card);
    lv_label_set_text(page_lbl, page_text);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Next arrow
    lv_obj_t *next_btn = lv_btn_create(card);
    lv_obj_set_size(next_btn, 44, 26);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -4);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x1e2836), 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x3a4a5a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(next_btn, 8, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, tone_picker_next_page_event_cb, LV_EVENT_CLICKED, NULL);
}

// ==================== Alarm Preview Screen ====================

void create_alarm_preview_screen(void) {
    screen_alarm_preview = lv_obj_create(NULL);
    lv_obj_add_style(screen_alarm_preview, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_preview);
    lv_label_set_text(header, "Alarm Preview");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_preview);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_preview_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Preview cards
    create_alarm_preview_card(screen_alarm_preview, "High Alarm", 280,
                              &current_alarm_settings.high_alarm,
                              lv_color_hex(current_alarm_settings.high_alarm.text_color),
                              lv_color_hex(current_alarm_settings.high_alarm.text_color));

    create_alarm_preview_card(screen_alarm_preview, "High Warning", 190,
                              &current_alarm_settings.high_warning,
                              lv_color_hex(current_alarm_settings.high_warning.text_color),
                              lv_color_hex(current_alarm_settings.high_warning.text_color));

    create_alarm_preview_card(screen_alarm_preview, "Low Warning", 75,
                              &current_alarm_settings.low_warning,
                              lv_color_hex(current_alarm_settings.low_warning.text_color),
                              lv_color_hex(current_alarm_settings.low_warning.text_color));

    create_alarm_preview_card(screen_alarm_preview, "Low Alarm", 48,
                              &current_alarm_settings.low_alarm,
                              lv_color_hex(current_alarm_settings.low_alarm.text_color),
                              lv_color_hex(current_alarm_settings.low_alarm.text_color));
}

void create_alarm_preview_card(lv_obj_t *parent, const char *title, int glucose_value,
                               alarm_config_t *alarm, lv_color_t color_low, lv_color_t color_high) {
    int child_count = lv_obj_get_child_cnt(parent);
    int card_idx = (child_count > 2) ? child_count - 2 : 0;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 290, 42);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 50 + card_idx * 48);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title (left)
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    // Glucose value (right, colored)
    char glucose_text[16];
    snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", glucose_value);
    lv_obj_t *glucose_lbl = lv_label_create(card);
    lv_label_set_text(glucose_lbl, glucose_text);
    lv_obj_set_style_text_font(glucose_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(glucose_lbl, color_low, 0);
    lv_obj_align(glucose_lbl, LV_ALIGN_RIGHT_MID, -12, 0);
}

// ==================== Alarm Snooze Screen ====================

void create_alarm_snooze_screen(void) {
    // Determine which alarm triggered
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

    // ---- Glass panel overlay on home screen (chart-style) ----
    screen_alarm_snooze = lv_obj_create(screen_home);
    lv_obj_set_size(screen_alarm_snooze, 290, 210);
    lv_obj_center(screen_alarm_snooze);
    lv_obj_set_style_bg_color(screen_alarm_snooze, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(screen_alarm_snooze, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(screen_alarm_snooze, 16, 0);
    lv_obj_set_style_border_width(screen_alarm_snooze, 1, 0);
    lv_obj_set_style_border_color(screen_alarm_snooze, lv_color_hex(0x2A3A4A), 0);
    lv_obj_set_style_border_opa(screen_alarm_snooze, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(screen_alarm_snooze, 25, 0);
    lv_obj_set_style_shadow_color(screen_alarm_snooze, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(screen_alarm_snooze, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(screen_alarm_snooze, 0, 0);
    lv_obj_clear_flag(screen_alarm_snooze, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Title row: bell + alarm type ----
    lv_obj_t *alarm_icon = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(alarm_icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(alarm_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(alarm_icon, lv_color_hex(alarm_color), 0);
    lv_obj_align(alarm_icon, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t *type_label = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(type_label, alarm_type);
    lv_obj_set_style_text_font(type_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(type_label, lv_color_hex(alarm_color), 0);
    lv_obj_align(type_label, LV_ALIGN_TOP_LEFT, 40, 14);

    // ---- Divider ----
    lv_obj_t *div = lv_obj_create(screen_alarm_snooze);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 270, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // ---- Glucose value (centered, prominent) ----
    lv_obj_t *glucose_label = lv_label_create(screen_alarm_snooze);
    char glucose_text[16];
    snprintf(glucose_text, sizeof(glucose_text), "%d", current_glucose);
    lv_label_set_text(glucose_label, glucose_text);
    lv_obj_set_style_text_font(glucose_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(glucose_label, lv_color_hex(alarm_color), 0);
    lv_obj_align(glucose_label, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *unit_lbl = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(unit_lbl, "mg/dL");
    lv_obj_set_style_text_font(unit_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(unit_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align_to(unit_lbl, glucose_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    // ---- Snooze section header ----
    lv_obj_t *snooze_hdr = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(snooze_hdr, "SNOOZE");
    lv_obj_set_style_text_font(snooze_hdr, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(snooze_hdr, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(snooze_hdr, 2, 0);
    lv_obj_align(snooze_hdr, LV_ALIGN_TOP_LEFT, 16, 116);

    // ---- Snooze buttons (ghost-style, 4 in a row) ----
    int snooze_times[] = {5, 10, 15, 30};
    int x_offsets[] = {-100, -34, 32, 98};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(screen_alarm_snooze);
        lv_obj_set_size(btn, 60, 30);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, x_offsets[i], 132);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        lv_obj_t *label = lv_label_create(btn);
        char text[16];
        snprintf(text, sizeof(text), "%d min", snooze_times[i]);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)snooze_times[i]);
    }

    // ---- Dismiss button (ghost-red style) ----
    lv_obj_t *dismiss_btn = lv_btn_create(screen_alarm_snooze);
    lv_obj_set_size(dismiss_btn, 260, 34);
    lv_obj_align(dismiss_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(dismiss_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_opa(dismiss_btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(dismiss_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(dismiss_btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(dismiss_btn, 8, 0);
    lv_obj_set_style_border_width(dismiss_btn, 1, 0);
    lv_obj_set_style_border_color(dismiss_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_opa(dismiss_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(dismiss_btn, 0, 0);

    lv_obj_t *dismiss_label = lv_label_create(dismiss_btn);
    lv_label_set_text(dismiss_label, "Dismiss (3 Hours)");
    lv_obj_set_style_text_font(dismiss_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dismiss_label, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(dismiss_label);

    lv_obj_add_event_cb(dismiss_btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)180);
}

// ==================== Event Handlers ====================

static void alarm_toggle_event_cb(lv_event_t *e) {
    alarm_trace_event("alarm_toggle_event_cb", e);
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *sw = lv_event_get_target(e);
    alarm->enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    int idx = alarm_index_from_ptr(alarm);
    ESP_LOGI(TAG, "Alarm enabled: %d (idx=%d)", alarm->enabled, idx);
}

static void alarm_card_click_event_cb(lv_event_t *e) {
    alarm_trace_event("alarm_card_click_event_cb", e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // Ignore clicks that targeted a child widget (e.g. the switch)
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;

    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_card_click_event_cb: alarm is NULL!");
        return;
    }

    ESP_LOGI(TAG, "Alarm card clicked - opening editor for threshold: %d", alarm->threshold);
    editing_alarm = alarm;

    if (screen_alarm_detail_editor != NULL) {
        lv_obj_del(screen_alarm_detail_editor);
        screen_alarm_detail_editor = NULL;
    }

    create_alarm_detail_editor_screen(alarm);

    if (screen_alarm_detail_editor != NULL) {
        lv_scr_load(screen_alarm_detail_editor);

        if (screen_alarm_settings != NULL) {
            lv_obj_del(screen_alarm_settings);
            screen_alarm_settings = NULL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to create detail editor screen!");
    }
}

static void alarm_threshold_slider_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    alarm->threshold = (int)value;

    lv_obj_t *threshold_value = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (threshold_value != NULL) {
        char threshold_text[16];
        snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", (int)value);
        lv_label_set_text(threshold_value, threshold_text);
    }
}

static void alarm_volume_slider_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    alarm->volume = (uint8_t)value;

    lv_obj_t *volume_value = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (volume_value != NULL) {
        char volume_text[8];
        snprintf(volume_text, sizeof(volume_text), "%d%%", (int)value);
        lv_label_set_text(volume_value, volume_text);
    }
}

static void alarm_audio_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->audio_enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Audio enabled: %d", alarm->audio_enabled);
}

static void alarm_visual_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->visual_enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Visual enabled: %d", alarm->visual_enabled);
}

static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->audio_repeat = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Audio repeat enabled: %d", alarm->audio_repeat);
}

static void alarm_settings_back_event_cb(lv_event_t *e) {
    alarm_trace_event("alarm_settings_back_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back to menu - saving alarm settings");
        nvs_save_alarm_settings(&current_alarm_settings);

        // Stop inactivity timer before leaving alarm UI
        if (alarm_inactivity_timer != NULL) {
            lv_timer_del(alarm_inactivity_timer);
            alarm_inactivity_timer = NULL;
        }

        // Recreate menu (was deleted when entering alarms)
        create_menu_screen();
        lv_scr_load(screen_menu);

        if (screen_alarm_settings != NULL) {
            lv_obj_del(screen_alarm_settings);
            screen_alarm_settings = NULL;
        }

        editing_alarm = NULL;
        home_screen_active = false;
        pause_background_tasks = false;
    }
}

static void tone_picker_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (screen_tone_picker != NULL) {
            lv_obj_del(screen_tone_picker);
            screen_tone_picker = NULL;
            tone_picker_current_page = 0;
            tone_picker_alarm_ref = NULL;
        }
    }
}

static void tone_picker_prev_page_event_cb(lv_event_t *e) {
    alarm_trace_event("tone_picker_prev_page_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        tone_picker_current_page--;
        if (tone_picker_current_page < 0) {
            tone_picker_current_page = TONE_PAGES - 1;
        }

        ESP_LOGI(TAG, "Tone picker: previous page -> %d", tone_picker_current_page);

        if (screen_tone_picker != NULL) {
            lv_obj_del(screen_tone_picker);
            screen_tone_picker = NULL;
        }
        create_tone_picker_screen(tone_picker_alarm_ref);
    }
}

static void tone_picker_next_page_event_cb(lv_event_t *e) {
    alarm_trace_event("tone_picker_next_page_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        tone_picker_current_page++;
        if (tone_picker_current_page >= TONE_PAGES) {
            tone_picker_current_page = 0;
        }

        ESP_LOGI(TAG, "Tone picker: next page -> %d", tone_picker_current_page);

        if (screen_tone_picker != NULL) {
            lv_obj_del(screen_tone_picker);
            screen_tone_picker = NULL;
        }
        create_tone_picker_screen(tone_picker_alarm_ref);
    }
}

static void tone_select_btn_event_cb(lv_event_t *e) {
    alarm_trace_event("tone_select_btn_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int tone_index = (int)(intptr_t)lv_event_get_user_data(e);

        if (editing_alarm != NULL) {
            editing_alarm->tone = tone_index;
            ESP_LOGI(TAG, "Tone selected: %d", tone_index);
        }

        // Close overlay and return to alarm settings list
        if (screen_tone_picker != NULL) {
            lv_obj_del(screen_tone_picker);
            screen_tone_picker = NULL;
        }

        tone_picker_current_page = 0;
        tone_picker_alarm_ref = NULL;

        nvs_save_alarm_settings(&current_alarm_settings);

        if (screen_alarm_settings != NULL) {
            lv_obj_del(screen_alarm_settings);
            screen_alarm_settings = NULL;
        }

        create_alarm_settings_screen();

        if (screen_alarm_settings != NULL) {
            lv_scr_load(screen_alarm_settings);
        }
    }
}

static void tone_preview_btn_event_cb(lv_event_t *e) {
    alarm_trace_event("tone_preview_btn_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int tone_index = (int)(intptr_t)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Preview tone: %d", tone_index);
        play_alarm_tone(tone_index, 100);
    }
}

static void alarm_detail_editor_back_event_cb(lv_event_t *e) {
    alarm_trace_event("alarm_detail_editor_back_event_cb", e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Detail editor back - saving and returning to alarm settings");
        nvs_save_alarm_settings(&current_alarm_settings);

        if (screen_alarm_settings != NULL) {
            lv_obj_del(screen_alarm_settings);
            screen_alarm_settings = NULL;
        }

        create_alarm_settings_screen();

        if (screen_alarm_settings != NULL) {
            lv_scr_load(screen_alarm_settings);
        }

        if (screen_alarm_detail_editor != NULL) {
            lv_obj_del(screen_alarm_detail_editor);
            screen_alarm_detail_editor = NULL;
        }

        editing_alarm = NULL;
    }
}

static void alarm_tone_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);

        ESP_LOGI(TAG, "Tone button clicked - opening tone picker");

        if (screen_tone_picker != NULL) {
            lv_obj_del(screen_tone_picker);
            screen_tone_picker = NULL;
        }

        tone_picker_current_page = 0;
        create_tone_picker_screen(alarm);
    }
}

static void alarm_snooze_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int snooze_minutes = (int)(intptr_t)lv_event_get_user_data(e);

        time_t now;
        time(&now);
        alarm_snooze_until = now + (snooze_minutes * 60);

        ESP_LOGI(TAG, "Alarm snoozed for %d minutes (until timestamp %ld, current %ld)",
                 snooze_minutes, (long)alarm_snooze_until, (long)now);
        sd_log(TAG, "ALARM: snoozed %dmin, glucose=%d, state=%d",
               snooze_minutes, current_glucose, current_alarm_state);

        alarm_acknowledged = true;

        if (visual_alarm_active) {
            visual_alarm_active = false;
        }

        lv_scr_load(screen_home);

        if (visual_alarm_overlay != NULL) {
            lv_obj_del(visual_alarm_overlay);
            visual_alarm_overlay = NULL;
        }
        if (screen_alarm_snooze != NULL) {
            lv_obj_del(screen_alarm_snooze);
            screen_alarm_snooze = NULL;
        }
    }
}

static void alarm_preview_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Alarm preview back");
        lv_scr_load(screen_alarm_settings);

        if (screen_alarm_preview != NULL) {
            lv_obj_del(screen_alarm_preview);
            screen_alarm_preview = NULL;
        }
    }
}

static void alarm_inactivity_timer_cb(lv_timer_t *timer) {
    lv_obj_t *active = lv_scr_act();
    bool alarm_ui_active =
        (active == screen_alarm_settings) ||
        (active == screen_alarm_detail_editor) ||
        (active == screen_alarm_preview) ||
        (screen_tone_picker != NULL);

    if (!alarm_ui_active) {
        // No alarm UI visible — stop and delete the timer
        lv_timer_del(timer);
        alarm_inactivity_timer = NULL;
        return;
    }

    uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
    if (inactive_ms < ALARM_UI_INACTIVITY_RETURN_MS) {
        return;
    }

    ESP_LOGI(TAG, "Alarm UI inactivity timeout (%lu ms) - returning to home", (unsigned long)inactive_ms);
    nvs_save_alarm_settings(&current_alarm_settings);

    // Clean up tone picker (child of detail editor screen)
    if (screen_tone_picker != NULL) {
        lv_obj_del(screen_tone_picker);
        screen_tone_picker = NULL;
        tone_picker_current_page = 0;
        tone_picker_alarm_ref = NULL;
    }

    // Load home FIRST (never delete active screen before lv_scr_load)
    lv_scr_load(screen_home);

    // Now safe to clean up alarm screens
    if (screen_alarm_preview != NULL) {
        lv_obj_del(screen_alarm_preview);
        screen_alarm_preview = NULL;
    }
    if (screen_alarm_detail_editor != NULL) {
        lv_obj_del(screen_alarm_detail_editor);
        screen_alarm_detail_editor = NULL;
    }
    if (screen_alarm_settings != NULL) {
        lv_obj_del(screen_alarm_settings);
        screen_alarm_settings = NULL;
    }

    editing_alarm = NULL;
    home_screen_active = true;
    pause_background_tasks = false;

    // Delete this timer — it will be recreated when alarm UI is opened again
    lv_timer_del(timer);
    alarm_inactivity_timer = NULL;
}
