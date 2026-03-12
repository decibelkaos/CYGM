/*
 * time_weather_screens.c
 *
 * Time/Date/Weather settings screen + location search screen
 */

#include "time_weather_screens.h"
#include "shared_state.h"
#include "main.h"
#include "nvs_config.h"
#include "features/weather_system.h"
#include "features/time_system.h"
#include "features/geocoding_api.h"
#include "dexcom_api.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include "ui/menu_screen.h"

static const char *TAG = "TIME_WEATHER";

// ==================== Glass Theme Constants ====================
#define GLASS_BG        0x0F1419
#define GLASS_BORDER    0x2A3A4A
#define GLASS_DIVIDER   0x263040
#define CARD_PRESSED    0x2a3545

// ==================== Confirmation Overlay State ====================
static lv_obj_t *confirm_overlay = NULL;
static lv_timer_t *confirm_timer = NULL;
static int confirm_countdown = 5;

// ==================== Location Search State ====================
static lv_obj_t *search_textarea = NULL;
static lv_obj_t *search_keyboard = NULL;
static lv_obj_t *result_card = NULL;
static lv_obj_t *result_label = NULL;
static lv_obj_t *result_counter_label = NULL;
static lv_obj_t *prev_btn = NULL;
static lv_obj_t *next_btn = NULL;
static lv_obj_t *confirm_btn = NULL;
static lv_obj_t *new_search_btn = NULL;
static lv_obj_t *hint_label = NULL;

static char temp_search_query[128] = {0};
static geocode_result_t search_results[MAX_GEOCODE_RESULTS];
static uint8_t search_result_count = 0;
static int selected_result_idx = -1;
static volatile bool location_search_in_progress = false;
static char pending_search_query[128] = {0};

// ==================== Forward Declarations ====================
static void create_location_search_screen(void);
static void add_search_keyboard(void);
static void update_result_display(void);
static void show_results_phase(void);
static void show_input_phase(void);
static void clear_search_state(void);
static void apply_selected_location(void);

// ==================== Background Search Task ====================
static void location_search_task(void *pvParameters) {
    // Take network mutex and close persistent Dexcom SSL client to free ~30KB.
    // Without this, esp_http_client_init fails with only ~7KB largest block.
    // The glucose task will re-establish SSL on its next cycle.
    bool mutex_held = false;
    if (network_mutex != NULL &&
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        mutex_held = true;
        if (dexcom_persistent_client_is_open()) {
            ESP_LOGI(TAG, "Closing Dexcom SSL to free memory for geocoding");
            dexcom_close_persistent_client();
        }
    }

    esp_err_t err = geocoding_search(pending_search_query, search_results, &search_result_count);

    if (mutex_held) {
        xSemaphoreGive(network_mutex);
    }

    if (lvgl_port_lock(100)) {
        if (err == ESP_OK && search_result_count > 0) {
            ESP_LOGI(TAG, "Found %d location(s)", search_result_count);
            selected_result_idx = 0;
            show_results_phase();
        } else {
            ESP_LOGW(TAG, "No locations found for: %s", pending_search_query);
            selected_result_idx = -1;
            search_result_count = 0;
            if (hint_label != NULL) {
                lv_label_set_text(hint_label, "No locations found.\nTry different search terms.");
                lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_ORANGE), 0);
            }
        }
        lvgl_port_unlock();
    }

    location_search_in_progress = false;
    vTaskDelete(NULL);
}

