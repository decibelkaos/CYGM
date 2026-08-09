/*
 * time_weather_screens.c - Time/Date/Weather settings and location search.
 */

#include "time_weather_screens.h"
#include "shared_state.h"
#include "main.h"
#include "nvs_config.h"
#include "features/weather_system.h"
#include "features/time_system.h"
#include "features/geocoding_api.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include "ui/menu_screen.h"

static const char *TAG = "TIME_WEATHER";

// ==================== Confirmation Overlay State ====================
static lv_obj_t *confirm_overlay = NULL;
static lv_timer_t *confirm_timer = NULL;
static int confirm_countdown = 5;

// ==================== Location Search State ====================
static lv_obj_t *search_textarea = NULL;
static lv_obj_t *search_keyboard = NULL;
static lv_obj_t *result_card = NULL;
static lv_obj_t *result_name_label = NULL;
static lv_obj_t *result_detail_label = NULL;
static lv_obj_t *result_counter_label = NULL;
static lv_obj_t *prev_btn = NULL;
static lv_obj_t *next_btn = NULL;
static lv_obj_t *confirm_btn = NULL;
static lv_obj_t *new_search_btn = NULL;
static lv_obj_t *hint_label = NULL;
static lv_obj_t *search_overlay = NULL;

static char temp_search_query[128] = {0};
static geocode_result_t search_results[MAX_GEOCODE_RESULTS];
static uint8_t search_result_count = 0;
static int selected_result_idx = -1;
static volatile bool location_search_in_progress = false;
static char pending_search_query[128] = {0};

// ==================== Shared Styles ====================
// Each lv_obj_set_style_* call allocates a per-object style from the 36KB LVGL
// pool. These looks repeat across the cards, so they are shared file statics —
// which must outlive every object using them, as LVGL keeps the pointer.
static lv_style_t s_card;      // settings card shell
static lv_style_t s_accent;    // 3px accent bar (colour stays per-object)
static lv_style_t s_rule;      // 1px hairline divider
static lv_style_t s_section;   // 12pt dim all-caps section header
static lv_style_t s_value;     // 14pt white value text
static bool s_styles_ready = false;

static void tw_init_styles(void) {
    if (s_styles_ready) return;
    s_styles_ready = true;

    lv_style_init(&s_card);
    lv_style_set_bg_color(&s_card, lv_color_hex(COLOR_CARD_BG));
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_radius(&s_card, 12);
    lv_style_set_border_width(&s_card, 1);
    lv_style_set_border_color(&s_card, lv_color_hex(COLOR_MODAL_BORDER));
    lv_style_set_border_opa(&s_card, LV_OPA_30);
    lv_style_set_shadow_width(&s_card, 0);
    lv_style_set_pad_all(&s_card, 0);

    lv_style_init(&s_accent);
    lv_style_set_bg_opa(&s_accent, LV_OPA_COVER);
    lv_style_set_radius(&s_accent, 2);

    lv_style_init(&s_rule);
    lv_style_set_bg_color(&s_rule, lv_color_hex(COLOR_DIVIDER));
    lv_style_set_bg_opa(&s_rule, LV_OPA_COVER);

    lv_style_init(&s_section);
    lv_style_set_text_font(&s_section, &lv_font_montserrat_12);
    lv_style_set_text_color(&s_section, lv_color_hex(COLOR_TEXT_DIM));
    lv_style_set_text_letter_space(&s_section, 2);

    lv_style_init(&s_value);
    lv_style_set_text_font(&s_value, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_value, lv_color_hex(COLOR_TEXT_WHITE));
}

// ==================== Widget Helpers ====================

// A control whose label IS the object — one pool object instead of a button
// plus a child label. A label draws its first line at the top of its content
// box, so vertical centring is pad_top, less the row the ghost border eats.
static lv_obj_t *tw_chip(lv_obj_t *parent, const char *txt, const lv_font_t *font,
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

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *tw_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
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

// Two stacked lines whose tops land `gap` pixels apart — one pool object
// instead of two separately positioned labels.
static lv_obj_t *tw_stacked_label(lv_obj_t *parent, lv_style_t *style,
                                  const lv_font_t *font, lv_coord_t gap) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_add_style(lbl, style, 0);
    lv_obj_set_style_text_line_space(lbl, gap - lv_font_get_line_height(font), 0);
    return lbl;
}

// A degraded screen the user can back out of beats LV_ASSERT_MALLOC's while(1).
#define TW_POOL_MIN_FREE    6144
#define TW_POOL_MIN_BIGGEST 2048

