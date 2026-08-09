/*
 * alarm_screens.c - alarm thresholds, tone picker, preview, and the paged
 * advanced alert options editor.
 */

#include "alarm_screens.h"
#include "shared_state.h"
#include "main.h"
#include "hardware/buzzer.h"
#include "hardware/screenshot.h"
#include "features/time_system.h"
#include "nvs_config.h"
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

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *alarm_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 4, 4);
    cygm_apply_ghost_btn(btn);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(lbl);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// Tone picker pagination
static int tone_picker_current_page = 0;
static alarm_config_t *tone_picker_alarm_ref = NULL;
// Non-NULL while the picker is choosing the data-gap tone in the alarm
// extension blob instead of a per-tier alarm tone.
static uint8_t *tone_picker_ext_ref = NULL;

// Both tables are indexed by alarm_tone_t: a new tone must be added at the same
// position in each. The static asserts below catch a missed one at build time.
static const char *tone_display_names[] = {
    "Beep 1 (Single)", "Beep 2 (Double)", "Beep 3 (Triple)", "Chime (Gentle)",
    "Twinkle Twinkle", "Fur Elise", "Dixie Horn", "When the Saints",
    "Siren (Urgent)", "Ascending Scale", "Doorbell", "Big Ben (Westminster)",
    "Heartbeat (Soft)", "Marimba Run", "Harp Arpeggio", "Sonar Ping",
    "Cuckoo Call", "Trill (Fast)", "Morse SOS", "Train Horn",
    "Pager Burst", "Sweep Siren", "Klaxon Honk", "Rapid Pulse",
    "Random (Loud)", "Church Bell", "Xylophone", "Alert (Triple)"
};
static const char *tone_short_names[] = {
    "Beep 1", "Beep 2", "Beep 3", "Chime",
    "Twinkle", "Fur Elise", "Dixie", "Saints",
    "Siren", "Ascending", "Doorbell", "Big Ben",
    "Heartbeat", "Marimba", "Harp", "Sonar",
    "Cuckoo", "Trill", "SOS", "Train",
    "Pager", "Sweep", "Klaxon", "Rapid",
    "Random", "Bell", "Xylo", "Alert"
};
#define TONES_PER_PAGE 4
#define TONE_PAGES 7

// Header shown under the picker title, one per page.
static const char *tone_page_categories[TONE_PAGES] = {
    "Basic Tones", "Melodies", "Alerts & Signals",
    "Gentle Tones", "Rhythms & Calls", "Urgent Alerts",
    "More Tones"
};

// Picker slot -> tone. The enum order is frozen because every index is
// persisted in NVS; this table is the only thing that decides display order,
// so a tone can be moved in the picker without repointing a saved alarm.
static const alarm_tone_t tone_display_order[] = {
    // Page 1 — Basic Tones
    ALARM_TONE_BEEP_1, ALARM_TONE_BEEP_2, ALARM_TONE_BEEP_3, ALARM_TONE_RANDOM,
    // Page 2 — Melodies
    ALARM_TONE_TWINKLE, ALARM_TONE_FUR_ELISE, ALARM_TONE_DIXIE_HORN, ALARM_TONE_SAINTS,
    // Page 3 — Alerts & Signals
    ALARM_TONE_SIREN, ALARM_TONE_ASCENDING, ALARM_TONE_DOORBELL, ALARM_TONE_BIG_BEN,
    // Page 4 — Gentle Tones
    ALARM_TONE_HEARTBEAT, ALARM_TONE_MARIMBA, ALARM_TONE_HARP, ALARM_TONE_SONAR,
    // Page 5 — Rhythms & Calls
    ALARM_TONE_CUCKOO, ALARM_TONE_TRILL, ALARM_TONE_SOS, ALARM_TONE_TRAIN,
    // Page 6 — Urgent Alerts
    ALARM_TONE_PAGER, ALARM_TONE_SWEEP, ALARM_TONE_KLAXON, ALARM_TONE_RAPID_PULSE,
    // Page 7 — More Tones
    ALARM_TONE_CHIME, ALARM_TONE_CHURCH_BELL, ALARM_TONE_XYLOPHONE, ALARM_TONE_ALERT_TRIPLE,
};

_Static_assert(sizeof(tone_display_names) / sizeof(tone_display_names[0]) == ALARM_TONE_COUNT,
               "tone_display_names must stay index-aligned with alarm_tone_t");
_Static_assert(sizeof(tone_short_names) / sizeof(tone_short_names[0]) == ALARM_TONE_COUNT,
               "tone_short_names must stay index-aligned with alarm_tone_t");
_Static_assert(sizeof(tone_display_order) / sizeof(tone_display_order[0]) == ALARM_TONE_COUNT,
               "tone_display_order must list every tone exactly once");
_Static_assert(TONES_PER_PAGE * TONE_PAGES == ALARM_TONE_COUNT,
               "tone picker pages must cover every tone exactly once");
_Static_assert(ALARM_TONE_COUNT <= 32,
               "tone_display_order_validate() tracks tones in a 32-bit mask");

// Length alone does not prove the display order is a permutation — a duplicated
// tone would silently hide another from the UI. Runs once, on first picker build.
static void tone_display_order_validate(void) {
    static bool checked = false;
    if (checked) return;
    checked = true;

    uint32_t seen = 0;
    for (int slot = 0; slot < ALARM_TONE_COUNT; slot++) {
        int t = (int)tone_display_order[slot];
        if (t < 0 || t >= ALARM_TONE_COUNT) {
            ESP_LOGE(TAG, "tone_display_order[%d] = %d is out of range", slot, t);
            continue;
        }
        if (seen & (1u << t)) {
            ESP_LOGE(TAG, "tone_display_order lists tone %d twice", t);
        }
        seen |= (1u << t);
    }

    uint32_t all = (1u << ALARM_TONE_COUNT) - 1u;
    if (seen != all) {
        ESP_LOGE(TAG, "tone_display_order misses tone(s): mask 0x%08lx of 0x%08lx",
                 (unsigned long)seen, (unsigned long)all);
    }
}

// Picker slot -> stored tone value. Out-of-range slots fall back to the first
// tone rather than reading past the table.
static alarm_tone_t tone_from_slot(int slot) {
    if (slot < 0 || slot >= ALARM_TONE_COUNT) return ALARM_TONE_BEEP_1;
    return tone_display_order[slot];
}

// Forward declarations for internal event callbacks
static void alarm_toggle_event_cb(lv_event_t *e);
static void alarm_card_click_event_cb(lv_event_t *e);
static void alarm_threshold_slider_event_cb(lv_event_t *e);
static void alarm_volume_slider_event_cb(lv_event_t *e);
static void alarm_audio_toggle_event_cb(lv_event_t *e);
static void alarm_visual_toggle_event_cb(lv_event_t *e);
static void alarm_led_toggle_event_cb(lv_event_t *e);
static void alarm_audio_repeat_toggle_event_cb(lv_event_t *e);
static void alarm_settings_back_event_cb(lv_event_t *e);
static void tone_picker_prev_page_event_cb(lv_event_t *e);
static void tone_picker_next_page_event_cb(lv_event_t *e);
static void tone_select_btn_event_cb(lv_event_t *e);
static void tone_preview_btn_event_cb(lv_event_t *e);
static void tone_picker_close_event_cb(lv_event_t *e);
static void alarm_detail_editor_back_event_cb(lv_event_t *e);
static void alarm_tone_btn_event_cb(lv_event_t *e);
static void alarm_preview_back_event_cb(lv_event_t *e);
static void alarm_inactivity_timer_cb(lv_timer_t *timer);

// Advanced alert options (versioned alarm-extension blob) — see the section
// further down for the paged screen these belong to.
static void alarm_options_open_event_cb(lv_event_t *e);
static void alarm_options_close(bool return_to_settings);
static void alarm_ext_flush(void);
static void alarm_options_clear_refs(void);
static void time_picker_close(void);

// Strips the theme styles first: the default theme animates the knob via a
// style transition, and animations freeze this hardware.
static void alarm_style_switch(lv_obj_t *sw, int w, int h) {
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, w, h);
    int radius = h / 2;

    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_PRESSED), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, radius, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_PRESSED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, radius, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
}

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

// Every screen this module owns NULLs its own pointer on delete: the inactivity
// watchdog frees these from another task, so a pointer cleared only at the call
// site goes stale and the next create() orphans a 4-7KB screen in the pool.
static void alarm_settings_screen_delete_cb(lv_event_t *e) {
    (void)e;
    screen_alarm_settings = NULL;
}

