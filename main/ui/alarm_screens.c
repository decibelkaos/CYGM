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
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "ALARM_SCREENS";

// Forward declarations for internal event callbacks
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_edit_btn_event_cb(lv_event_t *e);
static void alarm_threshold_slider_event_cb(lv_event_t *e);
static void alarm_volume_slider_event_cb(lv_event_t *e);
static void alarm_audio_toggle_event_cb(lv_event_t *e);
static void alarm_visual_toggle_event_cb(lv_event_t *e);
static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e);
static void alarm_settings_back_event_cb(lv_event_t *e);
static void tone_picker_back_event_cb(lv_event_t *e);
static void tone_select_btn_event_cb(lv_event_t *e);
static void tone_preview_btn_event_cb(lv_event_t *e);
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);
static void alarm_snooze_event_cb(lv_event_t *e);
static void alarm_preview_back_event_cb(lv_event_t *e);

// Public function implementations

void create_alarm_settings_screen(void) {
    screen_alarm_settings = lv_obj_create(NULL);
    lv_obj_add_style(screen_alarm_settings, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_settings);
    lv_label_set_text(header, "Alarm Settings");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_settings);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_settings_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Alarm cards - 4 alarms stacked vertically
    int card_y = 50;
    int card_height = 42;
    int card_spacing = 4;

    // High Alarm
    create_alarm_card(screen_alarm_settings, "High Alarm", &current_alarm_settings.high_alarm,
                      card_y, card_height);
    card_y += card_height + card_spacing;

    // High Warning
    create_alarm_card(screen_alarm_settings, "High Warning", &current_alarm_settings.high_warning,
                      card_y, card_height);
    card_y += card_height + card_spacing;

    // Low Warning
    create_alarm_card(screen_alarm_settings, "Low Warning", &current_alarm_settings.low_warning,
                      card_y, card_height);
    card_y += card_height + card_spacing;

    // Low Alarm
    create_alarm_card(screen_alarm_settings, "Low Alarm", &current_alarm_settings.low_alarm,
                      card_y, card_height);
}