// ==================== Phase Switching ====================
static void show_results_phase(void) {
    // Hide keyboard and hint
    if (search_keyboard) lv_obj_add_flag(search_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (hint_label) lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);

    // Disable textarea input
    if (search_textarea) {
        lv_obj_clear_state(search_textarea, LV_STATE_FOCUSED);
        lv_obj_clear_flag(search_textarea, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }

    // Show result card, confirm, new search
    if (result_card) lv_obj_clear_flag(result_card, LV_OBJ_FLAG_HIDDEN);
    if (confirm_btn) lv_obj_clear_flag(confirm_btn, LV_OBJ_FLAG_HIDDEN);
    if (new_search_btn) lv_obj_clear_flag(new_search_btn, LV_OBJ_FLAG_HIDDEN);

    update_result_display();
}

static void show_input_phase(void) {
    // Show keyboard and hint
    if (search_keyboard) lv_obj_clear_flag(search_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (hint_label) {
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hint_label, "Type a city name or postal code\nthen press Enter to search");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }

    // Re-enable textarea
    if (search_textarea) {
        lv_obj_add_flag(search_textarea, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_state(search_textarea, LV_STATE_FOCUSED);
        if (search_keyboard) lv_keyboard_set_textarea(search_keyboard, search_textarea);
    }

    // Hide result card, confirm, new search
    if (result_card) lv_obj_add_flag(result_card, LV_OBJ_FLAG_HIDDEN);
    if (confirm_btn) lv_obj_add_flag(confirm_btn, LV_OBJ_FLAG_HIDDEN);
    if (new_search_btn) lv_obj_add_flag(new_search_btn, LV_OBJ_FLAG_HIDDEN);

    // Clear old results
    search_result_count = 0;
    selected_result_idx = -1;
}

// ==================== Result Display ====================
static void update_result_display(void) {
    if (result_label == NULL || search_result_count == 0 || selected_result_idx < 0)
        return;

    geocode_result_t *r = &search_results[selected_result_idx];
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n%s%s%s\n%s",
             r->name,
             r->admin1[0] ? r->admin1 : "",
             r->admin1[0] ? ", " : "",
             r->country,
             r->timezone);
    lv_label_set_text(result_label, buf);

    // Update counter
    if (result_counter_label) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d / %d", selected_result_idx + 1, search_result_count);
        lv_label_set_text(result_counter_label, cnt);
    }

    // Show/hide prev/next
    if (search_result_count > 1) {
        if (prev_btn) lv_obj_clear_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
        if (next_btn) lv_obj_clear_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (prev_btn) lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
        if (next_btn) lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// ==================== State Helpers ====================
static void clear_search_state(void) {
    memset(temp_search_query, 0, sizeof(temp_search_query));
    search_result_count = 0;
    selected_result_idx = -1;
    search_textarea = NULL;
    search_keyboard = NULL;
    result_card = NULL;
    result_label = NULL;
    result_counter_label = NULL;
    prev_btn = NULL;
    next_btn = NULL;
    confirm_btn = NULL;
    new_search_btn = NULL;
    hint_label = NULL;
}

static void apply_selected_location(void) {
    if (selected_result_idx < 0 || selected_result_idx >= search_result_count)
        return;

    geocode_result_t *r = &search_results[selected_result_idx];
    ESP_LOGI(TAG, "Location confirmed: %s, %s (%s)", r->name, r->country, r->timezone);

    // Save search query
    strncpy(user_zipcode, temp_search_query, sizeof(user_zipcode) - 1);
    user_zipcode[sizeof(user_zipcode) - 1] = '\0';

    // Save coordinates
    user_latitude = r->latitude;
    user_longitude = r->longitude;

    // Format location name
    if (r->admin1[0] != '\0') {
        snprintf(user_location, sizeof(user_location), "%.30s, %.30s", r->name, r->admin1);
    } else {
        snprintf(user_location, sizeof(user_location), "%.30s, %.30s", r->name, r->country);
    }

    // Timezone
    if (r->timezone[0] != '\0') {
        strncpy(user_timezone, r->timezone, sizeof(user_timezone) - 1);
        user_timezone[sizeof(user_timezone) - 1] = '\0';
    } else {
        strncpy(user_timezone, "America/New_York", sizeof(user_timezone) - 1);
        ESP_LOGW(TAG, "No timezone in result, using default");
    }

    // Save to NVS
    nvs_save_weather_coords(user_zipcode, user_latitude, user_longitude, user_location);
    nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);

    // Apply timezone
    const char *posix_tz = get_posix_timezone(user_timezone);
    setenv("TZ", posix_tz, 1);
    tzset();

    // Reset geocoding tracker
    memset(geocoded_zipcode, 0, sizeof(geocoded_zipcode));

    // Trigger immediate weather update
    if (weather_task_handle != NULL) {
        xTaskNotifyGive(weather_task_handle);
    }

    update_time_display();
    ESP_LOGI(TAG, "Location saved: %s (%.4f, %.4f) [%s]",
             user_location, user_latitude, user_longitude, user_timezone);

    // Return to settings — load new screen FIRST, then delete old.
    // Deleting active screen before lv_scr_load causes PANIC crash.
    clear_search_state();
    create_time_weather_settings_screen();
    lv_scr_load(screen_time_weather_settings);
    if (screen_zipcode_entry != NULL) {
        lv_obj_del_async(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }
}

// ==================== Location Search Event Handlers ====================

static void location_back_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Location search — back");
    clear_search_state();
    // Create and load new screen FIRST — LVGL needs the active screen alive during transition.
    // Deleting screen_zipcode_entry (the active screen) before lv_scr_load causes PANIC crash.
    create_time_weather_settings_screen();
    lv_scr_load(screen_time_weather_settings);
    // Now safe to delete old search screen (async because we may be inside its event callback)
    if (screen_zipcode_entry != NULL) {
        lv_obj_del_async(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }
}

static void location_kb_ready_cb(lv_event_t *e) {
    (void)e;

    // Get current text from textarea
    if (search_textarea == NULL) return;
    const char *txt = lv_textarea_get_text(search_textarea);

    if (txt == NULL || strlen(txt) < 2) {
        if (hint_label) {
            lv_label_set_text(hint_label, "Type at least 2 characters");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_ORANGE), 0);
        }
        return;
    }

    if (location_search_in_progress) {
        ESP_LOGW(TAG, "Search already in progress");
        return;
    }

    // Save query and start search
    strncpy(temp_search_query, txt, sizeof(temp_search_query) - 1);
    temp_search_query[sizeof(temp_search_query) - 1] = '\0';
    strncpy(pending_search_query, txt, sizeof(pending_search_query) - 1);
    pending_search_query[sizeof(pending_search_query) - 1] = '\0';

    ESP_LOGI(TAG, "Searching for: %s", pending_search_query);

    if (hint_label) {
        lv_label_set_text(hint_label, "Searching...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    }

    location_search_in_progress = true;
    if (xTaskCreate(location_search_task, "loc_search", 6144, NULL, 2, NULL) != pdPASS) {
        location_search_in_progress = false;
        ESP_LOGE(TAG, "Failed to create search task");
        if (hint_label) {
            lv_label_set_text(hint_label, "Search failed — try again");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_RED), 0);
        }
    }
}