void create_alarm_settings_screen(void) {
    ESP_LOGI(TAG, "Creating alarm settings screen - free heap: %lu bytes", esp_get_free_heap_size());

    // Enforce off-home resource policy while in alarm UI
    home_screen_active = false;
    pause_background_tasks = true;

    if (alarm_inactivity_timer == NULL) {
        alarm_inactivity_timer = lv_timer_create(alarm_inactivity_timer_cb, 1000, NULL);
    }

    // Idempotent: never build a second screen on top of a live one.
    if (screen_alarm_settings != NULL) {
        if (screen_alarm_settings == lv_scr_act()) {
            ESP_LOGW(TAG, "Alarm settings screen already loaded - reusing");
            return;
        }
        ESP_LOGW(TAG, "Stale alarm settings screen - freeing before rebuild");
        lv_obj_del(screen_alarm_settings);
        screen_alarm_settings = NULL;
    }

    screen_alarm_settings = lv_obj_create(NULL);
    if (screen_alarm_settings == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm settings screen - out of memory!");
        return;
    }
    lv_obj_add_event_cb(screen_alarm_settings, alarm_settings_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_style(screen_alarm_settings, &style_bg, 0);
    lv_obj_clear_flag(screen_alarm_settings, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header (y=0-30) ----
    lv_obj_t *header = lv_label_create(screen_alarm_settings);
    lv_label_set_text(header, "Alarms");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 6);

    alarm_std_back_btn(screen_alarm_settings, alarm_settings_back_event_cb, NULL);

    // Advanced alert options (quiet hours, escalation, data gap, predictive)
    lv_obj_t *opts_btn = lv_btn_create(screen_alarm_settings);
    lv_obj_set_size(opts_btn, CYGM_BTN_W_ICON, CYGM_BTN_H);
    lv_obj_align(opts_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    cygm_apply_ghost_btn(opts_btn);
    lv_obj_t *opts_lbl = lv_label_create(opts_btn);
    lv_label_set_text(opts_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(opts_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(opts_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(opts_lbl);
    lv_obj_add_event_cb(opts_btn, alarm_options_open_event_cb, LV_EVENT_CLICKED, NULL);

    // ---- Glucose Range Bar (screen bottom, y=228..236) ----
    // Parked on the bottom edge, clear of the header buttons, so it runs the
    // full 270px. W*H is fixed by the static buffer below.
    #define RANGE_BAR_W 270
    #define RANGE_BAR_H 8
    #define RANGE_MIN   40
    #define RANGE_MAX   400
    #define RANGE_BAR_BOTTOM_GAP 4   // Bezel margin under the bar
    static lv_color_t range_bar_buf[RANGE_BAR_W * RANGE_BAR_H];

    lv_obj_t *range_canvas = lv_canvas_create(screen_alarm_settings);
    lv_canvas_set_buffer(range_canvas, range_bar_buf, RANGE_BAR_W, RANGE_BAR_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(range_canvas, LV_ALIGN_BOTTOM_MID, 0, -RANGE_BAR_BOTTOM_GAP);
    lv_canvas_fill_bg(range_canvas, lv_color_hex(COLOR_BG), LV_OPA_COVER);

    // Compute pixel positions for each threshold
    int low_alarm_th   = current_alarm_settings.low_alarm.threshold;
    int low_warn_th    = current_alarm_settings.low_warning.threshold;
    int high_warn_th   = current_alarm_settings.high_warning.threshold;
    int high_alarm_th  = current_alarm_settings.high_alarm.threshold;

    // Map threshold to pixel x within the bar
    #define THRESH_TO_PX(val) ((int)(((val) - RANGE_MIN) * RANGE_BAR_W / (RANGE_MAX - RANGE_MIN)))

    int px_low_alarm  = THRESH_TO_PX(low_alarm_th);
    int px_low_warn   = THRESH_TO_PX(low_warn_th);
    int px_high_warn  = THRESH_TO_PX(high_warn_th);
    int px_high_alarm = THRESH_TO_PX(high_alarm_th);

    // Clamp pixel positions
    if (px_low_alarm < 0) px_low_alarm = 0;
    if (px_low_warn < px_low_alarm) px_low_warn = px_low_alarm;
    if (px_high_warn < px_low_warn) px_high_warn = px_low_warn;
    if (px_high_alarm < px_high_warn) px_high_alarm = px_high_warn;
    if (px_high_alarm > RANGE_BAR_W) px_high_alarm = RANGE_BAR_W;

    // Draw colored zones
    lv_draw_rect_dsc_t zone_dsc;

    // Zone 1: RED (40 to low_alarm)
    lv_draw_rect_dsc_init(&zone_dsc);
    zone_dsc.bg_color = lv_color_hex(COLOR_RED);
    zone_dsc.bg_opa = LV_OPA_COVER;
    zone_dsc.radius = 0;
    if (px_low_alarm > 0) {
        lv_canvas_draw_rect(range_canvas, 0, 0, px_low_alarm, RANGE_BAR_H, &zone_dsc);
    }

    // Zone 2: ORANGE (low_alarm to low_warning)
    lv_draw_rect_dsc_init(&zone_dsc);
    zone_dsc.bg_color = lv_color_hex(COLOR_ORANGE);
    zone_dsc.bg_opa = LV_OPA_COVER;
    zone_dsc.radius = 0;
    if (px_low_warn > px_low_alarm) {
        lv_canvas_draw_rect(range_canvas, px_low_alarm, 0, px_low_warn - px_low_alarm, RANGE_BAR_H, &zone_dsc);
    }

    // Zone 3: GREEN (low_warning to high_warning) — safe zone
    lv_draw_rect_dsc_init(&zone_dsc);
    zone_dsc.bg_color = lv_color_hex(COLOR_GREEN);
    zone_dsc.bg_opa = LV_OPA_COVER;
    zone_dsc.radius = 0;
    if (px_high_warn > px_low_warn) {
        lv_canvas_draw_rect(range_canvas, px_low_warn, 0, px_high_warn - px_low_warn, RANGE_BAR_H, &zone_dsc);
    }

    // Zone 4: YELLOW (high_warning to high_alarm)
    lv_draw_rect_dsc_init(&zone_dsc);
    zone_dsc.bg_color = lv_color_hex(COLOR_YELLOW);
    zone_dsc.bg_opa = LV_OPA_COVER;
    zone_dsc.radius = 0;
    if (px_high_alarm > px_high_warn) {
        lv_canvas_draw_rect(range_canvas, px_high_warn, 0, px_high_alarm - px_high_warn, RANGE_BAR_H, &zone_dsc);
    }

    // Zone 5: RED (high_alarm to 400)
    lv_draw_rect_dsc_init(&zone_dsc);
    zone_dsc.bg_color = lv_color_hex(COLOR_RED);
    zone_dsc.bg_opa = LV_OPA_COVER;
    zone_dsc.radius = 0;
    if (RANGE_BAR_W > px_high_alarm) {
        lv_canvas_draw_rect(range_canvas, px_high_alarm, 0, RANGE_BAR_W - px_high_alarm, RANGE_BAR_H, &zone_dsc);
    }

    // Threshold labels sit one row ABOVE the bar; below it they land on the
    // bezel. Centring each on its own tick is not enough — close thresholds
    // overlap — so lay the row out left to right enforcing TH_LBL_GAP, then
    // walk it back from the right edge so nothing runs off-screen.
    #define TH_LBL_Y      214  // 11px tall, 3px above the bar at y=228
    #define TH_LBL_GAP    10   // ~1.7 digit widths — enough to read as separate
    #define TH_DIGIT_W    6    // montserrat_10 digit advance
    int bar_left_x = (320 - RANGE_BAR_W) / 2;  // Left edge of the bar in screen coords

    const int th_val[4] = { low_alarm_th, low_warn_th, high_warn_th, high_alarm_th };
    const int th_px[4]  = { px_low_alarm, px_low_warn, px_high_warn, px_high_alarm };
    char thresh_buf[4][12];
    int th_w[4];
    int th_x[4];

    for (int i = 0; i < 4; i++) {
        int n = snprintf(thresh_buf[i], sizeof(thresh_buf[i]), "%d", th_val[i]);
        th_w[i] = ((n > 0) ? n : 2) * TH_DIGIT_W;
        th_x[i] = bar_left_x + th_px[i] - th_w[i] / 2;
    }

    for (int i = 1; i < 4; i++) {
        int min_x = th_x[i - 1] + th_w[i - 1] + TH_LBL_GAP;
        if (th_x[i] < min_x) th_x[i] = min_x;
    }

    int right_limit = bar_left_x + RANGE_BAR_W - th_w[3];
    if (th_x[3] > right_limit) th_x[3] = right_limit;
    for (int i = 2; i >= 0; i--) {
        int max_x = th_x[i + 1] - th_w[i] - TH_LBL_GAP;
        if (th_x[i] > max_x) th_x[i] = max_x;
    }
    if (th_x[0] < 2) th_x[0] = 2;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *th_lbl = lv_label_create(screen_alarm_settings);
        lv_label_set_text(th_lbl, thresh_buf[i]);
        lv_obj_set_style_text_font(th_lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(th_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_pos(th_lbl, th_x[i], TH_LBL_Y);
    }

    // ---- Alarm cards with section grouping ----
    // The stack runs y=42..208: clear of the header buttons at the top and 6px
    // above the threshold label row at the bottom. Two section labels and four
    // cards have to fit in that 166px, which is what caps ALARM_CARD_H.
    #define ALARM_CARD_W     290
    #define ALARM_CARD_H     32
    #define ALARM_SECT_Y     42                                   // First section label
    #define ALARM_ROW_1_Y    (ALARM_SECT_Y + 14)                  // 56
    #define ALARM_ROW_PITCH  (ALARM_CARD_H + 3)                   // 35
    #define ALARM_ROW_2_Y    (ALARM_ROW_1_Y + ALARM_ROW_PITCH)    // 91
    #define ALARM_SECT_2_Y   (ALARM_ROW_2_Y + ALARM_CARD_H + 4)   // 127
    #define ALARM_ROW_3_Y    (ALARM_SECT_2_Y + 14)                // 141
    #define ALARM_ROW_4_Y    (ALARM_ROW_3_Y + ALARM_ROW_PITCH)    // 176

    lv_obj_t *high_section = lv_label_create(screen_alarm_settings);
    lv_label_set_text(high_section, "HIGH ALERTS");
    lv_obj_set_style_text_font(high_section, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(high_section, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(high_section, 2, 0);
    lv_obj_align(high_section, LV_ALIGN_TOP_RIGHT, -18, ALARM_SECT_Y);

    create_alarm_card(screen_alarm_settings, "High Alarm", &current_alarm_settings.high_alarm, ALARM_ROW_1_Y, ALARM_CARD_H);
    create_alarm_card(screen_alarm_settings, "High Warning", &current_alarm_settings.high_warning, ALARM_ROW_2_Y, ALARM_CARD_H);

    lv_obj_t *low_section = lv_label_create(screen_alarm_settings);
    lv_label_set_text(low_section, "LOW ALERTS");
    lv_obj_set_style_text_font(low_section, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(low_section, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(low_section, 2, 0);
    lv_obj_align(low_section, LV_ALIGN_TOP_RIGHT, -18, ALARM_SECT_2_Y);

    create_alarm_card(screen_alarm_settings, "Low Warning", &current_alarm_settings.low_warning, ALARM_ROW_3_Y, ALARM_CARD_H);
    create_alarm_card(screen_alarm_settings, "Low Alarm", &current_alarm_settings.low_alarm, ALARM_ROW_4_Y, ALARM_CARD_H);

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
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

    // Deliberately NOT routed through cygm_apply_ghost_btn: the cards stack 4px
    // apart, so growing each ext_click_area would overlap adjacent alarms. At
    // this size they already clear the minimum touch target on both axes.
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
    cygm_format_threshold(alarm->threshold, threshold_text, sizeof(threshold_text));
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
    alarm_style_switch(sw, 40, 20);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -22, 0);

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
    lv_obj_clear_flag(screen_alarm_detail_editor, LV_OBJ_FLAG_SCROLLABLE);

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

    alarm_std_back_btn(screen_alarm_detail_editor, alarm_detail_editor_back_event_cb, NULL);

    // ---- Card stack ----
    // The back button's touch box grows to y=48, so the first card starts at 44
    // and its first control sits below that. The cards have nothing to tap, so
    // their CLICKABLE flag comes off — left on, a card sits above the back
    // button in z-order and swallows taps landing in its grown box.
    #define AED_CARD_W        290
    #define AED_CARD_GAP        6
    #define AED_THRESH_Y       44
    #define AED_THRESH_H       54
    #define AED_AUDIO_Y  (AED_THRESH_Y + AED_THRESH_H + AED_CARD_GAP)   // 104
    #define AED_AUDIO_H        78
    #define AED_VISUAL_Y (AED_AUDIO_Y + AED_AUDIO_H + AED_CARD_GAP)     // 188
    #define AED_VISUAL_H       44
    // Shared inner column: content runs x=16..278 inside every card.
    #define AED_PAD_L          16
    #define AED_ROW_W         262

    // ---- Threshold Card ----
    lv_obj_t *thresh_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(thresh_card, AED_CARD_W, AED_THRESH_H);
    lv_obj_align(thresh_card, LV_ALIGN_TOP_MID, 0, AED_THRESH_Y);
    lv_obj_set_style_bg_color(thresh_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(thresh_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thresh_card, 12, 0);
    lv_obj_set_style_border_width(thresh_card, 0, 0);
    lv_obj_set_style_pad_all(thresh_card, 0, 0);
    lv_obj_clear_flag(thresh_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(thresh_card, LV_OBJ_FLAG_CLICKABLE);

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
    cygm_format_threshold(alarm->threshold, threshold_text, sizeof(threshold_text));
    lv_label_set_text(thresh_value, threshold_text);
    lv_obj_set_style_text_font(thresh_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(thresh_value, lv_color_hex(alarm->text_color), 0);
    lv_obj_align(thresh_value, LV_ALIGN_TOP_RIGHT, -12, 6);  // Baseline-matched to "Threshold"
    alarm_threshold_value_label = thresh_value;

    // Threshold slider
    lv_obj_t *thresh_slider = lv_slider_create(thresh_card);
    lv_obj_set_size(thresh_slider, AED_ROW_W, 12);
    lv_obj_align(thresh_slider, LV_ALIGN_TOP_LEFT, AED_PAD_L, 32);
    // In mmol mode the slider operates in tenths-of-mmol so each knob step is a
    // clean 0.1 mmol/L; the stored alarm->threshold stays canonical mg/dL.
    if (user_glucose_mmol) {
        lv_slider_set_range(thresh_slider, mgdl_to_mmol_tenths(50), mgdl_to_mmol_tenths(400));
        lv_slider_set_value(thresh_slider, mgdl_to_mmol_tenths(alarm->threshold), LV_ANIM_OFF);
    } else {
        lv_slider_set_range(thresh_slider, 50, 400);
        lv_slider_set_value(thresh_slider, alarm->threshold, LV_ANIM_OFF);
    }

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
    // NO knob shadow — shadows cause intermittent freezes on this hardware.

    lv_obj_set_user_data(thresh_slider, thresh_value);
    lv_obj_add_event_cb(thresh_slider, alarm_threshold_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // ---- Audio Card ----
    lv_obj_t *audio_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(audio_card, AED_CARD_W, AED_AUDIO_H);
    lv_obj_align(audio_card, LV_ALIGN_TOP_MID, 0, AED_AUDIO_Y);
    lv_obj_set_style_bg_color(audio_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(audio_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(audio_card, 12, 0);
    lv_obj_set_style_border_width(audio_card, 0, 0);
    lv_obj_set_style_pad_all(audio_card, 0, 0);
    lv_obj_clear_flag(audio_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(audio_card, LV_OBJ_FLAG_CLICKABLE);

    // Left accent bar (blue)
    lv_obj_t *audio_accent = lv_obj_create(audio_card);
    lv_obj_remove_style_all(audio_accent);
    lv_obj_set_size(audio_accent, 3, 58);
    lv_obj_align(audio_accent, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(audio_accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(audio_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(audio_accent, 2, 0);

    // Audio Alert label + switch (row 1, y=4)
    lv_obj_t *audio_label = lv_label_create(audio_card);
    lv_label_set_text(audio_label, "Audio Alert");
    lv_obj_set_style_text_font(audio_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(audio_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(audio_label, LV_ALIGN_TOP_LEFT, 16, 4);

    lv_obj_t *audio_sw = lv_switch_create(audio_card);
    alarm_style_switch(audio_sw, 36, 16);
    lv_obj_align(audio_sw, LV_ALIGN_TOP_RIGHT, -12, 4);
    if (alarm->audio_enabled) lv_obj_add_state(audio_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(audio_sw, alarm_audio_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Divider line
    lv_obj_t *audio_div = lv_obj_create(audio_card);
    lv_obj_remove_style_all(audio_div);
    lv_obj_set_size(audio_div, AED_ROW_W, 1);
    lv_obj_align(audio_div, LV_ALIGN_TOP_LEFT, AED_PAD_L, 22);
    lv_obj_set_style_bg_color(audio_div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(audio_div, LV_OPA_COVER, 0);

    // Row 2: Tone button (left) + Volume slider (right) on same line (y=26)
    int tone_idx = (alarm->tone >= 0 && alarm->tone < ALARM_TONE_COUNT) ? alarm->tone : 0;
    char tone_text[32];
    snprintf(tone_text, sizeof(tone_text), "%s", tone_short_names[tone_idx]);

    lv_obj_t *tone_btn = lv_btn_create(audio_card);
    lv_obj_set_size(tone_btn, 130, 24);
    lv_obj_align(tone_btn, LV_ALIGN_TOP_LEFT, AED_PAD_L, 26);
    lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_bg_opa(tone_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tone_btn, 0, 0);
    lv_obj_update_layout(tone_btn);
    cygm_apply_ghost_btn(tone_btn);

    lv_obj_t *tone_lbl = lv_label_create(tone_btn);
    lv_label_set_text(tone_lbl, tone_text);
    lv_obj_set_style_text_font(tone_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tone_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(tone_lbl);
    lv_obj_add_event_cb(tone_btn, alarm_tone_btn_event_cb, LV_EVENT_CLICKED, alarm);

    // Volume slider: 10px clear of the tone button and vertically centred on it.
    lv_obj_t *vol_slider = lv_slider_create(audio_card);
    lv_obj_set_size(vol_slider, 90, 10);
    lv_obj_align(vol_slider, LV_ALIGN_TOP_LEFT, 156, 33);
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
    // NO knob shadow — see threshold slider note (shadows freeze this hardware).

    // Volume percentage label (right of slider)
    lv_obj_t *vol_value = lv_label_create(audio_card);
    char vol_text[8];
    snprintf(vol_text, sizeof(vol_text), "%d%%", alarm->volume);
    lv_label_set_text(vol_value, vol_text);
    lv_obj_set_style_text_font(vol_value, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(vol_value, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(vol_value, LV_ALIGN_TOP_RIGHT, -12, 32);  // Centred on the slider, in the switch column

    lv_obj_set_user_data(vol_slider, vol_value);
    lv_obj_add_event_cb(vol_slider, alarm_volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Divider before repeat
    lv_obj_t *repeat_div = lv_obj_create(audio_card);
    lv_obj_remove_style_all(repeat_div);
    lv_obj_set_size(repeat_div, AED_ROW_W, 1);
    lv_obj_align(repeat_div, LV_ALIGN_TOP_LEFT, AED_PAD_L, 52);
    lv_obj_set_style_bg_color(repeat_div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(repeat_div, LV_OPA_COVER, 0);

    // Repeat label + switch (row 3, y=56)
    lv_obj_t *repeat_label = lv_label_create(audio_card);
    lv_label_set_text(repeat_label, "Repeat");
    lv_obj_set_style_text_font(repeat_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(repeat_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(repeat_label, LV_ALIGN_TOP_LEFT, 16, 56);

    lv_obj_t *repeat_sw = lv_switch_create(audio_card);
    alarm_style_switch(repeat_sw, 36, 16);
    lv_obj_align(repeat_sw, LV_ALIGN_TOP_RIGHT, -12, 56);  // Row-centred with the Repeat label
    if (alarm->audio_repeat) lv_obj_add_state(repeat_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(repeat_sw, alarm_audio_repeat_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // ---- Visual Card ----
    lv_obj_t *visual_card = lv_obj_create(screen_alarm_detail_editor);
    lv_obj_set_size(visual_card, AED_CARD_W, AED_VISUAL_H);
    lv_obj_align(visual_card, LV_ALIGN_TOP_MID, 0, AED_VISUAL_Y);
    lv_obj_set_style_bg_color(visual_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(visual_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(visual_card, 12, 0);
    lv_obj_set_style_border_width(visual_card, 0, 0);
    lv_obj_set_style_pad_all(visual_card, 0, 0);
    lv_obj_clear_flag(visual_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(visual_card, LV_OBJ_FLAG_CLICKABLE);

    // Left accent bar (green)
    lv_obj_t *vis_accent = lv_obj_create(visual_card);
    lv_obj_remove_style_all(vis_accent);
    lv_obj_set_size(vis_accent, 3, 30);
    lv_obj_align(vis_accent, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(vis_accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(vis_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vis_accent, 2, 0);

    // Screen Flash label + switch
    lv_obj_t *screen_label = lv_label_create(visual_card);
    lv_label_set_text(screen_label, "Screen Flash");
    lv_obj_set_style_text_font(screen_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(screen_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(screen_label, LV_ALIGN_TOP_LEFT, 16, 3);

    lv_obj_t *vis_sw = lv_switch_create(visual_card);
    alarm_style_switch(vis_sw, 36, 16);
    lv_obj_align(vis_sw, LV_ALIGN_TOP_RIGHT, -12, 3);
    if (alarm->visual_enabled) lv_obj_add_state(vis_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(vis_sw, alarm_visual_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    // Divider line
    lv_obj_t *vis_div = lv_obj_create(visual_card);
    lv_obj_remove_style_all(vis_div);
    lv_obj_set_size(vis_div, AED_ROW_W, 1);
    lv_obj_align(vis_div, LV_ALIGN_TOP_LEFT, AED_PAD_L, 22);
    lv_obj_set_style_bg_color(vis_div, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(vis_div, LV_OPA_COVER, 0);

    // LED Flash label + switch
    lv_obj_t *led_label = lv_label_create(visual_card);
    lv_label_set_text(led_label, "LED Flash");
    lv_obj_set_style_text_font(led_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(led_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(led_label, LV_ALIGN_TOP_LEFT, 16, 25);

    lv_obj_t *led_sw = lv_switch_create(visual_card);
    alarm_style_switch(led_sw, 36, 16);
    lv_obj_align(led_sw, LV_ALIGN_TOP_RIGHT, -12, 25);
    if (alarm->led_enabled) lv_obj_add_state(led_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(led_sw, alarm_led_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

    ESP_LOGI(TAG, "Alarm detail editor screen created - free heap: %lu bytes", esp_get_free_heap_size());
}

// ==================== Tone Picker (Modal Overlay) ====================

// The picker is parented to the live screen, so deleting that screen takes the
// overlay with it. Only the pointer is cleared: paging deletes and rebuilds the
// overlay, then reads the page index and owner refs back.
static void tone_picker_delete_cb(lv_event_t *e) {
    (void)e;
    screen_tone_picker = NULL;
}

void create_tone_picker_screen(alarm_config_t *alarm) {
    tone_picker_alarm_ref = alarm;
    tone_display_order_validate();

    // Dimmed backdrop (fullscreen, clickable to dismiss)
    screen_tone_picker = lv_obj_create(lv_scr_act());
    if (screen_tone_picker == NULL) {
        ESP_LOGE(TAG, "Failed to create tone picker overlay!");
        return;
    }
    lv_obj_add_event_cb(screen_tone_picker, tone_picker_delete_cb, LV_EVENT_DELETE, NULL);
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
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // A ring plus a full-size glyph, so the control reads as a 34px target
    // rather than a loose 9px mark; the local border and radius survive the
    // shared ghost style.
    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 34, 34);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 17, 0);
    lv_obj_update_layout(close_btn);   // resolve the size so the helper can size the touch box
    cygm_apply_ghost_btn(close_btn);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, tone_picker_close_event_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, (tone_picker_ext_ref != NULL) ? "Data Gap Tone" : "Select Tone");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

    // Category label (changes per page)
    int page = tone_picker_current_page;
    if (page < 0 || page >= TONE_PAGES) page = 0;
    const char *category = tone_page_categories[page];
    lv_obj_t *cat_lbl = lv_label_create(card);
    lv_label_set_text(cat_lbl, category);
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cat_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(cat_lbl, LV_ALIGN_TOP_LEFT, 16, 32);

    // The picker serves two owners: a per-tier alarm_config_t, or the data-gap
    // tone in the alarm-extension blob. tone_picker_ext_ref decides which.
    int selected_tone = (tone_picker_ext_ref != NULL) ? (int)*tone_picker_ext_ref
                      : (alarm != NULL) ? (int)alarm->tone : -1;

    // Rows are laid out by picker SLOT, but only the stored enum value from
    // tone_display_order reaches the callbacks or the checkmark test.
    int start_slot = page * TONES_PER_PAGE;
    int end_slot = start_slot + TONES_PER_PAGE;
    if (end_slot > ALARM_TONE_COUNT) end_slot = ALARM_TONE_COUNT;

    #define TONE_ROW_Y_START  46
    #define TONE_ROW_H        33
    #define TONE_ROW_GAP      4
    #define TONE_ROW_W        266

    for (int slot = start_slot; slot < end_slot; slot++) {
        int tone_id = (int)tone_from_slot(slot);
        int row_idx = slot - start_slot;
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
        if (selected_tone == tone_id) {
            lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        } else {
            lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_PRESSED), 0);
        }
        lv_obj_set_style_bg_color(tone_btn, lv_color_hex(COLOR_PRESSED), LV_STATE_PRESSED);

        // Checkmark for selected item
        if (selected_tone == tone_id) {
            lv_obj_t *check = lv_label_create(tone_btn);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(check, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_align(check, LV_ALIGN_LEFT_MID, 0, 0);
        }

        // Tone name
        lv_obj_t *tone_name = lv_label_create(tone_btn);
        lv_label_set_text(tone_name, tone_display_names[tone_id]);
        lv_obj_set_style_text_font(tone_name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(tone_name, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(tone_name, LV_ALIGN_LEFT_MID, (selected_tone == tone_id) ? 22 : 0, 0);

        // Preview play button (green circle). Its enlarged touch box is clipped
        // to the row it lives in, so it cannot steal taps from the row below.
        lv_obj_t *play_btn = lv_btn_create(tone_btn);
        lv_obj_set_size(play_btn, 30, 28);
        lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1a9a4a), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(play_btn, 0, 0);
        lv_obj_update_layout(play_btn);
        cygm_apply_ghost_btn(play_btn);

        lv_obj_t *play_lbl = lv_label_create(play_btn);
        lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(play_lbl);
        lv_obj_add_event_cb(play_btn, tone_preview_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tone_id);

        // Select tone on row click. The callbacks receive the stored enum value,
        // never the slot, so nothing downstream knows about display order.
        lv_obj_add_event_cb(tone_btn, tone_select_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tone_id);
    }

    // --- Bottom navigation bar ---

    // Prev arrow
    lv_obj_t *prev_btn = lv_btn_create(card);
    lv_obj_set_size(prev_btn, 44, 26);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 16, -4);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(COLOR_PRESSED), 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_update_layout(prev_btn);
    cygm_apply_ghost_btn(prev_btn);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, tone_picker_prev_page_event_cb, LV_EVENT_CLICKED, NULL);

    // Page indicator (centered)
    char page_text[16];
    snprintf(page_text, sizeof(page_text), "%d / %d", page + 1, TONE_PAGES);
    lv_obj_t *page_lbl = lv_label_create(card);
    lv_label_set_text(page_lbl, page_text);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Next arrow
    lv_obj_t *next_btn = lv_btn_create(card);
    lv_obj_set_size(next_btn, 44, 26);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -16, -4);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(COLOR_PRESSED), 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_update_layout(next_btn);
    cygm_apply_ghost_btn(next_btn);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, tone_picker_next_page_event_cb, LV_EVENT_CLICKED, NULL);
}

// ==================== Alarm Preview Screen ====================

static void alarm_preview_screen_delete_cb(lv_event_t *e) {
    (void)e;
    screen_alarm_preview = NULL;
}

void create_alarm_preview_screen(void) {
    // Idempotent — see alarm_settings_screen_delete_cb for why.
    if (screen_alarm_preview != NULL) {
        if (screen_alarm_preview == lv_scr_act()) {
            ESP_LOGW(TAG, "Alarm preview screen already loaded - reusing");
            return;
        }
        ESP_LOGW(TAG, "Stale alarm preview screen - freeing before rebuild");
        lv_obj_del(screen_alarm_preview);
        screen_alarm_preview = NULL;
    }

    screen_alarm_preview = lv_obj_create(NULL);
    if (screen_alarm_preview == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm preview screen - out of memory!");
        return;
    }
    lv_obj_add_event_cb(screen_alarm_preview, alarm_preview_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_style(screen_alarm_preview, &style_bg, 0);
    lv_obj_clear_flag(screen_alarm_preview, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header = lv_label_create(screen_alarm_preview);
    lv_label_set_text(header, "Alarm Preview");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    alarm_std_back_btn(screen_alarm_preview, alarm_preview_back_event_cb, NULL);

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
    // 48 + 47*idx ends the fourth card at 231, clear of the bottom bezel —
    // any taller and the screen becomes scrollable.
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48 + card_idx * 47);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title (left)
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    // Glucose value (right, colored) — localized unit
    char glucose_text[16];
    cygm_format_threshold(glucose_value, glucose_text, sizeof(glucose_text));
    lv_obj_t *glucose_lbl = lv_label_create(card);
    lv_label_set_text(glucose_lbl, glucose_text);
    lv_obj_set_style_text_font(glucose_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(glucose_lbl, color_low, 0);
    lv_obj_align(glucose_lbl, LV_ALIGN_RIGHT_MID, -12, 0);
}

// ==================== Advanced Alert Options ====================
//
// Editor for the versioned alarm-extension blob (cygm_alarm_ext_t, cgm_types.h).
// Fields are edited in place on the global the engine reads live, then written
// back via nvs_save_alarm_ext() when the user leaves a page or the screen.
// Paged rather than scrolled: scroll momentum can schedule style transitions,
// and animations freeze this hardware.

#define ALARM_OPT_PAGES     5
#define ALARM_OPT_ROW_H     36
#define ALARM_OPT_ROW_STEP  40
#define ALARM_OPT_ROW_W    290

static lv_obj_t *screen_alarm_options = NULL;
static lv_obj_t *alarm_options_page_body = NULL;      // holds only the current page's rows
static lv_obj_t *alarm_options_page_header = NULL;    // section name, retitled per page
static lv_obj_t *alarm_options_page_counter = NULL;   // "n / N"
static int  alarm_options_page = 0;
static bool alarm_ext_dirty = false;

// Row value labels that outlive their event callbacks. Cleared whenever the
// page body is rebuilt so a callback can never write to a freed label.
static lv_obj_t *quiet_start_value_lbl = NULL;
static lv_obj_t *quiet_end_value_lbl = NULL;
static lv_obj_t *gap_tone_value_lbl = NULL;

// ---- Stepper table: one callback drives every +/- pair ----

typedef enum {
    OPT_FMT_MIN = 0,   // "20 min"
    OPT_FMT_PCT,       // "70%"
    OPT_FMT_RATE,      // tenths -> "3.0 mg/dL/min"
    OPT_FMT_GLUCOSE,   // mg/dL -> localized unit
} opt_fmt_t;

typedef struct {
    uint8_t  *field;
    uint8_t   min;
    uint8_t   max;
    uint8_t   step;
    opt_fmt_t fmt;
    lv_obj_t *value_lbl;
} opt_stepper_t;

enum {
    OPT_SNOOZE = 0,
    OPT_ESC_STEP,
    OPT_ESC_MAXVOL,
    OPT_SUPPRESS,
    OPT_GAP_MIN,
    OPT_GAP_VOL,
    OPT_PREDICT_HORIZON,
    OPT_RATE,
    OPT_URGENT_FLOOR,
    OPT_STEPPER_COUNT
};

// Ranges mirror the field comments in cgm_types.h; the loader clamps to the
// same bounds, so the UI can never author a value the engine would reject.
static opt_stepper_t opt_steppers[OPT_STEPPER_COUNT] = {
    [OPT_SNOOZE]          = { &alarm_ext_settings.snooze_default_min,   5, 120,  5, OPT_FMT_MIN,     NULL },
    [OPT_ESC_STEP]        = { &alarm_ext_settings.escalate_step_min,    1,  30,  1, OPT_FMT_MIN,     NULL },
    [OPT_ESC_MAXVOL]      = { &alarm_ext_settings.escalate_max_volume, 10, 100, 10, OPT_FMT_PCT,     NULL },
    [OPT_SUPPRESS]        = { &alarm_ext_settings.suppress_min,         5, 120,  5, OPT_FMT_MIN,     NULL },
    [OPT_GAP_MIN]         = { &alarm_ext_settings.gap_minutes,         10,  60,  5, OPT_FMT_MIN,     NULL },
    [OPT_GAP_VOL]         = { &alarm_ext_settings.gap_volume,           0, 100, 10, OPT_FMT_PCT,     NULL },
    [OPT_PREDICT_HORIZON] = { &alarm_ext_settings.predict_horizon_min,  5,  45,  5, OPT_FMT_MIN,     NULL },
    [OPT_RATE]            = { &alarm_ext_settings.rate_threshold_x10,  10,  60,  1, OPT_FMT_RATE,    NULL },
    [OPT_URGENT_FLOOR]    = { &alarm_ext_settings.urgent_low_floor,    40,  90,  5, OPT_FMT_GLUCOSE, NULL },
};

static void opt_stepper_event_cb(lv_event_t *e);
static void opt_switch_event_cb(lv_event_t *e);
static void alarm_options_back_event_cb(lv_event_t *e);
static void alarm_options_prev_page_event_cb(lv_event_t *e);
static void alarm_options_next_page_event_cb(lv_event_t *e);
static void quiet_start_btn_event_cb(lv_event_t *e);
static void quiet_end_btn_event_cb(lv_event_t *e);
static void gap_tone_btn_event_cb(lv_event_t *e);
static void time_picker_step_event_cb(lv_event_t *e);
static void time_picker_done_event_cb(lv_event_t *e);
static void alarm_options_build_page(void);

static void alarm_ext_flush(void) {
    if (!alarm_ext_dirty) return;
    alarm_ext_dirty = false;
    esp_err_t err = nvs_save_alarm_ext(&alarm_ext_settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save alarm extension settings: %s", esp_err_to_name(err));
    }
}

static void alarm_options_clear_refs(void) {
    for (int i = 0; i < OPT_STEPPER_COUNT; i++) {
        opt_steppers[i].value_lbl = NULL;
    }
    quiet_start_value_lbl = NULL;
    quiet_end_value_lbl = NULL;
    gap_tone_value_lbl = NULL;
}

// Values are bounded here as well as clamped on edit so GCC can prove the
// format strings below never truncate (-Werror=format-truncation).
static void opt_format_value(const opt_stepper_t *s, char *buf, size_t len) {
    int v = (int)*s->field;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    switch (s->fmt) {
        case OPT_FMT_PCT:
            snprintf(buf, len, "%d%%", v);
            break;
        case OPT_FMT_RATE:
            // "/min" only: the full "mg/dL/min" does not fit the ~74px value
            // zone and runs into the minus stepper.
            snprintf(buf, len, "%d.%d/min", v / 10, v % 10);
            break;
        case OPT_FMT_GLUCOSE:
            cygm_format_threshold(v, buf, len);
            break;
        case OPT_FMT_MIN:
        default:
            snprintf(buf, len, "%d min", v);
            break;
    }
}

// Quiet-hours values and the time picker's readout both come through here, so
// both follow the user's 12/24h preference. The stored fields are 24h and come
// back from NVS unvalidated, hence the clamp. Callers must pass >= 7 bytes.
static void format_hhmm(char *buf, size_t len, uint8_t hour, uint8_t minute) {
    uint8_t h = (hour > 23) ? 23 : hour;
    uint8_t m = (minute > 59) ? 59 : minute;
    cygm_format_clock(buf, len, h, m);
}

// ---- Row builders ----

static lv_obj_t *opt_row_create(int row, const char *title, const char *subtitle) {
    lv_obj_t *card = lv_obj_create(alarm_options_page_body);
    lv_obj_set_size(card, ALARM_OPT_ROW_W, ALARM_OPT_ROW_H);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, row * ALARM_OPT_ROW_STEP);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 14, -7);

    lv_obj_t *sub_lbl = lv_label_create(card);
    lv_label_set_text(sub_lbl, subtitle);
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(sub_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(sub_lbl, LV_ALIGN_LEFT_MID, 14, 9);

    return card;
}

static void opt_row_add_switch(lv_obj_t *row, uint8_t *field) {
    lv_obj_t *sw = lv_switch_create(row);
    alarm_style_switch(sw, 40, 20);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -14, 0);
    if (*field) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, opt_switch_event_cb, LV_EVENT_VALUE_CHANGED, field);
}

// Minus and plus sit 82px apart so their enlarged touch boxes cannot overlap.
static void opt_row_add_stepper(lv_obj_t *row, int idx) {
    opt_stepper_t *s = &opt_steppers[idx];

    lv_obj_t *minus = lv_btn_create(row);
    lv_obj_set_size(minus, 30, 26);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -118, 0);
    lv_obj_update_layout(minus);
    cygm_apply_ghost_btn(minus);
    lv_obj_t *minus_lbl = lv_label_create(minus);
    lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(minus_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(minus_lbl);
    lv_obj_add_event_cb(minus, opt_stepper_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(idx << 1));

    lv_obj_t *plus = lv_btn_create(row);
    lv_obj_set_size(plus, 30, 26);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_update_layout(plus);
    cygm_apply_ghost_btn(plus);
    lv_obj_t *plus_lbl = lv_label_create(plus);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(plus_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(plus_lbl);
    lv_obj_add_event_cb(plus, opt_stepper_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)((idx << 1) | 1));

    char value_text[40];
    opt_format_value(s, value_text, sizeof(value_text));
    lv_obj_t *value_lbl = lv_label_create(row);
    lv_label_set_text(value_lbl, value_text);
    lv_obj_set_style_text_font(value_lbl,
        (s->fmt == OPT_FMT_RATE) ? &lv_font_montserrat_10 : &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(value_lbl, LV_ALIGN_RIGHT_MID, -42, 0);
    s->value_lbl = value_lbl;
}

static lv_obj_t *opt_row_add_action(lv_obj_t *row, const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_size(btn, 96, 28);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_update_layout(btn);
    cygm_apply_ghost_btn(btn);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return lbl;
}

// ---- Page content ----

static const char *alarm_options_page_title(int page) {
    switch (page) {
        case 0:  return "QUIET HOURS";
        case 1:  return "ESCALATION";
        case 2:  return "DATA GAP";
        case 3:  return "PREDICTIVE";
        default: return "SAFETY FLOOR";
    }
}

static void alarm_options_build_page(void) {
    if (alarm_options_page_body == NULL) return;

    alarm_options_clear_refs();
    lv_obj_clean(alarm_options_page_body);

    char buf[16];

    switch (alarm_options_page) {
        case 0: {
            lv_obj_t *r = opt_row_create(0, "Quiet Hours", "silence non-urgent");
            opt_row_add_switch(r, &alarm_ext_settings.quiet_enabled);

            r = opt_row_create(1, "Start", "window begins");
            format_hhmm(buf, sizeof(buf),
                        alarm_ext_settings.quiet_start_hour, alarm_ext_settings.quiet_start_min);
            quiet_start_value_lbl = opt_row_add_action(r, buf, quiet_start_btn_event_cb);

            r = opt_row_create(2, "End", "window ends");
            format_hhmm(buf, sizeof(buf),
                        alarm_ext_settings.quiet_end_hour, alarm_ext_settings.quiet_end_min);
            quiet_end_value_lbl = opt_row_add_action(r, buf, quiet_end_btn_event_cb);

            r = opt_row_create(3, "Snooze", "default length");
            opt_row_add_stepper(r, OPT_SNOOZE);
            break;
        }
        case 1: {
            lv_obj_t *r = opt_row_create(0, "Escalate", "louder if ignored");
            opt_row_add_switch(r, &alarm_ext_settings.escalate_enabled);

            r = opt_row_create(1, "Step", "between increases");
            opt_row_add_stepper(r, OPT_ESC_STEP);

            r = opt_row_create(2, "Max Volume", "escalation ceiling");
            opt_row_add_stepper(r, OPT_ESC_MAXVOL);

            r = opt_row_create(3, "Repeat After", "re-alert delay");
            opt_row_add_stepper(r, OPT_SUPPRESS);
            break;
        }
        case 2: {
            lv_obj_t *r = opt_row_create(0, "Gap Alert", "warn when data stops");
            opt_row_add_switch(r, &alarm_ext_settings.gap_enabled);

            r = opt_row_create(1, "Gap After", "no reading for");
            opt_row_add_stepper(r, OPT_GAP_MIN);

            r = opt_row_create(2, "Gap Tone", "gap alert sound");
            int gap_tone = alarm_ext_settings.gap_tone;
            if (gap_tone < 0 || gap_tone >= ALARM_TONE_COUNT) gap_tone = ALARM_TONE_ASCENDING;
            gap_tone_value_lbl = opt_row_add_action(r, tone_short_names[gap_tone], gap_tone_btn_event_cb);

            r = opt_row_create(3, "Gap Volume", "gap alert loudness");
            opt_row_add_stepper(r, OPT_GAP_VOL);
            break;
        }
        case 3: {
            lv_obj_t *r = opt_row_create(0, "Predict Low", "warn before it hits");
            opt_row_add_switch(r, &alarm_ext_settings.predict_enabled);

            r = opt_row_create(1, "Horizon", "look-ahead window");
            opt_row_add_stepper(r, OPT_PREDICT_HORIZON);

            r = opt_row_create(2, "Rate Alert", "fast rise or fall");
            opt_row_add_switch(r, &alarm_ext_settings.rate_enabled);

            r = opt_row_create(3, "Rate", "trigger change speed");
            opt_row_add_stepper(r, OPT_RATE);
            break;
        }
        default: {
            lv_obj_t *r = opt_row_create(0, "Urgent Low", "always-on safety floor");
            opt_row_add_stepper(r, OPT_URGENT_FLOOR);

            // No switch here by design: the urgent-low guard cannot be disabled.
            lv_obj_t *note = lv_label_create(alarm_options_page_body);
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(note, 274);
            lv_label_set_text(note,
                "The urgent low alert always sounds at full volume - through snooze, "
                "quiet hours and muted alarms. It cannot be turned off.");
            lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_GRAY), 0);
            lv_obj_align(note, LV_ALIGN_TOP_MID, 0, ALARM_OPT_ROW_STEP + 6);

            char eff[32];
            cygm_format_threshold(cygm_urgent_low_threshold(), eff, sizeof(eff));
            char eff_text[72];
            snprintf(eff_text, sizeof(eff_text), "Sounding below %s right now", eff);
            lv_obj_t *eff_lbl = lv_label_create(alarm_options_page_body);
            lv_label_set_text(eff_lbl, eff_text);
            lv_obj_set_style_text_font(eff_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(eff_lbl, lv_color_hex(COLOR_ORANGE), 0);
            lv_obj_align(eff_lbl, LV_ALIGN_TOP_MID, 0, ALARM_OPT_ROW_STEP + 62);
            break;
        }
    }
}

// ---- Screen ----

// screen_alarm_options is file-static, so the inactivity watchdog's reclaim
// table cannot reach it — this callback is the only thing keeping the pointer
// and its child references honest when the screen dies.
static void alarm_options_screen_delete_cb(lv_event_t *e) {
    (void)e;
    alarm_options_clear_refs();
    alarm_options_page_body = NULL;
    alarm_options_page_header = NULL;
    alarm_options_page_counter = NULL;
    screen_alarm_options = NULL;
}

static void create_alarm_options_screen(void) {
    ESP_LOGI(TAG, "Creating alarm options screen - free heap: %lu bytes", esp_get_free_heap_size());

    // Idempotent — see alarm_settings_screen_delete_cb for why.
    if (screen_alarm_options != NULL) {
        if (screen_alarm_options == lv_scr_act()) {
            ESP_LOGW(TAG, "Alert options screen already loaded - reusing");
            return;
        }
        ESP_LOGW(TAG, "Stale alert options screen - freeing before rebuild");
        lv_obj_del(screen_alarm_options);
        screen_alarm_options = NULL;
    }

    screen_alarm_options = lv_obj_create(NULL);
    if (screen_alarm_options == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm options screen - out of memory!");
        return;
    }
    lv_obj_add_event_cb(screen_alarm_options, alarm_options_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_style(screen_alarm_options, &style_bg, 0);
    lv_obj_clear_flag(screen_alarm_options, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_label_create(screen_alarm_options);
    // Keep this title short: a longer one runs into the top-right page caption.
    lv_label_set_text(header, "Alerts");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 8);

    alarm_std_back_btn(screen_alarm_options, alarm_options_back_event_cb, NULL);

    lv_obj_t *cat_lbl = lv_label_create(screen_alarm_options);
    lv_label_set_text(cat_lbl, alarm_options_page_title(alarm_options_page));
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cat_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cat_lbl, 2, 0);
    lv_obj_align(cat_lbl, LV_ALIGN_TOP_RIGHT, -12, 14);

    // Only the rows live in the body, so a page change rebuilds four cards
    // rather than a whole screen — largest-free-block is the scarce resource.
    alarm_options_page_body = lv_obj_create(screen_alarm_options);
    lv_obj_remove_style_all(alarm_options_page_body);
    lv_obj_set_size(alarm_options_page_body, 300, 160);
    lv_obj_align(alarm_options_page_body, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_clear_flag(alarm_options_page_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev_btn = lv_btn_create(screen_alarm_options);
    lv_obj_set_size(prev_btn, 44, 28);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 14, -6);
    lv_obj_update_layout(prev_btn);
    cygm_apply_ghost_btn(prev_btn);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, alarm_options_prev_page_event_cb, LV_EVENT_CLICKED, NULL);

    char page_text[16];
    snprintf(page_text, sizeof(page_text), "%d / %d", alarm_options_page + 1, ALARM_OPT_PAGES);
    lv_obj_t *page_lbl = lv_label_create(screen_alarm_options);
    lv_label_set_text(page_lbl, page_text);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t *next_btn = lv_btn_create(screen_alarm_options);
    lv_obj_set_size(next_btn, 44, 28);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -14, -6);
    lv_obj_update_layout(next_btn);
    cygm_apply_ghost_btn(next_btn);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, alarm_options_next_page_event_cb, LV_EVENT_CLICKED, NULL);

    alarm_options_page_header = cat_lbl;
    alarm_options_page_counter = page_lbl;
    alarm_options_build_page();

    ESP_LOGI(TAG, "Alarm options screen created - free heap: %lu bytes", esp_get_free_heap_size());
}

static void alarm_options_goto_page(int page) {
    if (page < 0) page = ALARM_OPT_PAGES - 1;
    if (page >= ALARM_OPT_PAGES) page = 0;
    alarm_options_page = page;

    alarm_ext_flush();
    alarm_options_build_page();

    if (alarm_options_page_header != NULL) {
        lv_label_set_text(alarm_options_page_header, alarm_options_page_title(alarm_options_page));
    }
    if (alarm_options_page_counter != NULL) {
        char page_text[16];
        snprintf(page_text, sizeof(page_text), "%d / %d", alarm_options_page + 1, ALARM_OPT_PAGES);
        lv_label_set_text(alarm_options_page_counter, page_text);
    }
}

// Tear the options screen down. Callers that still want the alarm list back on
// screen pass true; the inactivity path passes false and loads home itself.
static void alarm_options_close(bool return_to_settings) {
    time_picker_close();
    alarm_ext_flush();
    alarm_options_clear_refs();
    alarm_options_page_body = NULL;
    alarm_options_page_header = NULL;
    alarm_options_page_counter = NULL;
    alarm_options_page = 0;

    if (screen_tone_picker != NULL) {
        lv_obj_del(screen_tone_picker);
        screen_tone_picker = NULL;
        tone_picker_current_page = 0;
        tone_picker_alarm_ref = NULL;
        tone_picker_ext_ref = NULL;
    }

    if (return_to_settings) {
        if (screen_alarm_settings != NULL) {
            lv_obj_del(screen_alarm_settings);
            screen_alarm_settings = NULL;
        }
        create_alarm_settings_screen();
        if (screen_alarm_settings != NULL) {
            lv_scr_load(screen_alarm_settings);
        }
    }

    if (screen_alarm_options != NULL) {
        lv_obj_del(screen_alarm_options);
        screen_alarm_options = NULL;
    }
}

// ---- Time picker (quiet-hours window) ----

static lv_obj_t *time_picker_overlay = NULL;
static uint8_t  *time_picker_hour_field = NULL;
static uint8_t  *time_picker_min_field = NULL;
static lv_obj_t *time_picker_big_lbl = NULL;
static lv_obj_t *time_picker_row_lbl = NULL;

static void time_picker_refresh_labels(void) {
    if (time_picker_hour_field == NULL || time_picker_min_field == NULL) return;
    char buf[16];
    format_hhmm(buf, sizeof(buf), *time_picker_hour_field, *time_picker_min_field);
    if (time_picker_big_lbl != NULL) lv_label_set_text(time_picker_big_lbl, buf);
    if (time_picker_row_lbl != NULL) lv_label_set_text(time_picker_row_lbl, buf);
}

// The overlay is parented to the live screen, so deleting that screen takes it
// down without going through time_picker_close(); drop every session ref here.
static void time_picker_overlay_delete_cb(lv_event_t *e) {
    (void)e;
    time_picker_overlay = NULL;
    time_picker_hour_field = NULL;
    time_picker_min_field = NULL;
    time_picker_big_lbl = NULL;
    time_picker_row_lbl = NULL;
}

static void time_picker_close(void) {
    if (time_picker_overlay != NULL) {
        lv_obj_del(time_picker_overlay);   // delete cb clears every static below
    }
    time_picker_overlay = NULL;
    time_picker_hour_field = NULL;
    time_picker_min_field = NULL;
    time_picker_big_lbl = NULL;
    time_picker_row_lbl = NULL;
}

// Hour and minute rows sit 52px apart so the enlarged hit boxes the shared
// helper adds cannot reach into the row above or below.
#define TIME_PICKER_BTN_W    34
#define TIME_PICKER_BTN_H    30
#define TIME_PICKER_HOUR_Y   80
#define TIME_PICKER_MIN_Y   132

static void time_picker_add_step_btn(lv_obj_t *parent, const char *symbol,
                                     int x_offset, int y_offset, int ud) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, TIME_PICKER_BTN_W, TIME_PICKER_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, x_offset, y_offset);
    lv_obj_update_layout(btn);
    cygm_apply_ghost_btn(btn);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, time_picker_step_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);
}

static void open_time_picker(const char *title, uint8_t *hour_field, uint8_t *min_field,
                             lv_obj_t *row_lbl) {
    if (hour_field == NULL || min_field == NULL) return;
    time_picker_close();

    time_picker_hour_field = hour_field;
    time_picker_min_field = min_field;
    time_picker_row_lbl = row_lbl;

    time_picker_overlay = lv_obj_create(lv_scr_act());
    if (time_picker_overlay == NULL) {
        ESP_LOGE(TAG, "Failed to create time picker overlay!");
        time_picker_hour_field = NULL;
        time_picker_min_field = NULL;
        time_picker_row_lbl = NULL;
        return;
    }
    lv_obj_add_event_cb(time_picker_overlay, time_picker_overlay_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(time_picker_overlay);
    lv_obj_set_size(time_picker_overlay, 320, 240);
    lv_obj_set_pos(time_picker_overlay, 0, 0);
    lv_obj_set_style_bg_color(time_picker_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(time_picker_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(time_picker_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(time_picker_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(time_picker_overlay, time_picker_done_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(time_picker_overlay);
    lv_obj_set_size(card, 262, 214);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 8);

    time_picker_big_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(time_picker_big_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(time_picker_big_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(time_picker_big_lbl, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *hour_lbl = lv_label_create(card);
    lv_label_set_text(hour_lbl, "Hour");
    lv_obj_set_style_text_font(hour_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hour_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(hour_lbl, LV_ALIGN_TOP_LEFT, 18, TIME_PICKER_HOUR_Y + 8);
    time_picker_add_step_btn(card, LV_SYMBOL_MINUS, -80, TIME_PICKER_HOUR_Y, 0);
    time_picker_add_step_btn(card, LV_SYMBOL_PLUS,  -16, TIME_PICKER_HOUR_Y, 1);

    lv_obj_t *min_lbl = lv_label_create(card);
    lv_label_set_text(min_lbl, "Minute");
    lv_obj_set_style_text_font(min_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(min_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(min_lbl, LV_ALIGN_TOP_LEFT, 18, TIME_PICKER_MIN_Y + 8);
    time_picker_add_step_btn(card, LV_SYMBOL_MINUS, -80, TIME_PICKER_MIN_Y, 2);
    time_picker_add_step_btn(card, LV_SYMBOL_PLUS,  -16, TIME_PICKER_MIN_Y, 3);

    lv_obj_t *done_btn = lv_btn_create(card);
    lv_obj_set_size(done_btn, CYGM_BTN_W_TEXT, 32);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_update_layout(done_btn);
    cygm_apply_ghost_btn(done_btn);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(done_lbl);
    lv_obj_add_event_cb(done_btn, time_picker_done_event_cb, LV_EVENT_CLICKED, NULL);

    time_picker_refresh_labels();
}

// ---- Event handlers ----

static void opt_switch_event_cb(lv_event_t *e) {
    uint8_t *field = (uint8_t *)lv_event_get_user_data(e);
    if (field == NULL) return;
    *field = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    alarm_ext_dirty = true;
}

static void opt_stepper_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int packed = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = packed >> 1;
    if (idx < 0 || idx >= OPT_STEPPER_COUNT) return;

    opt_stepper_t *s = &opt_steppers[idx];
    if (s->field == NULL) return;

    // Clamp, never wrap: a mis-tap must not send a safety threshold to the
    // opposite end of its range.
    int value = (int)*s->field + ((packed & 1) ? (int)s->step : -(int)s->step);
    if (value < (int)s->min) value = (int)s->min;
    if (value > (int)s->max) value = (int)s->max;
    if (value == (int)*s->field) return;

    *s->field = (uint8_t)value;
    alarm_ext_dirty = true;

    if (s->value_lbl != NULL) {
        char value_text[40];
        opt_format_value(s, value_text, sizeof(value_text));
        lv_label_set_text(s->value_lbl, value_text);
    }
}

static void alarm_options_open_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Opening advanced alert options");
    nvs_save_alarm_settings(&current_alarm_settings);

    alarm_options_page = 0;
    create_alarm_options_screen();

    if (screen_alarm_options == NULL) {
        ESP_LOGE(TAG, "Failed to create alarm options screen!");
        return;
    }

    lv_scr_load(screen_alarm_options);

    if (screen_alarm_settings != NULL) {
        lv_obj_del(screen_alarm_settings);
        screen_alarm_settings = NULL;
    }
}

static void alarm_options_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "Alert options back - saving alarm extension settings");
    alarm_options_close(true);
}

static void alarm_options_prev_page_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    alarm_options_goto_page(alarm_options_page - 1);
}

static void alarm_options_next_page_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    alarm_options_goto_page(alarm_options_page + 1);
}

static void quiet_start_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_time_picker("Quiet Hours Start",
                     &alarm_ext_settings.quiet_start_hour,
                     &alarm_ext_settings.quiet_start_min,
                     quiet_start_value_lbl);
}

static void quiet_end_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_time_picker("Quiet Hours End",
                     &alarm_ext_settings.quiet_end_hour,
                     &alarm_ext_settings.quiet_end_min,
                     quiet_end_value_lbl);
}

static void gap_tone_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (screen_tone_picker != NULL) {
        lv_obj_del(screen_tone_picker);
        screen_tone_picker = NULL;
    }
    tone_picker_current_page = 0;
    tone_picker_ext_ref = &alarm_ext_settings.gap_tone;
    create_tone_picker_screen(NULL);
}

static void time_picker_step_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (time_picker_hour_field == NULL || time_picker_min_field == NULL) return;

    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
        case 0:
            *time_picker_hour_field = (*time_picker_hour_field == 0) ? 23
                                    : (uint8_t)(*time_picker_hour_field - 1);
            break;
        case 1:
            *time_picker_hour_field = (uint8_t)((*time_picker_hour_field + 1) % 24);
            break;
        case 2:
            *time_picker_min_field = (*time_picker_min_field < 5) ? 55
                                   : (uint8_t)(*time_picker_min_field - 5);
            break;
        default:
            *time_picker_min_field = (uint8_t)((*time_picker_min_field + 5) % 60);
            break;
    }

    alarm_ext_dirty = true;
    time_picker_refresh_labels();
}

static void time_picker_done_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    time_picker_close();
    alarm_ext_flush();
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

    // In mmol mode the slider value is tenths-of-mmol; convert back to canonical
    // mg/dL for storage/comparison (round: mg/dL = tenths * 1000 / 555).
    int mgdl = user_glucose_mmol ? (int)((value * 1000 + 277) / 555) : (int)value;
    alarm->threshold = mgdl;

    lv_obj_t *threshold_value = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (threshold_value != NULL) {
        char threshold_text[32];  // sized for GCC -Werror=format-truncation worst case
        if (user_glucose_mmol) {
            // Format from the slider tenths so the label matches the knob
            // exactly; the bound lets GCC prove the snprintf cannot truncate.
            int t = (int)value; if (t < 0) t = 0; if (t > 9999) t = 9999;
            snprintf(threshold_text, sizeof(threshold_text), "%d.%d mmol/L",
                     t / 10, t % 10);
        } else {
            cygm_format_threshold(mgdl, threshold_text, sizeof(threshold_text));
        }
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

static void alarm_led_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) return;

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->led_enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "LED enabled: %d", alarm->led_enabled);
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
        alarm_ext_flush();

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
            tone_picker_ext_ref = NULL;
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

        // Data-gap tone: the alarm list is not on screen, so only the overlay
        // is torn down and the options row label is refreshed in place.
        if (tone_picker_ext_ref != NULL) {
            *tone_picker_ext_ref = (uint8_t)tone_index;
            alarm_ext_dirty = true;
            ESP_LOGI(TAG, "Data-gap tone selected: %d", tone_index);

            if (gap_tone_value_lbl != NULL && tone_index >= 0 && tone_index < ALARM_TONE_COUNT) {
                lv_label_set_text(gap_tone_value_lbl, tone_short_names[tone_index]);
            }

            if (screen_tone_picker != NULL) {
                lv_obj_del(screen_tone_picker);
                screen_tone_picker = NULL;
            }
            tone_picker_current_page = 0;
            tone_picker_alarm_ref = NULL;
            tone_picker_ext_ref = NULL;

            alarm_ext_flush();
            return;
        }

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
    // While the device is driven over serial there is no touch activity, so
    // this timeout would yank the UI home mid-session. Use the accessor, not
    // the raw flag — the hold self-expires.
    if (cygm_ui_hold_active()) return;

    lv_obj_t *active = lv_scr_act();
    bool alarm_ui_active =
        (active == screen_alarm_settings) ||
        (active == screen_alarm_detail_editor) ||
        (active == screen_alarm_preview) ||
        (active == screen_alarm_options) ||
        (screen_tone_picker != NULL) ||
        (time_picker_overlay != NULL);

    if (!alarm_ui_active) {
        // The global watchdog runs the same timeout from another task and can
        // load home first, so anything this module still owns must be flushed
        // and freed here or it leaks with the alert settings unsaved.
        if (screen_alarm_options != NULL && screen_alarm_options != active) {
            alarm_options_close(false);
        }
        lv_timer_del(timer);
        alarm_inactivity_timer = NULL;
        return;
    }

    // A takeover is parented to lv_scr_act(), so deleting these screens under a
    // live alert would free the overlay out from under its pulse timer. The
    // timer keeps ticking; teardown resumes once stop_visual_alarm() clears it.
    if (visual_alarm_active) return;

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
        tone_picker_ext_ref = NULL;
    }

    // Load home FIRST (never delete active screen before lv_scr_load)
    lv_scr_load(screen_home);

    // Persists the alarm extension blob and drops the options screen; a no-op
    // when the user never opened it.
    alarm_options_close(false);

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
