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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "ALARM_SCREENS";

// Tone picker pagination
static int tone_picker_current_page = 0;
static alarm_config_t *tone_picker_alarm_ref = NULL;

// Forward declarations for internal event callbacks
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_edit_btn_event_cb(lv_event_t *e);
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
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);
static void alarm_snooze_event_cb(lv_event_t *e);
static void alarm_preview_back_event_cb(lv_event_t *e);

// Public function implementations

void create_alarm_settings_screen(void) {
    ESP_LOGI(TAG, "Creating alarm settings screen - free heap: %lu bytes", esp_get_free_heap_size());

    screen_alarm_settings = lv_obj_create(NULL);
    if (screen_alarm_settings == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm settings screen - out of memory!");
        return;
    }
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
    ESP_LOGI(TAG, "Creating High Alarm card...");
    create_alarm_card(screen_alarm_settings, "High Alarm", &current_alarm_settings.high_alarm,
                      card_y, card_height);
    card_y += card_height + card_spacing;
    vTaskDelay(pdMS_TO_TICKS(1));  // Brief yield to prevent watchdog timeout

    // High Warning
    ESP_LOGI(TAG, "Creating High Warning card...");
    create_alarm_card(screen_alarm_settings, "High Warning", &current_alarm_settings.high_warning,
                      card_y, card_height);
    card_y += card_height + card_spacing;
    vTaskDelay(pdMS_TO_TICKS(1));  // Brief yield to prevent watchdog timeout

    // Low Warning
    ESP_LOGI(TAG, "Creating Low Warning card...");
    create_alarm_card(screen_alarm_settings, "Low Warning", &current_alarm_settings.low_warning,
                      card_y, card_height);
    card_y += card_height + card_spacing;
    vTaskDelay(pdMS_TO_TICKS(1));  // Brief yield to prevent watchdog timeout

    // Low Alarm
    ESP_LOGI(TAG, "Creating Low Alarm card...");
    create_alarm_card(screen_alarm_settings, "Low Alarm", &current_alarm_settings.low_alarm,
                      card_y, card_height);

    ESP_LOGI(TAG, "Alarm settings screen created - free heap: %lu bytes", esp_get_free_heap_size());
}