void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm,
                              int y_pos, int height) {
    if (alarm == NULL) {
        ESP_LOGE(TAG, "create_alarm_card: alarm is NULL!");
        return;
    }

    // Card container
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 310, height);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y_pos);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_style_pad_all(card, 8, 0);

    // IMPORTANT: Disable scrolling on the card to prevent scrollbars
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Enable/Disable toggle (left)
    lv_obj_t *toggle = lv_switch_create(card);
    lv_obj_set_size(toggle, 40, 20);
    lv_obj_align(toggle, LV_ALIGN_LEFT_MID, 0, 0);
    if (alarm->enabled) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, alarm_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Title (left-center)
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 50, -8);

    // Threshold value (below title) - displayed in the alarm's configured color
    char threshold_text[32];
    snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", alarm->threshold);
    lv_obj_t *threshold_lbl = lv_label_create(card);
    lv_label_set_text(threshold_lbl, threshold_text);
    lv_obj_set_style_text_font(threshold_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(threshold_lbl, lv_color_hex(alarm->text_color), 0);  // Use alarm color!
    lv_obj_align(threshold_lbl, LV_ALIGN_LEFT_MID, 50, 8);

    // Edit button (right)
    lv_obj_t *edit_btn = lv_btn_create(card);
    lv_obj_set_size(edit_btn, 60, 30);
    lv_obj_align(edit_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(edit_btn, &style_card, 0);
    lv_obj_set_style_bg_color(edit_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);

    lv_obj_t *edit_lbl = lv_label_create(edit_btn);
    lv_label_set_text(edit_lbl, "Edit");
    lv_obj_set_style_text_font(edit_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(edit_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(edit_lbl);
    lv_obj_add_event_cb(edit_btn, alarm_edit_btn_event_cb, LV_EVENT_CLICKED, alarm);
}

void create_tone_picker_screen(alarm_config_t *alarm) {
    screen_tone_picker = lv_obj_create(NULL);
    if (screen_tone_picker == NULL) {
        ESP_LOGE(TAG, "Failed to create tone picker screen - LVGL out of memory!");
        return;
    }
    lv_obj_add_style(screen_tone_picker, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_tone_picker);
    lv_label_set_text(header, "Select Tone");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_tone_picker);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, tone_picker_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Tone options - 5 tones
    const char *tone_names[] = {
        "Beep 1 (Single)",
        "Beep 2 (Double)",
        "Beep 3 (Triple)",
        "Chime (Gentle)",
        "Alert (Urgent)"
    };

    int y_start = 60;
    int button_height = 45;
    int button_spacing = 8;

    for (int i = 0; i < ALARM_TONE_COUNT; i++) {
        lv_obj_t *tone_btn = lv_btn_create(screen_tone_picker);
        lv_obj_set_size(tone_btn, 280, button_height);
        lv_obj_align(tone_btn, LV_ALIGN_TOP_MID, 0, y_start + i * (button_height + button_spacing));
        lv_obj_add_style(tone_btn, &style_card, 0);

        // Highlight if this is the current tone
        if (alarm->tone == i) {
            lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        }

        lv_obj_t *tone_lbl = lv_label_create(tone_btn);
        lv_label_set_text(tone_lbl, tone_names[i]);
        lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(tone_lbl, LV_ALIGN_LEFT_MID, 15, 0);

        // Preview button (play icon)
        lv_obj_t *play_btn = lv_btn_create(tone_btn);
        lv_obj_set_size(play_btn, 35, 35);
        lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, -5, 0);
        lv_obj_add_style(play_btn, &style_card, 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x00AA00), 0);

        lv_obj_t *play_lbl = lv_label_create(play_btn);
        lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(play_lbl);
        lv_obj_add_event_cb(play_btn, tone_preview_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Select this tone when button clicked
        lv_obj_add_event_cb(tone_btn, tone_select_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

void create_alarm_preview_screen(void) {
    screen_alarm_preview = lv_obj_create(NULL);
    lv_obj_add_style(screen_alarm_preview, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_preview);
    lv_label_set_text(header, "Alarm Preview");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_preview);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -5, 3);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_preview_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Dummy home screen preview
    // Show 4 glucose values demonstrating each alarm state
    int card_y = 35;
    int card_height = 45;
    int card_spacing = 5;

    // High Alarm demo
    create_alarm_preview_card(screen_alarm_preview, "High Alarm", 280,
                              &current_alarm_settings.high_alarm,
                              lv_color_hex(current_alarm_settings.high_alarm.text_color),
                              lv_color_hex(current_alarm_settings.high_alarm.text_color));
    card_y += card_height + card_spacing;

    // High Warning demo
    create_alarm_preview_card(screen_alarm_preview, "High Warning", 190,
                              &current_alarm_settings.high_warning,
                              lv_color_hex(current_alarm_settings.high_warning.text_color),
                              lv_color_hex(current_alarm_settings.high_warning.text_color));
    card_y += card_height + card_spacing;

    // Low Warning demo
    create_alarm_preview_card(screen_alarm_preview, "Low Warning", 75,
                              &current_alarm_settings.low_warning,
                              lv_color_hex(current_alarm_settings.low_warning.text_color),
                              lv_color_hex(current_alarm_settings.low_warning.text_color));
    card_y += card_height + card_spacing;

    // Low Alarm demo
    create_alarm_preview_card(screen_alarm_preview, "Low Alarm", 48,
                              &current_alarm_settings.low_alarm,
                              lv_color_hex(current_alarm_settings.low_alarm.text_color),
                              lv_color_hex(current_alarm_settings.low_alarm.text_color));
}

void create_alarm_preview_card(lv_obj_t *parent, const char *title, int glucose_value,
                               alarm_config_t *alarm, lv_color_t color_low, lv_color_t color_high) {
    // Card container
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 310, 45);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 35 + (parent == screen_alarm_preview ? lv_obj_get_child_cnt(parent) - 2 : 0) * 50);
    lv_obj_add_style(card, &style_card, 0);

    // Title
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 10, 5);

    // Glucose value (colored)
    char glucose_text[32];
    snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", glucose_value);
    lv_obj_t *glucose_lbl = lv_label_create(card);
    lv_label_set_text(glucose_lbl, glucose_text);
    lv_obj_set_style_text_font(glucose_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(glucose_lbl, color_low, 0);
    lv_obj_align(glucose_lbl, LV_ALIGN_LEFT_MID, 10, 5);
}