static void location_prev_cb(lv_event_t *e) {
    (void)e;
    if (search_result_count > 0) {
        selected_result_idx--;
        if (selected_result_idx < 0) selected_result_idx = search_result_count - 1;
        update_result_display();
    }
}

static void location_next_cb(lv_event_t *e) {
    (void)e;
    if (search_result_count > 0) {
        selected_result_idx++;
        if (selected_result_idx >= search_result_count) selected_result_idx = 0;
        update_result_display();
    }
}

static void location_confirm_cb(lv_event_t *e) {
    (void)e;
    apply_selected_location();
}

static void location_new_search_cb(lv_event_t *e) {
    (void)e;
    show_input_phase();
    if (search_textarea) {
        lv_textarea_set_text(search_textarea, "");
    }
}

// ==================== Location Search Screen ====================
static void create_location_search_screen(void) {
    if (screen_zipcode_entry != NULL) {
        lv_obj_del(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }

    clear_search_state();

    screen_zipcode_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_zipcode_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_zipcode_entry, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Ghost back button ----
    lv_obj_t *back_btn = lv_btn_create(screen_zipcode_entry);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, location_back_cb, LV_EVENT_CLICKED, NULL);

    // ---- Title ----
    lv_obj_t *title = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(title, "Search Location");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---- Search field card ----
    lv_obj_t *search_card = lv_obj_create(screen_zipcode_entry);
    lv_obj_set_size(search_card, 300, 38);
    lv_obj_align(search_card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(search_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(search_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(search_card, 10, 0);
    lv_obj_set_style_border_width(search_card, 1, 0);
    lv_obj_set_style_border_color(search_card, lv_color_hex(GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(search_card, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(search_card, 0, 0);
    lv_obj_clear_flag(search_card, LV_OBJ_FLAG_SCROLLABLE);

    // Search icon
    lv_obj_t *search_icon = lv_label_create(search_card);
    lv_label_set_text(search_icon, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(search_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(search_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(search_icon, LV_ALIGN_LEFT_MID, 10, 0);

    // Textarea inside card
    search_textarea = lv_textarea_create(search_card);
    lv_obj_set_size(search_textarea, 250, 32);
    lv_obj_align(search_textarea, LV_ALIGN_LEFT_MID, 30, 0);
    lv_textarea_set_one_line(search_textarea, true);
    lv_textarea_set_max_length(search_textarea, 100);
    lv_textarea_set_placeholder_text(search_textarea, "City or postal code...");
    lv_textarea_set_text(search_textarea, "");

    // Textarea styling — glass field
    lv_obj_set_style_bg_opa(search_textarea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(search_textarea, 0, 0);
    lv_obj_set_style_text_font(search_textarea, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(search_textarea, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_pad_top(search_textarea, 4, 0);
    lv_obj_set_style_pad_bottom(search_textarea, 4, 0);
    lv_obj_set_style_pad_left(search_textarea, 4, 0);

    // Placeholder color
    lv_obj_set_style_text_color(search_textarea, lv_color_hex(COLOR_TEXT_DIM),
                                 LV_PART_TEXTAREA_PLACEHOLDER);

    // Cursor styling
    lv_obj_set_style_border_color(search_textarea, lv_color_hex(COLOR_ACCENT_BLUE),
                                   LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(search_textarea, 1,
                                   LV_PART_CURSOR | LV_STATE_FOCUSED);

    // ---- Hint text ----
    hint_label = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(hint_label, "Type a city name or postal code\nthen press Enter to search");
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint_label, 290);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 82);

    // ---- Result card (hidden initially) ----
    result_card = lv_obj_create(screen_zipcode_entry);
    lv_obj_set_size(result_card, 300, 100);
    lv_obj_align(result_card, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_color(result_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(result_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(result_card, 10, 0);
    lv_obj_set_style_border_width(result_card, 1, 0);
    lv_obj_set_style_border_color(result_card, lv_color_hex(GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(result_card, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(result_card, 8, 0);
    lv_obj_clear_flag(result_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(result_card, LV_OBJ_FLAG_HIDDEN);

    // Green accent bar on result card
    lv_obj_t *r_accent = lv_obj_create(result_card);
    lv_obj_remove_style_all(r_accent);
    lv_obj_set_size(r_accent, 3, 60);
    lv_obj_align(r_accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(r_accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(r_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r_accent, 1, 0);

    // Result text
    result_label = lv_label_create(result_card);
    lv_label_set_text(result_label, "");
    lv_obj_set_style_text_font(result_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(result_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_width(result_label, 200);
    lv_obj_align(result_label, LV_ALIGN_LEFT_MID, 12, 0);

    // Result counter (top-right of card)
    result_counter_label = lv_label_create(result_card);
    lv_label_set_text(result_counter_label, "");
    lv_obj_set_style_text_font(result_counter_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(result_counter_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(result_counter_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Prev button (ghost, inside result card)
    prev_btn = lv_btn_create(result_card);
    lv_obj_set_size(prev_btn, 30, 40);
    lv_obj_align(prev_btn, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);
    lv_obj_set_style_radius(prev_btn, 6, 0);
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, location_prev_cb, LV_EVENT_CLICKED, NULL);

    // Next button (ghost, inside result card)
    next_btn = lv_btn_create(result_card);
    lv_obj_set_size(next_btn, 30, 40);
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);
    lv_obj_set_style_radius(next_btn, 6, 0);
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, location_next_cb, LV_EVENT_CLICKED, NULL);

    // ---- Action buttons row (hidden initially, side by side) ----
    // "Use Location" button (left)
    confirm_btn = lv_btn_create(screen_zipcode_entry);
    lv_obj_set_size(confirm_btn, 148, 36);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_MID, -77, 188);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm_btn, 10, 0);
    lv_obj_set_style_border_width(confirm_btn, 0, 0);
    lv_obj_set_style_shadow_width(confirm_btn, 0, 0);
    lv_obj_add_flag(confirm_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, LV_SYMBOL_OK " Use Location");
    lv_obj_set_style_text_font(confirm_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(confirm_lbl);
    lv_obj_add_event_cb(confirm_btn, location_confirm_cb, LV_EVENT_CLICKED, NULL);

    // "New Search" button (right)
    new_search_btn = lv_btn_create(screen_zipcode_entry);
    lv_obj_set_size(new_search_btn, 148, 36);
    lv_obj_align(new_search_btn, LV_ALIGN_TOP_MID, 77, 188);
    lv_obj_set_style_bg_opa(new_search_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(new_search_btn, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(new_search_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(new_search_btn, 1, 0);
    lv_obj_set_style_border_color(new_search_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_opa(new_search_btn, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(new_search_btn, 0, 0);
    lv_obj_set_style_radius(new_search_btn, 10, 0);
    lv_obj_add_flag(new_search_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ns_lbl = lv_label_create(new_search_btn);
    lv_label_set_text(ns_lbl, LV_SYMBOL_REFRESH " New Search");
    lv_obj_set_style_text_font(ns_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ns_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(ns_lbl);
    lv_obj_add_event_cb(new_search_btn, location_new_search_cb, LV_EVENT_CLICKED, NULL);

    // NOTE: Keyboard is NOT created here — it's deferred to add_search_keyboard()
    // which runs AFTER the settings screen is freed, preventing LVGL pool exhaustion.
    // The 36KB LVGL pool can't hold Home + Menu + Settings + Search + Keyboard simultaneously.

    ESP_LOGI(TAG, "Location search screen created (keyboard deferred)");
}

// Create the on-screen keyboard.  Called separately because the keyboard's ~3KB
// LVGL allocation can only succeed after the settings screen is freed.  Without
// this split, 4 screens + keyboard exceed the 36KB pool → LV_ASSERT_MALLOC
// fires while(1) → device freeze.
static void add_search_keyboard(void) {
    if (search_keyboard != NULL || screen_zipcode_entry == NULL) return;

    search_keyboard = lv_keyboard_create(screen_zipcode_entry);
    if (search_keyboard == NULL) {
        ESP_LOGE(TAG, "Keyboard creation failed (LVGL pool exhausted)");
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGE(TAG, "LVGL pool: used=%lu free=%lu frag=%d%%",
                 (unsigned long)(mon.total_size - mon.free_size),
                 (unsigned long)mon.free_size, mon.frag_pct);
        if (hint_label) {
            lv_label_set_text(hint_label, "Memory error — press back\nand try again");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_RED), 0);
        }
        return;
    }

    lv_obj_set_size(search_keyboard, 320, 130);
    lv_obj_align(search_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(search_keyboard, search_textarea);
    lv_keyboard_set_mode(search_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard background
    lv_obj_set_style_bg_color(search_keyboard, lv_color_hex(GLASS_BG), 0);
    lv_obj_set_style_bg_opa(search_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(search_keyboard, 0, 0);
    lv_obj_set_style_pad_all(search_keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(search_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(search_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(search_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(search_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(search_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(search_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(search_keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight
    lv_obj_set_style_bg_color(search_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    // Keyboard events
    lv_obj_add_event_cb(search_keyboard, location_kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(search_keyboard, location_back_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "Search keyboard created");
}

// Stub for header compatibility
void create_zipcode_entry_screen(void) {
    create_location_search_screen();
    add_search_keyboard();
}

// ==================== Settings Screen Handlers ====================

static void time_weather_back_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Saving time & weather settings to NVS...");

    // Save time settings
    esp_err_t time_ret = nvs_save_time_settings(user_timezone, user_dst_enabled, user_24hr_format);
    if (time_ret == ESP_OK) {
        const char *posix_tz = get_posix_timezone(user_timezone);
        setenv("TZ", posix_tz, 1);
        tzset();
        update_time_display();
        ESP_LOGI(TAG, "Time settings saved: %s -> %s", user_timezone, posix_tz);
    }

    // Save weather settings
    esp_err_t weather_ret = nvs_save_weather_settings(user_zipcode, user_temp_celsius,
                                                       user_weather_interval_min);
    if (weather_ret == ESP_OK) {
        if (weather_task_handle != NULL) {
            xTaskNotifyGive(weather_task_handle);
        }
        ESP_LOGI(TAG, "Weather settings saved");
    }

    // Recreate menu (was deleted when entering time/weather settings)
    create_menu_screen();
    lv_scr_load(screen_menu);

    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }
}

static void location_search_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Opening location search");

    // Step 1: Create search screen (lightweight — no keyboard yet)
    create_location_search_screen();

    // Step 2: Load search screen (settings becomes non-active)
    lv_scr_load(screen_zipcode_entry);

    // Step 3: Free settings screen to reclaim ~4KB LVGL pool for keyboard.
    // Without this, Home+Menu+Settings+Search+Keyboard exceeds 36KB pool.
    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }

    // Step 4: Create keyboard (now has enough LVGL memory)
    add_search_keyboard();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool after search: used=%lu free=%lu frag=%d%%",
             (unsigned long)(mon.total_size - mon.free_size),
             (unsigned long)mon.free_size, mon.frag_pct);
}

// ==================== Weather Interval Confirmation Overlay ====================

static void confirm_overlay_close(void) {
    if (confirm_timer) {
        lv_timer_del(confirm_timer);
        confirm_timer = NULL;
    }
    confirm_overlay = NULL;  // Child of settings screen — deleted with parent

    // Load home screen FIRST — LVGL needs current screen alive during transition
    lv_scr_load(screen_home);
    home_screen_active = true;
    pause_background_tasks = false;

    // Now safe to delete old settings screen (and all children including overlay)
    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }
}

static void confirm_timer_cb(lv_timer_t *t) {
    (void)t;
    confirm_countdown--;
    if (confirm_countdown <= 0) {
        confirm_overlay_close();
        return;
    }
    // Update countdown label
    if (confirm_overlay) {
        lv_obj_t *countdown_lbl = (lv_obj_t *)lv_obj_get_user_data(confirm_overlay);
        if (countdown_lbl) {
            lv_label_set_text_fmt(countdown_lbl, "Returning home in %ds...", confirm_countdown);
        }
    }
}

static void confirm_overlay_tap_cb(lv_event_t *e) {
    (void)e;
    confirm_overlay_close();
}

static void show_interval_confirm_overlay(uint8_t minutes) {
    // Save the setting
    user_weather_interval_min = minutes;
    nvs_save_weather_settings(user_zipcode, user_temp_celsius, user_weather_interval_min);
    if (weather_task_handle != NULL) {
        xTaskNotifyGive(weather_task_handle);
    }
    ESP_LOGI(TAG, "Weather interval set to %d min", minutes);

    // Delete any existing overlay
    if (confirm_overlay) {
        if (confirm_timer) { lv_timer_del(confirm_timer); confirm_timer = NULL; }
        lv_obj_del(confirm_overlay);
        confirm_overlay = NULL;
    }

    // ---- Full-screen click catcher (dim backdrop) ----
    confirm_overlay = lv_obj_create(screen_time_weather_settings);
    lv_obj_set_size(confirm_overlay, 320, 240);
    lv_obj_set_pos(confirm_overlay, 0, 0);
    lv_obj_set_style_bg_color(confirm_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(confirm_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(confirm_overlay, 0, 0);
    lv_obj_set_style_radius(confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(confirm_overlay, 0, 0);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(confirm_overlay, confirm_overlay_tap_cb, LV_EVENT_CLICKED, NULL);

    // ---- Glass panel (matches glucose chart overlay style) ----
    lv_obj_t *panel = lv_obj_create(confirm_overlay);
    lv_obj_set_size(panel, 270, 140);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(GLASS_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(panel, 25, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // Pass clicks to backdrop

    // ---- Title row (chart-style: icon + text) ----
    lv_obj_t *title_row = lv_obj_create(panel);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, 254, 26);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(title_row);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, "Weather Updated");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    // ---- Subtle divider (chart grid-line style) ----
    lv_obj_t *div = lv_obj_create(panel);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 254, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(div, lv_color_hex(GLASS_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // ---- Interval text (centered, prominent) ----
    const char *interval_text = (minutes == 15) ? "15 minutes" :
                                (minutes == 30) ? "30 minutes" : "1 hour";
    lv_obj_t *msg = lv_label_create(panel);
    lv_label_set_text_fmt(msg, "Updates every %s", interval_text);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 254);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -4);

    // ---- Bottom row (chart-style: stats left, close right) ----
    confirm_countdown = 5;
    lv_obj_t *countdown_lbl = lv_label_create(panel);
    lv_label_set_text_fmt(countdown_lbl, "Returning home in %ds...", confirm_countdown);
    lv_obj_set_style_text_font(countdown_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(countdown_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(countdown_lbl, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    // Close button (chart-style ghost)
    lv_obj_t *close_btn = lv_btn_create(panel);
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
    lv_obj_add_event_cb(close_btn, confirm_overlay_tap_cb, LV_EVENT_CLICKED, NULL);

    // Store countdown label for timer callback
    lv_obj_set_user_data(confirm_overlay, countdown_lbl);

    // Start 1-second countdown timer
    confirm_timer = lv_timer_create(confirm_timer_cb, 1000, NULL);
}

static void weather_interval_btn_cb(lv_event_t *e) {
    uint8_t minutes = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    show_interval_confirm_overlay(minutes);
}

// ==================== Settings Screen ====================
void create_time_weather_settings_screen(void) {
    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }

    screen_time_weather_settings = lv_obj_create(NULL);
    lv_obj_add_style(screen_time_weather_settings, &style_bg, 0);
    lv_obj_clear_flag(screen_time_weather_settings, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *header = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(header, "Time & Weather");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_time_weather_settings);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, time_weather_back_event_cb, LV_EVENT_CLICKED, NULL);

    // ============ Location Card ============
    lv_obj_t *loc_card = lv_obj_create(screen_time_weather_settings);
    lv_obj_set_size(loc_card, 290, 130);
    lv_obj_align(loc_card, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(loc_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(loc_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(loc_card, 12, 0);
    lv_obj_set_style_border_width(loc_card, 1, 0);
    lv_obj_set_style_border_color(loc_card, lv_color_hex(GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(loc_card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(loc_card, 15, 0);
    lv_obj_set_style_shadow_color(loc_card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(loc_card, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(loc_card, 0, 0);
    lv_obj_clear_flag(loc_card, LV_OBJ_FLAG_SCROLLABLE);

    // Blue accent bar
    lv_obj_t *accent = lv_obj_create(loc_card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 3, 50);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 6, -18);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 2, 0);

    // Timezone section
    lv_obj_t *tz_header = lv_label_create(loc_card);
    lv_label_set_text(tz_header, "TIMEZONE");
    lv_obj_set_style_text_font(tz_header, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tz_header, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(tz_header, 2, 0);
    lv_obj_align(tz_header, LV_ALIGN_TOP_LEFT, 16, 6);

    lv_obj_t *tz_value = lv_label_create(loc_card);
    lv_label_set_text(tz_value, strlen(user_timezone) > 0 ? user_timezone : "Not set");
    lv_obj_set_style_text_font(tz_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tz_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(tz_value, LV_ALIGN_TOP_LEFT, 16, 20);

    // Divider
    lv_obj_t *div1 = lv_obj_create(loc_card);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, 262, 1);
    lv_obj_align(div1, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(div1, lv_color_hex(GLASS_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_COVER, 0);

    // Location section
    lv_obj_t *loc_header = lv_label_create(loc_card);
    lv_label_set_text(loc_header, "LOCATION");
    lv_obj_set_style_text_font(loc_header, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(loc_header, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(loc_header, 2, 0);
    lv_obj_align(loc_header, LV_ALIGN_TOP_LEFT, 16, 48);

    lv_obj_t *loc_value = lv_label_create(loc_card);
    lv_label_set_text(loc_value, strlen(user_location) > 0 ? user_location : "Not set");
    lv_obj_set_style_text_font(loc_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(loc_value, LV_ALIGN_TOP_LEFT, 16, 62);

    // Divider
    lv_obj_t *div2 = lv_obj_create(loc_card);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, 262, 1);
    lv_obj_align(div2, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(div2, lv_color_hex(GLASS_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);

    // Search button
    lv_obj_t *search_btn = lv_btn_create(loc_card);
    lv_obj_set_size(search_btn, 262, 36);
    lv_obj_align(search_btn, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(search_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(search_btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(search_btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(search_btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(search_btn, 8, 0);
    lv_obj_set_style_border_width(search_btn, 1, 0);
    lv_obj_set_style_border_color(search_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_opa(search_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(search_btn, 0, 0);

    lv_obj_t *search_lbl = lv_label_create(search_btn);
    lv_label_set_text(search_lbl, LV_SYMBOL_REFRESH " Search Location");
    lv_obj_set_style_text_font(search_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(search_lbl, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_center(search_lbl);
    lv_obj_add_event_cb(search_btn, location_search_event_cb, LV_EVENT_CLICKED, NULL);

    // ============ Weather Interval Card ============
    lv_obj_t *interval_card = lv_obj_create(screen_time_weather_settings);
    lv_obj_set_size(interval_card, 290, 56);
    lv_obj_align(interval_card, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_bg_color(interval_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(interval_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(interval_card, 12, 0);
    lv_obj_set_style_border_width(interval_card, 1, 0);
    lv_obj_set_style_border_color(interval_card, lv_color_hex(GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(interval_card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(interval_card, 15, 0);
    lv_obj_set_style_shadow_color(interval_card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(interval_card, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(interval_card, 0, 0);
    lv_obj_clear_flag(interval_card, LV_OBJ_FLAG_SCROLLABLE);

    // Orange accent bar
    lv_obj_t *accent2 = lv_obj_create(interval_card);
    lv_obj_remove_style_all(accent2);
    lv_obj_set_size(accent2, 3, 36);
    lv_obj_align(accent2, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(accent2, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_bg_opa(accent2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent2, 2, 0);

    lv_obj_t *interval_label = lv_label_create(interval_card);
    lv_label_set_text(interval_label, "Weather Update");
    lv_obj_set_style_text_font(interval_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(interval_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(interval_label, LV_ALIGN_TOP_LEFT, 16, 4);

    // Three interval buttons: 15min, 30min, 1hr
    static const uint8_t intervals[] = {15, 30, 60};
    static const char *labels[] = {"15 min", "30 min", "1 hr"};
    const int btn_w = 80;
    const int btn_h = 28;
    const int btn_y = 22;
    const int total_w = btn_w * 3 + 8 * 2;  // 3 buttons + 2 gaps
    const int start_x = (290 - total_w) / 2;

    for (int i = 0; i < 3; i++) {
        bool is_active = (user_weather_interval_min == intervals[i]);

        lv_obj_t *btn = lv_btn_create(interval_card);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, start_x + i * (btn_w + 8), btn_y);

        if (is_active) {
            // Active: filled blue
            lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
        } else {
            // Inactive: ghost with border
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(GLASS_BORDER), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
        }
        // Pressed state
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, is_active
            ? lv_color_hex(COLOR_TEXT_WHITE)
            : lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, weather_interval_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)intervals[i]);
    }
}