void create_alarm_card(lv_obj_t *parent, const char *title, alarm_config_t *alarm,
                              int y_pos, int height) {
    if (alarm == NULL) {
        ESP_LOGE(TAG, "create_alarm_card: alarm is NULL!");
        return;
    }
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_alarm_card: parent is NULL!");
        return;
    }

    ESP_LOGI(TAG, "Creating alarm card: %s (heap: %lu)", title, esp_get_free_heap_size());

    // Card container
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        ESP_LOGE(TAG, "Failed to create card container for %s!", title);
        return;
    }

    // Set size first, then align (avoid potential circular layout issues)
    lv_obj_set_width(card, 310);
    lv_obj_set_height(card, height);
    lv_obj_set_pos(card, 5, y_pos);  // Use set_pos instead of align for first card
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_style_pad_all(card, 8, 0);

    // IMPORTANT: Disable scrolling on the card to prevent scrollbars
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Enable/Disable toggle (left)
    lv_obj_t *toggle = lv_switch_create(card);
    if (toggle == NULL) {
        ESP_LOGE(TAG, "Failed to create toggle for %s!", title);
        return;
    }
    lv_obj_set_width(toggle, 40);
    lv_obj_set_height(toggle, 20);
    lv_obj_align(toggle, LV_ALIGN_LEFT_MID, 0, 0);

    // Clear style transitions to prevent animation hangs
    lv_obj_set_style_transition(toggle, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(toggle, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(toggle, NULL, LV_PART_KNOB | LV_STATE_ANY);

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
    if (edit_btn == NULL) {
        ESP_LOGE(TAG, "Failed to create edit button for %s!", title);
        return;
    }
    lv_obj_set_width(edit_btn, 60);
    lv_obj_set_height(edit_btn, 30);
    lv_obj_align(edit_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(edit_btn, &style_card, 0);
    lv_obj_set_style_bg_color(edit_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);

    lv_obj_t *edit_lbl = lv_label_create(edit_btn);
    lv_label_set_text(edit_lbl, "Edit");
    lv_obj_set_style_text_font(edit_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(edit_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(edit_lbl);
    lv_obj_add_event_cb(edit_btn, alarm_edit_btn_event_cb, LV_EVENT_CLICKED, alarm);

    ESP_LOGI(TAG, "Alarm card created: %s", title);
}

void create_tone_picker_screen(alarm_config_t *alarm) {
    // Save alarm reference for pagination
    tone_picker_alarm_ref = alarm;

    // Create overlay matching WiFi overlay style
    screen_tone_picker = lv_obj_create(lv_scr_act());
    if (screen_tone_picker == NULL) {
        ESP_LOGE(TAG, "Failed to create tone picker overlay - LVGL out of memory!");
        return;
    }

    // Style matching existing overlays (wifi_screens.c style)
    lv_obj_set_size(screen_tone_picker, 280, 220);
    lv_obj_center(screen_tone_picker);
    lv_obj_set_style_bg_color(screen_tone_picker, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(screen_tone_picker, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(screen_tone_picker, 2, 0);
    lv_obj_set_style_radius(screen_tone_picker, 12, 0);
    lv_obj_set_style_pad_all(screen_tone_picker, 5, 0);
    lv_obj_clear_flag(screen_tone_picker, LV_OBJ_FLAG_SCROLLABLE);

    // Page content area
    if (tone_picker_current_page == 0) {
        // Page 1: Basic tones - 4 tones
        const char *tone_names[] = {
            "Beep 1 (Single)",
            "Beep 2 (Double)",
            "Beep 3 (Triple)",
            "Chime (Gentle)"
        };

        int y_start = 10;
        int button_height = 38;
        int button_spacing = 8;

        for (int i = 0; i < 4; i++) {
            lv_obj_t *tone_btn = lv_btn_create(screen_tone_picker);
            lv_obj_set_size(tone_btn, 220, button_height);
            lv_obj_align(tone_btn, LV_ALIGN_TOP_MID, 0, y_start + i * (button_height + button_spacing));
            lv_obj_add_style(tone_btn, &style_card, 0);

            // Highlight if this is the current tone
            if (alarm->tone == i) {
                lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
            }

            lv_obj_t *tone_lbl = lv_label_create(tone_btn);
            lv_label_set_text(tone_lbl, tone_names[i]);
            lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_align(tone_lbl, LV_ALIGN_LEFT_MID, 10, 0);

            // Preview button (play icon)
            lv_obj_t *play_btn = lv_btn_create(tone_btn);
            lv_obj_set_size(play_btn, 28, 28);
            lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, -3, 0);
            lv_obj_add_style(play_btn, &style_card, 0);
            lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x00AA00), 0);
            lv_obj_set_style_border_width(play_btn, 0, 0);

            lv_obj_t *play_lbl = lv_label_create(play_btn);
            lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
            lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(play_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_center(play_lbl);
            lv_obj_add_event_cb(play_btn, tone_preview_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            // Select this tone when button clicked
            lv_obj_add_event_cb(tone_btn, tone_select_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    } else if (tone_picker_current_page == 1) {
        // Page 2: Classic melodies - 4 tones
        const char *melody_names[] = {
            "Twinkle Twinkle",
            "Happy Birthday",
            "Mary Had a Little Lamb",
            "Jingle Bells"
        };

        int y_start = 10;
        int button_height = 38;
        int button_spacing = 8;

        for (int i = 0; i < 4; i++) {
            int tone_index = 4 + i;  // Tones 4-7
            lv_obj_t *tone_btn = lv_btn_create(screen_tone_picker);
            lv_obj_set_size(tone_btn, 220, button_height);
            lv_obj_align(tone_btn, LV_ALIGN_TOP_MID, 0, y_start + i * (button_height + button_spacing));
            lv_obj_add_style(tone_btn, &style_card, 0);

            // Highlight if this is the current tone
            if (alarm->tone == tone_index) {
                lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
            }

            lv_obj_t *tone_lbl = lv_label_create(tone_btn);
            lv_label_set_text(tone_lbl, melody_names[i]);
            lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_align(tone_lbl, LV_ALIGN_LEFT_MID, 10, 0);

            // Preview button (play icon)
            lv_obj_t *play_btn = lv_btn_create(tone_btn);
            lv_obj_set_size(play_btn, 28, 28);
            lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, -3, 0);
            lv_obj_add_style(play_btn, &style_card, 0);
            lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x00AA00), 0);
            lv_obj_set_style_border_width(play_btn, 0, 0);

            lv_obj_t *play_lbl = lv_label_create(play_btn);
            lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
            lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(play_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_center(play_lbl);
            lv_obj_add_event_cb(play_btn, tone_preview_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tone_index);

            // Select this tone when button clicked
            lv_obj_add_event_cb(tone_btn, tone_select_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tone_index);
        }
    } else {
        // Page 3: Placeholder
        lv_obj_t *placeholder = lv_label_create(screen_tone_picker);
        lv_label_set_text(placeholder, "More alarms\ncoming soon");
        lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(placeholder, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(placeholder);
    }

    // Vertical pagination buttons on sides
    // Previous button (< - left side, tall and narrow)
    lv_obj_t *prev_btn = lv_btn_create(screen_tone_picker);
    lv_obj_set_size(prev_btn, 20, 200);
    lv_obj_align(prev_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(prev_btn, 6, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);

    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, tone_picker_prev_page_event_cb, LV_EVENT_CLICKED, NULL);

    // Next button (> - right side, tall and narrow)
    lv_obj_t *next_btn = lv_btn_create(screen_tone_picker);
    lv_obj_set_size(next_btn, 20, 200);
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(next_btn, 6, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);

    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, tone_picker_next_page_event_cb, LV_EVENT_CLICKED, NULL);

    // Page indicator at bottom
    lv_obj_t *page_lbl = lv_label_create(screen_tone_picker);
    lv_label_set_text_fmt(page_lbl, "%d / 3", tone_picker_current_page + 1);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -5);
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
    // Create overlay on home screen (matches brightness overlay style)
    screen_alarm_snooze = lv_obj_create(screen_home);
    lv_obj_set_size(screen_alarm_snooze, 280, 200);
    lv_obj_center(screen_alarm_snooze);
    lv_obj_set_style_bg_color(screen_alarm_snooze, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(screen_alarm_snooze, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen_alarm_snooze, 2, 0);
    lv_obj_set_style_border_color(screen_alarm_snooze, lv_color_hex(COLOR_RED), 0);  // Red border for alarm
    lv_obj_set_style_radius(screen_alarm_snooze, 12, 0);
    lv_obj_clear_flag(screen_alarm_snooze, LV_OBJ_FLAG_SCROLLABLE);

    // Title "ALARM" (top left, like brightness overlay)
    lv_obj_t *title = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(title, "ALARM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

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

    // Alarm type (top right, like brightness percentage)
    lv_obj_t *type_label = lv_label_create(screen_alarm_snooze);
    lv_label_set_text(type_label, alarm_type);
    lv_obj_set_style_text_font(type_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(type_label, lv_color_hex(alarm_color), 0);
    lv_obj_align(type_label, LV_ALIGN_TOP_RIGHT, -10, 10);

    // Current glucose value (centered, prominent)
    lv_obj_t *glucose_label = lv_label_create(screen_alarm_snooze);
    char glucose_text[32];
    snprintf(glucose_text, sizeof(glucose_text), "%d mg/dL", current_glucose);
    lv_label_set_text(glucose_label, glucose_text);
    lv_obj_set_style_text_font(glucose_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(glucose_label, lv_color_hex(alarm_color), 0);
    lv_obj_align(glucose_label, LV_ALIGN_TOP_MID, 0, 40);

    // Snooze buttons (4 in a row, centered as a group, closer to glucose value)
    int snooze_times[] = {5, 10, 15, 30};
    int button_width = 55;  // Slightly smaller buttons

    // Calculate x positions relative to center for perfect centering
    // Button centers: -90, -30, +30, +90 from center
    int x_offsets[] = {-90, -30, 30, 90};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(screen_alarm_snooze);
        lv_obj_set_size(btn, button_width, 30);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, x_offsets[i], 90);  // Centered from middle
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B82F6), 0);  // Blue
        lv_obj_set_style_radius(btn, 6, 0);

        lv_obj_t *label = lv_label_create(btn);
        char text[16];
        snprintf(text, sizeof(text), "%d min", snooze_times[i]);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)snooze_times[i]);
    }

    // Dismiss button (bottom, full width)
    lv_obj_t *dismiss_btn = lv_btn_create(screen_alarm_snooze);
    lv_obj_set_size(dismiss_btn, 260, 35);
    lv_obj_align(dismiss_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(dismiss_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_radius(dismiss_btn, 8, 0);

    lv_obj_t *dismiss_label = lv_label_create(dismiss_btn);
    lv_label_set_text(dismiss_label, "Dismiss (3 Hours)");
    lv_obj_set_style_text_font(dismiss_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dismiss_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(dismiss_label);

    lv_obj_add_event_cb(dismiss_btn, alarm_snooze_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)180);  // 180 minutes = 3 hours
}

void create_alarm_detail_editor_screen(alarm_config_t *alarm) {
    ESP_LOGI(TAG, "Creating alarm detail editor screen - free heap: %lu bytes", esp_get_free_heap_size());

    if (alarm == NULL) {
        ESP_LOGE(TAG, "create_alarm_detail_editor_screen: alarm is NULL!");
        return;
    }

    screen_alarm_detail_editor = lv_obj_create(NULL);
    if (screen_alarm_detail_editor == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm detail editor screen - out of memory!");
        ESP_LOGE(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
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

    // Clear style transitions to prevent animation hangs
    lv_obj_set_style_transition(audio_toggle, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(audio_toggle, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(audio_toggle, NULL, LV_PART_KNOB | LV_STATE_ANY);

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
        "Beep 1", "Beep 2", "Beep 3", "Chime",
        "Twinkle Twinkle", "Happy Birthday", "Mary Lamb", "Jingle Bells"
    };
    char tone_text[32];
    int tone_idx = (alarm->tone >= 0 && alarm->tone < 8) ? alarm->tone : 0;
    snprintf(tone_text, sizeof(tone_text), "Tone: %s", tone_names[tone_idx]);

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

    // Clear style transitions to prevent animation hangs
    lv_obj_set_style_transition(visual_toggle, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(visual_toggle, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(visual_toggle, NULL, LV_PART_KNOB | LV_STATE_ANY);

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

    // Clear style transitions to prevent animation hangs
    lv_obj_set_style_transition(repeat_toggle, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(repeat_toggle, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(repeat_toggle, NULL, LV_PART_KNOB | LV_STATE_ANY);

    if (alarm->audio_repeat) {
        lv_obj_add_state(repeat_toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(repeat_toggle, alarm_audio_repeat_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    ESP_LOGI(TAG, "Alarm detail editor screen created successfully - free heap: %lu bytes", esp_get_free_heap_size());
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

static void tone_picker_prev_page_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        tone_picker_current_page--;
        if (tone_picker_current_page < 0) {
            tone_picker_current_page = 2;  // Wrap to last page (page 3)
        }

        ESP_LOGI(TAG, "Tone picker: previous page -> %d", tone_picker_current_page);

        if (lvgl_port_lock(0)) {
            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }

            create_tone_picker_screen(tone_picker_alarm_ref);

            lvgl_port_unlock();
        }
    }
}

static void tone_picker_next_page_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        tone_picker_current_page++;
        if (tone_picker_current_page > 2) {
            tone_picker_current_page = 0;  // Wrap to first page
        }

        ESP_LOGI(TAG, "Tone picker: next page -> %d", tone_picker_current_page);

        if (lvgl_port_lock(0)) {
            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }

            create_tone_picker_screen(tone_picker_alarm_ref);

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

        // Close overlay and refresh detail editor
        if (lvgl_port_lock(0)) {
            // Delete tone picker overlay FIRST before screen transition
            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }

            // Reset pagination
            tone_picker_current_page = 0;
            tone_picker_alarm_ref = NULL;

            // Recreate detail editor to refresh tone button text
            if (screen_alarm_detail_editor != NULL) {
                lv_obj_del(screen_alarm_detail_editor);
                screen_alarm_detail_editor = NULL;
            }

            create_alarm_detail_editor_screen(editing_alarm);

            if (screen_alarm_detail_editor != NULL) {
                lv_scr_load(screen_alarm_detail_editor);
            } else {
                ESP_LOGE(TAG, "Failed to create detail editor screen after tone selection - OOM?");
                ESP_LOGE(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
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

        ESP_LOGI(TAG, "Tone button clicked - opening tone picker overlay");

        if (lvgl_port_lock(0)) {
            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }

            // Reset to first page when opening
            tone_picker_current_page = 0;

            // Create overlay (no lv_scr_load needed - it's an overlay on current screen)
            create_tone_picker_screen(alarm);

            lvgl_port_unlock();
        }
    }
}

static void alarm_snooze_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int snooze_minutes = (int)(intptr_t)lv_event_get_user_data(e);

        // Set snooze time using Unix timestamp (compatible with time())
        time_t now;
        time(&now);
        alarm_snooze_until = now + (snooze_minutes * 60);

        ESP_LOGI(TAG, "Alarm snoozed for %d minutes (until timestamp %lld, current %ld)",
                 snooze_minutes, alarm_snooze_until, (long)now);

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