void create_alarm_snooze_screen(void) {
    screen_alarm_snooze = lv_obj_create(NULL);
    lv_obj_add_style(screen_alarm_snooze, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(header, "Alarm Alert");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Current glucose value
    lv_obj_t *glucose_label = lv_label_create(screen_alarm_snooze);
    char glucose_text[32];
    snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", current_glucose);
    lv_label_set_text(glucose_label, glucose_text);
    lv_obj_set_style_text_font(glucose_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(glucose_label, lv_color_hex(COLOR_RED), 0);
    lv_obj_align(glucose_label, LV_ALIGN_TOP_MID, 0, 40);

    // Snooze label
    lv_obj_t *snooze_label = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(snooze_label, "Snooze for:");
    lv_obj_set_style_text_font(snooze_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(snooze_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(snooze_label, LV_ALIGN_TOP_MID, 0, 100);

    // Snooze buttons in a row
    int snooze_times[] = {5, 10, 15, 30};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(screen_alarm_snooze);
        lv_obj_set_size(btn, 70, 35);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10 + (i * 75), 125);
        lv_obj_add_style(btn, &style_card, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x007AFF), 0);  // Blue

        lv_obj_t *label = lv_label_create(btn);
        char text[16];
        snprintf(text, sizeof(text), "%d min", snooze_times[i]);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)snooze_times[i]);
    }

    // Disarm button (snooze for 3 hours)
    lv_obj_t *dismiss_btn = lv_btn_create(screen_alarm_snooze);
    lv_obj_set_size(dismiss_btn, 300, 35);
    lv_obj_align(dismiss_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_style(dismiss_btn, &style_card, 0);
    lv_obj_set_style_bg_color(dismiss_btn, lv_color_hex(COLOR_RED), 0);

    lv_obj_t *dismiss_label = lv_label_create(dismiss_btn);
    lv_label_set_text(dismiss_label, "Disarm All Alarms (3 Hours)");
    lv_obj_set_style_text_font(dismiss_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dismiss_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(dismiss_label);

    lv_obj_add_event_cb(dismiss_btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)180);  // 180 minutes = 3 hours
}