// Callers must already hold the LVGL lock.
static bool tw_pool_low(const char *what) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if (mon.free_size >= TW_POOL_MIN_FREE &&
        mon.free_biggest_size >= TW_POOL_MIN_BIGGEST) {
        return false;
    }
    ESP_LOGE(TAG, "LVGL pool too low for %s: free=%lu biggest=%lu frag=%d%% "
             "(need free>=%d biggest>=%d) — building degraded screen",
             what, (unsigned long)mon.free_size,
             (unsigned long)mon.free_biggest_size, (int)mon.frag_pct,
             TW_POOL_MIN_FREE, TW_POOL_MIN_BIGGEST);
    return true;
}

static void tw_add_low_memory_notice(lv_obj_t *scr) {
    lv_obj_t *warn = lv_label_create(scr);
    lv_label_set_text(warn, "Memory low — restart device");
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(warn, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, 0);
}

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
    // Provider SSL is already closed by the caller to free heap.
    bool mutex_held = false;
    if (network_mutex != NULL &&
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        mutex_held = true;
    }

    esp_err_t err = geocoding_search(pending_search_query, search_results, &search_result_count);

    if (mutex_held) {
        xSemaphoreGive(network_mutex);
    }

    // lvgl_port_lock(1) is a try-lock, so retry with a yield: one failed attempt
    // would strand the "Searching..." overlay until the inactivity watchdog.
    bool locked = false;
    for (int r = 0; r < 50 && !locked; r++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (locked) {
        // Remove search overlay
        if (search_overlay != NULL) {
            lv_obj_del(search_overlay);
            search_overlay = NULL;
        }

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
    } else {
        // Still remove the modal overlay so the screen never hangs;
        // lv_obj_del_async is safe without the lock held.
        ESP_LOGW(TAG, "LVGL lock unavailable after search — async-removing overlay to avoid hang");
        if (search_overlay != NULL) {
            lv_obj_del_async(search_overlay);
            search_overlay = NULL;
        }
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
    if (result_name_label == NULL || search_result_count == 0 || selected_result_idx < 0)
        return;

    geocode_result_t *r = &search_results[selected_result_idx];

    // Location name — large, prominent
    lv_label_set_text(result_name_label, r->name);

    // Admin/country + timezone — smaller, secondary
    if (result_detail_label) {
        char detail[192];
        snprintf(detail, sizeof(detail), "%s%s%s\n%s",
                 r->admin1[0] ? r->admin1 : "",
                 r->admin1[0] ? ", " : "",
                 r->country,
                 r->timezone);
        lv_label_set_text(result_detail_label, detail);
    }

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
    result_name_label = NULL;
    result_detail_label = NULL;
    result_counter_label = NULL;
    prev_btn = NULL;
    next_btn = NULL;
    confirm_btn = NULL;
    new_search_btn = NULL;
    hint_label = NULL;
    search_overlay = NULL;
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

    // Load the new screen FIRST: deleting the active screen before
    // lv_scr_load panics.
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
    // Load the new screen FIRST: deleting the active one before lv_scr_load
    // panics. The delete is async because we may be inside its event callback.
    create_time_weather_settings_screen();
    lv_scr_load(screen_time_weather_settings);
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

    // Show search overlay on active screen
    if (search_overlay == NULL && screen_zipcode_entry != NULL) {
        search_overlay = lv_obj_create(screen_zipcode_entry);
        lv_obj_set_size(search_overlay, 320, 240);
        lv_obj_set_pos(search_overlay, 0, 0);
        lv_obj_set_style_bg_color(search_overlay, lv_color_hex(COLOR_MODAL_BG), 0);
        lv_obj_set_style_bg_opa(search_overlay, LV_OPA_80, 0);
        lv_obj_set_style_border_width(search_overlay, 0, 0);
        lv_obj_set_style_radius(search_overlay, 0, 0);
        lv_obj_set_style_shadow_width(search_overlay, 0, 0);
        lv_obj_clear_flag(search_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(search_overlay, LV_OBJ_FLAG_CLICKABLE);  // never trap touch if stuck

        lv_obj_t *search_lbl = lv_label_create(search_overlay);
        lv_label_set_text(search_lbl, LV_SYMBOL_GPS "  Searching...");
        lv_obj_set_style_text_font(search_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(search_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(search_lbl);
    }

    // Free provider SSL before creating the task: with it alive (~15KB) there
    // is not enough heap left for a 4KB task stack.
    if (network_mutex != NULL &&
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (dexcom_persistent_client_is_open()) {
            dexcom_close_persistent_client();
            ESP_LOGI(TAG, "Closed Dexcom SSL for search (heap: %lu)", esp_get_free_heap_size());
        }
        if (libre_persistent_client_is_open()) {
            libre_close_persistent_client();
            ESP_LOGI(TAG, "Closed Libre SSL for search (heap: %lu)", esp_get_free_heap_size());
        }
        xSemaphoreGive(network_mutex);
    }

    location_search_in_progress = true;
    if (xTaskCreate(location_search_task, "loc_search", 4096, NULL, 2, NULL) != pdPASS) {
        location_search_in_progress = false;
        ESP_LOGE(TAG, "Failed to create search task");
        // Remove search overlay on failure
        if (search_overlay != NULL) {
            lv_obj_del(search_overlay);
            search_overlay = NULL;
        }
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

// The background search task writes to hint_label and search_overlay after its
// HTTP round-trip. NULL the statics on EVERY delete path, including teardowns
// this file does not initiate, or that task writes into freed pool memory.
static void search_screen_delete_cb(lv_event_t *e) {
    // A deferred delete of a superseded screen must not disown the live one's
    // widgets, so only the screen the global still points at clears them.
    if (lv_event_get_target(e) != screen_zipcode_entry) return;
    clear_search_state();
}

static void create_location_search_screen(void) {
    if (screen_zipcode_entry != NULL) {
        lv_obj_del(screen_zipcode_entry);
        screen_zipcode_entry = NULL;
    }

    clear_search_state();

    tw_init_styles();

    // This builder plus its deferred keyboard is one of the heaviest
    // allocations on the device; measure before committing to it.
    bool degraded = tw_pool_low("location search");

    screen_zipcode_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_zipcode_entry, &style_bg, 0);
    lv_obj_clear_flag(screen_zipcode_entry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_zipcode_entry, search_screen_delete_cb,
                        LV_EVENT_DELETE, NULL);

    tw_std_back_btn(screen_zipcode_entry, location_back_cb, NULL);

    // ---- Title ----
    lv_obj_t *title = lv_label_create(screen_zipcode_entry);
    lv_label_set_text(title, "Search Location");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // search_textarea stays NULL, which also stops add_search_keyboard().
    if (degraded) {
        tw_add_low_memory_notice(screen_zipcode_entry);
        ESP_LOGW(TAG, "Location search screen created (degraded)");
        return;
    }

    // ---- Search field card ----
    lv_obj_t *search_card = lv_obj_create(screen_zipcode_entry);
    lv_obj_set_size(search_card, 300, 38);
    lv_obj_align(search_card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(search_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(search_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(search_card, 10, 0);
    lv_obj_set_style_border_width(search_card, 1, 0);
    lv_obj_set_style_border_color(search_card, lv_color_hex(COLOR_MODAL_BORDER), 0);
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
    lv_obj_set_style_border_color(result_card, lv_color_hex(COLOR_MODAL_BORDER), 0);
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

    // Result location name — large, prominent
    result_name_label = lv_label_create(result_card);
    lv_label_set_text(result_name_label, "");
    lv_obj_set_style_text_font(result_name_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(result_name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_width(result_name_label, 200);
    lv_obj_align(result_name_label, LV_ALIGN_LEFT_MID, 12, -16);

    // Result admin/country/timezone — smaller, secondary
    result_detail_label = lv_label_create(result_card);
    lv_label_set_text(result_detail_label, "");
    lv_obj_set_style_text_font(result_detail_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(result_detail_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_width(result_detail_label, 200);
    lv_obj_align(result_detail_label, LV_ALIGN_LEFT_MID, 12, 12);

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
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    cygm_apply_ghost_btn(prev_btn);
    // Prev and Next abut: any touch-box growth would cross-select.
    lv_obj_set_ext_click_area(prev_btn, 0);
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
    lv_obj_set_style_border_width(next_btn, 0, 0);
    cygm_apply_ghost_btn(next_btn);
    lv_obj_set_ext_click_area(next_btn, 0);
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
    lv_obj_set_style_border_width(confirm_btn, 0, 0);
    cygm_apply_ghost_btn(confirm_btn);
    // Only 6px separates this pair; keep their touch boxes from crossing.
    lv_obj_set_ext_click_area(confirm_btn, 2);
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
    cygm_apply_ghost_btn(new_search_btn);
    lv_obj_set_ext_click_area(new_search_btn, 2);
    lv_obj_add_flag(new_search_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ns_lbl = lv_label_create(new_search_btn);
    lv_label_set_text(ns_lbl, LV_SYMBOL_REFRESH " New Search");
    lv_obj_set_style_text_font(ns_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ns_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(ns_lbl);
    lv_obj_add_event_cb(new_search_btn, location_new_search_cb, LV_EVENT_CLICKED, NULL);

    // The keyboard is deferred to add_search_keyboard(), which must run only
    // after the settings screen is freed — the 36KB pool cannot hold
    // Home + Menu + Settings + Search + Keyboard at once.

    ESP_LOGI(TAG, "Location search screen created (keyboard deferred)");
}

// Split out because the keyboard's ~3KB allocation only succeeds once the
// settings screen is freed; overflowing the pool trips LV_ASSERT_MALLOC's
// while(1) and freezes the device.
static void add_search_keyboard(void) {
    // A degraded search screen has no textarea — there is nothing to type into.
    if (search_keyboard != NULL || screen_zipcode_entry == NULL ||
        search_textarea == NULL) {
        return;
    }

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
    lv_obj_set_style_bg_color(search_keyboard, lv_color_hex(COLOR_MODAL_BG), 0);
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

void create_zipcode_entry_screen(void) {
    create_location_search_screen();
    add_search_keyboard();
}

// ==================== Settings Screen Handlers ====================

static void location_search_event_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Opening location search");

    // Order matters: build the search screen without its keyboard, load it,
    // free the settings screen to reclaim ~4KB of pool, and only then create
    // the keyboard.
    create_location_search_screen();
    lv_scr_load(screen_zipcode_entry);

    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }

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

    // Load home FIRST: LVGL needs the current screen alive during the swap.
    lv_scr_load(screen_home);
    home_screen_active = true;
    pause_background_tasks = false;

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

// Tear the overlay down without navigating. The countdown timer outlives the
// widgets it writes to, so it must die first.
static void tw_confirm_overlay_dismiss(void) {
    if (confirm_timer != NULL) {
        lv_timer_del(confirm_timer);
        confirm_timer = NULL;
    }
    if (confirm_overlay != NULL) {
        lv_obj_del(confirm_overlay);
        confirm_overlay = NULL;
    }
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
    tw_confirm_overlay_dismiss();

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
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
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
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIVIDER), 0);
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
    cygm_apply_ghost_btn(close_btn);

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

// Fires on every teardown, including ones this file does not initiate (the
// inactivity watchdog). The countdown timer outlives the overlay it writes to,
// which is a child of this screen, so kill it here or it reads freed memory.
static void settings_screen_delete_cb(lv_event_t *e) {
    (void)e;

    if (confirm_timer != NULL) {
        lv_timer_del(confirm_timer);
        confirm_timer = NULL;
    }
    confirm_overlay = NULL;
}

// ==================== Settings Cards ====================

// Timezone + location + the search entry point. Screen rows 46..164.
// Rows within the 294x116 content box:
//   TIMEZONE header   3..18     timezone value  18..34
//   divider          37..38
//   LOCATION header  41..56     location value  56..72
//   search button    76..110
// Each header pair and value pair shares one label, so the 38px stride below
// is what separates the stacked lines.
static void create_location_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 118);
    // 46 is the floor: the card is 296 wide, so anything higher runs under the
    // back button, which is drawn to y=40.
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_add_style(card, &s_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Blue accent bar, bracketing the two text rows.
    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_add_style(accent, &s_accent, 0);
    lv_obj_set_size(accent, 3, 64);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);

    lv_obj_t *headers = tw_stacked_label(card, &s_section,
                                         &lv_font_montserrat_12, 38);
    lv_label_set_text(headers, "TIMEZONE\nLOCATION");
    lv_obj_align(headers, LV_ALIGN_TOP_LEFT, 16, 3);

    // Both values are read once at build time and never updated in place, so
    // they can share one object.
    lv_obj_t *values = tw_stacked_label(card, &s_value,
                                        &lv_font_montserrat_14, 38);
    lv_label_set_text_fmt(values, "%s\n%s",
                          strlen(user_timezone) > 0 ? user_timezone : "Not set",
                          strlen(user_location) > 0 ? user_location : "Not set");
    lv_obj_align(values, LV_ALIGN_TOP_LEFT, 16, 18);

    // Divider — sits in the blank band between the two value lines.
    lv_obj_t *div1 = lv_obj_create(card);
    lv_obj_remove_style_all(div1);
    lv_obj_add_style(div1, &s_rule, 0);
    lv_obj_set_size(div1, 264, 1);
    lv_obj_align(div1, LV_ALIGN_TOP_MID, 0, 37);

    lv_obj_t *search_btn = tw_chip(card, LV_SYMBOL_REFRESH " Search Location",
                                   &lv_font_montserrat_14, COLOR_ACCENT_LIGHT,
                                   264, 34);
    lv_obj_align(search_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(search_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(search_btn, LV_OPA_20, 0);
    lv_obj_add_event_cb(search_btn, location_search_event_cb, LV_EVENT_CLICKED, NULL);
}

// Weather refresh cadence. Screen rows 176..216.
// One row, not two: the 294x38 content box cannot stack a section header above
// the chip row, so the header sits beside the chips. Keep that caption short —
// a longer one leaves too little width for three legible options.
static void create_interval_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 296, 40);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_add_style(card, &s_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_add_style(accent, &s_accent, 0);
    lv_obj_set_size(accent, 3, 28);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ORANGE), 0);

    lv_obj_t *interval_label = lv_label_create(card);
    lv_label_set_text(interval_label, "UPDATE");
    lv_obj_add_style(interval_label, &s_section, 0);
    lv_obj_align(interval_label, LV_ALIGN_TOP_LEFT, 16, 11);

    // Three chips right-aligned as a group: 3*62 + 2*6 = 198, starting at x=88
    // to clear the caption and ending 8 short of the 294px content box.
    static const uint8_t intervals[] = {15, 30, 60};
    static const char *labels[] = {"15 min", "30 min", "1 hr"};
    const int btn_w = 62;
    const int btn_h = 26;
    const int btn_y = 6;
    const int start_x = 88;

    for (int i = 0; i < 3; i++) {
        bool is_active = (user_weather_interval_min == intervals[i]);

        lv_obj_t *btn = tw_chip(card, labels[i], &lv_font_montserrat_14,
                                is_active ? COLOR_TEXT_WHITE : COLOR_TEXT_DIM,
                                btn_w, btn_h);
        lv_obj_set_pos(btn, start_x + i * (btn_w + 6), btn_y);

        if (is_active) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            // Dropping the border grows the content box by a pixel, so restate
            // the centring or the selected chip's text loses the baseline.
            lv_obj_set_style_pad_top(btn,
                (btn_h - lv_font_get_line_height(&lv_font_montserrat_14)) / 2, 0);
        } else {
            lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_MODAL_BORDER), 0);
        }
        // Three abutting options: keep the touch boxes from overlapping.
        lv_obj_set_ext_click_area(btn, 3);

        lv_obj_add_event_cb(btn, weather_interval_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)intervals[i]);
    }
}

// ==================== Settings Screen ====================

// Back to the menu. The menu dispatcher deletes screen_menu after loading this
// screen, so recreate it before loading (loading a NULL screen crashes).
static void settings_back_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (screen_menu == NULL) create_menu_screen();
    lv_scr_load(screen_menu);
    if (screen_time_weather_settings != NULL) {
        lv_obj_del_async(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }
}

void create_time_weather_settings_screen(void) {
    if (screen_time_weather_settings != NULL) {
        lv_obj_del(screen_time_weather_settings);
        screen_time_weather_settings = NULL;
    }

    tw_init_styles();

    // Measured after the old screen is freed, so it reflects what this build
    // actually has to work with.
    bool degraded = tw_pool_low("time/weather settings");

    screen_time_weather_settings = lv_obj_create(NULL);
    lv_obj_add_style(screen_time_weather_settings, &style_bg, 0);
    lv_obj_clear_flag(screen_time_weather_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_time_weather_settings, settings_screen_delete_cb,
                        LV_EVENT_DELETE, NULL);

    tw_std_back_btn(screen_time_weather_settings, settings_back_cb, NULL);

    // Title wording is shared with the menu row that opens this screen; keep
    // the two in step.
    lv_obj_t *header = lv_label_create(screen_time_weather_settings);
    lv_label_set_text(header, "Time / Weather");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 6);

    // Nothing below here is worth the risk of an LV_ASSERT_MALLOC while(1):
    // the user keeps a titled screen and a working back button instead.
    if (degraded) {
        tw_add_low_memory_notice(screen_time_weather_settings);
        return;
    }

    // Both cards carry their own screen coordinates (46..164 and 176..216).
    // Nothing scrolls or pages, so no container earns its pool cost.
    create_location_card(screen_time_weather_settings);
    create_interval_card(screen_time_weather_settings);
}