void create_alarm_detail_editor_screen(alarm_config_t *alarm) {
    if (alarm == NULL) {
        ESP_LOGE(TAG, "create_alarm_detail_editor_screen: alarm is NULL!");
        return;
    }

    screen_alarm_detail_editor = lv_obj_create(NULL);
    if (screen_alarm_detail_editor == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm detail editor screen - out of memory?");
        return;
    }
    lv_obj_add_style(screen_alarm_detail_editor, &style_bg, 0);

    // Determine alarm name for header
    const char *alarm_name = "Alarm Configuration";
    if (alarm == &current_alarm_settings.high_alarm) {
        alarm_name = "High Alarm";
    } else if (alarm == &current_alarm_settings.high_warning) {
        alarm_name = "High Warning";
    } else if (alarm == &current_alarm_settings.low_warning) {
        alarm_name = "Low Warning";
    } else if (alarm == &current_alarm_settings.low_alarm) {
        alarm_name = "Low Alarm";
    }

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(header, alarm_name);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button (saves settings)
    lv_obj_t *back_btn = lv_btn_create(screen_alarm_detail_editor);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, alarm_detail_editor_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Threshold Section (Y=50)
    lv_obj_t *threshold_label = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(threshold_label, "Threshold");
    lv_obj_set_style_text_font(threshold_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(threshold_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(threshold_label, LV_ALIGN_TOP_LEFT, 15, 50);

    // Threshold value display
    lv_obj_t *threshold_value = lv_label_create(screen_alarm_detail_editor);
    char threshold_text[16];
    snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", alarm->threshold);
    lv_label_set_text(threshold_value, threshold_text);
    lv_obj_set_style_text_font(threshold_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(threshold_value, lv_color_hex(alarm->text_color), 0);
    lv_obj_align(threshold_value, LV_ALIGN_TOP_RIGHT, -15, 50);

    // Store reference globally for color updates
    alarm_threshold_value_label = threshold_value;

    // Threshold slider (50-400 mg/dL)
    lv_obj_t *threshold_slider = lv_slider_create(screen_alarm_detail_editor);
    lv_obj_set_size(threshold_slider, 290, 10);
    lv_obj_align(threshold_slider, LV_ALIGN_TOP_MID, 0, 75);
    lv_slider_set_range(threshold_slider, 50, 400);
    lv_slider_set_value(threshold_slider, alarm->threshold, LV_ANIM_OFF);
    lv_obj_add_event_cb(threshold_slider, alarm_threshold_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Store threshold value label as user data so slider can update it
    lv_obj_set_user_data(threshold_slider, threshold_value);

    // Audio Settings Section (Y=95, colors are fixed: red for urgent, amber for warnings)
    lv_obj_t *audio_label = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(audio_label, "Audio Alert");
    lv_obj_set_style_text_font(audio_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(audio_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(audio_label, LV_ALIGN_TOP_LEFT, 15, 95);

    // Audio toggle
    lv_obj_t *audio_toggle = lv_switch_create(screen_alarm_detail_editor);
    lv_obj_set_size(audio_toggle, 40, 20);
    lv_obj_align(audio_toggle, LV_ALIGN_TOP_RIGHT, -15, 92);
    if (alarm->audio_enabled) {
        lv_obj_add_state(audio_toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(audio_toggle, alarm_audio_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Tone selection button (Y=120)
    lv_obj_t *tone_btn = lv_btn_create(screen_alarm_detail_editor);
    lv_obj_set_size(tone_btn, 150, 30);
    lv_obj_align(tone_btn, LV_ALIGN_TOP_LEFT, 15, 120);
    lv_obj_add_style(tone_btn, &style_card, 0);
    lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x444444), 0);

    // Get tone name
    const char *tone_names[] = {
        "Beep 1", "Beep 2", "Beep 3", "Chime", "Alert"
    };
    char tone_text[32];
    snprintf(tone_text, sizeof(tone_text), "Tone: %s", tone_names[alarm->tone]);

    lv_obj_t *tone_lbl = lv_label_create(tone_btn);
    lv_label_set_text(tone_lbl, tone_text);
    lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(tone_lbl);
    lv_obj_add_event_cb(tone_btn, alarm_tone_btn_event_cb, LV_EVENT_CLICKED, alarm);

    // Volume label and value
    lv_obj_t *volume_label = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(volume_label, "Volume");
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(volume_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(volume_label, LV_ALIGN_TOP_LEFT, 175, 120);

    lv_obj_t *volume_value = lv_label_create(screen_alarm_detail_editor);
    char volume_text[8];
    snprintf(volume_text, sizeof(volume_text), "%d%%", alarm->volume);
    lv_label_set_text(volume_value, volume_text);
    lv_obj_set_style_text_font(volume_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(volume_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(volume_value, LV_ALIGN_TOP_RIGHT, -15, 120);

    // Volume slider (Y=140)
    lv_obj_t *volume_slider = lv_slider_create(screen_alarm_detail_editor);
    lv_obj_set_size(volume_slider, 135, 8);
    lv_obj_align(volume_slider, LV_ALIGN_TOP_RIGHT, -15, 140);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, alarm->volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, alarm_volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Store volume value label as user data
    lv_obj_set_user_data(volume_slider, volume_value);

    // Visual Alert Section (Y=180) - kept at original position
    lv_obj_t *visual_label = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(visual_label, "Visual Alert");
    lv_obj_set_style_text_font(visual_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(visual_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(visual_label, LV_ALIGN_TOP_LEFT, 15, 180);

    // Visual toggle
    lv_obj_t *visual_toggle = lv_switch_create(screen_alarm_detail_editor);
    lv_obj_set_size(visual_toggle, 40, 20);
    lv_obj_align(visual_toggle, LV_ALIGN_TOP_RIGHT, -15, 177);
    if (alarm->visual_enabled) {
        lv_obj_add_state(visual_toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(visual_toggle, alarm_visual_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Audio Repeat Section (Y=205) - moved below Visual Alert
    lv_obj_t *repeat_label = lv_label_create(screen_alarm_detail_editor);
    lv_label_set_text(repeat_label, "Repeat Until Dismissed");
    lv_obj_set_style_text_font(repeat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(repeat_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(repeat_label, LV_ALIGN_TOP_LEFT, 30, 207);

    // Audio repeat toggle
    lv_obj_t *repeat_toggle = lv_switch_create(screen_alarm_detail_editor);
    lv_obj_set_size(repeat_toggle, 40, 20);
    lv_obj_align(repeat_toggle, LV_ALIGN_TOP_RIGHT, -15, 204);
    if (alarm->audio_repeat) {
        lv_obj_add_state(repeat_toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(repeat_toggle, alarm_audio_repeat_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);
}

// Event handler implementations

static void alarm_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Alarm enabled: %d", alarm->enabled);
}

static void alarm_edit_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
        if (alarm == NULL) {
            ESP_LOGE(TAG, "alarm_edit_btn_event_cb: alarm is NULL!");
            return;
        }

        ESP_LOGI(TAG, "Edit alarm clicked - threshold: %d, color: 0x%06X", alarm->threshold, alarm->text_color);
        ESP_LOGI(TAG, "Free heap before screen delete: %lu bytes", esp_get_free_heap_size());

        // Open detailed alarm editor screen
        editing_alarm = alarm;

        // No need to lock - we're already in LVGL event callback context
        if (screen_alarm_detail_editor != NULL) {
            ESP_LOGI(TAG, "Deleting old detail editor screen");
            lv_obj_del(screen_alarm_detail_editor);
            screen_alarm_detail_editor = NULL;
        }

        ESP_LOGI(TAG, "Free heap before screen create: %lu bytes", esp_get_free_heap_size());
        create_alarm_detail_editor_screen(alarm);

        if (screen_alarm_detail_editor != NULL) {
            ESP_LOGI(TAG, "Loading detail editor screen");
            lv_scr_load(screen_alarm_detail_editor);

            // Now safe to delete alarm settings screen (no longer active)
            if (screen_alarm_settings != NULL) {
                lv_obj_del(screen_alarm_settings);
                screen_alarm_settings = NULL;
            }
        } else {
            ESP_LOGE(TAG, "Failed to create detail editor screen!");
        }
    }
}

static void alarm_threshold_slider_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_threshold_slider_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    alarm->threshold = (int)value;

    // Update threshold value label
    lv_obj_t *threshold_value = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (threshold_value != NULL) {
        char threshold_text[16];
        snprintf(threshold_text, sizeof(threshold_text), "%d mg/dL", (int)value);
        lv_label_set_text(threshold_value, threshold_text);
    }

    ESP_LOGI(TAG, "Threshold changed: %d mg/dL", (int)value);
}

static void alarm_volume_slider_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_volume_slider_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    alarm->volume = (uint8_t)value;

    // Update volume value label
    lv_obj_t *volume_value = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (volume_value != NULL) {
        char volume_text[8];
        snprintf(volume_text, sizeof(volume_text), "%d%%", (int)value);
        lv_label_set_text(volume_value, volume_text);
    }

    // Volume changed (don't log - too frequent)
}

static void alarm_audio_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_audio_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->audio_enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Audio enabled: %d", alarm->audio_enabled);
}

static void alarm_visual_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_visual_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->visual_enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Visual enabled: %d", alarm->visual_enabled);
}

static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_audio_repeat_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->audio_repeat = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Audio repeat enabled: %d", alarm->audio_repeat);
}

static void alarm_settings_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back to menu - saving alarm settings");
        nvs_save_alarm_settings(&current_alarm_settings);

        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}

static void tone_picker_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Tone picker back - returning to detail editor");

        if (lvgl_port_lock(0)) {
            if (screen_alarm_detail_editor != NULL) {
                lv_scr_load(screen_alarm_detail_editor);
            }

            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void tone_select_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int tone_index = (int)(intptr_t)lv_event_get_user_data(e);

        if (editing_alarm != NULL) {
            editing_alarm->tone = tone_index;
            ESP_LOGI(TAG, "Tone selected: %d", tone_index);
        }

        // Return to detail editor
        if (lvgl_port_lock(0)) {
            if (screen_alarm_detail_editor != NULL) {
                lv_obj_del(screen_alarm_detail_editor);
                screen_alarm_detail_editor = NULL;
            }

            create_alarm_detail_editor_screen(editing_alarm);

            if (screen_alarm_detail_editor != NULL) {
                lv_scr_load(screen_alarm_detail_editor);
            }

            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

static void tone_preview_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int tone_index = (int)(intptr_t)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Preview tone: %d", tone_index);

        // Play the tone using buzzer
        play_alarm_tone(tone_index, 100);  // Play at 100% volume for preview
    }
}

static void alarm_detail_editor_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Detail editor back - saving and returning to alarm settings");
        nvs_save_alarm_settings(&current_alarm_settings);

        if (lvgl_port_lock(0)) {
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
            lvgl_port_unlock();
        }

        editing_alarm = NULL;
    }
}

static void alarm_tone_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);

        ESP_LOGI(TAG, "Tone button clicked - opening tone picker");

        if (lvgl_port_lock(0)) {
            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }

            create_tone_picker_screen(alarm);

            if (screen_tone_picker != NULL) {
                lv_scr_load(screen_tone_picker);
            }
            lvgl_port_unlock();
        }
    }
}

static void alarm_snooze_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int snooze_minutes = (int)(intptr_t)lv_event_get_user_data(e);
        ESP_LOGI(TAG, "Alarm snoozed for %d minutes", snooze_minutes);

        // Set snooze time
        alarm_snooze_until = esp_timer_get_time() / 1000000 + (snooze_minutes * 60);

        // Mark alarm as acknowledged
        alarm_acknowledged = true;

        // Stop visual alarm if active
        if (visual_alarm_active) {
            visual_alarm_active = false;
        }

        // Always dismiss the snooze screen and return to home
        if (lvgl_port_lock(0)) {
            // IMPORTANT: Load new screen BEFORE deleting current screen
            lv_scr_load(screen_home);

            // Now safe to delete the old screens
            if (visual_alarm_overlay != NULL) {
                lv_obj_del(visual_alarm_overlay);
                visual_alarm_overlay = NULL;
            }
            if (screen_alarm_snooze != NULL) {
                lv_obj_del(screen_alarm_snooze);
                screen_alarm_snooze = NULL;
            }

            lvgl_port_unlock();
        }
    }
}

static void alarm_preview_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Alarm preview back");

        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_alarm_settings);

            if (screen_alarm_preview != NULL) {
                lv_obj_del(screen_alarm_preview);
                screen_alarm_preview = NULL;
            }
            lvgl_port_unlock();
        }
    }
}
