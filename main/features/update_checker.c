/*
 * update_checker.c
 *
 * Checks version.json for newer firmware and, when a firmware_url is present,
 * downloads and flashes it over OTA.
 *
 * The manifest is fetched over plain HTTP — a second TLS session alongside the
 * provider client does not fit in contiguous heap — so everything in it is
 * attacker-controlled, and the document must stay inside HTTP_BUF_SIZE or it is
 * cut and never parses. firmware_url is therefore pinned to FIRMWARE_URL_PREFIX
 * (traversal rejected, since the origin server would normalise it away) and the
 * download itself runs over TLS with the ESP-IDF root bundle attached and
 * redirects refused, so a rewritten manifest cannot substitute an image that is
 * not ours. It could still name an OLDER image of ours, so the version baked
 * into the downloaded app descriptor is checked against this build before the
 * boot partition is switched.
 */

#include "update_checker.h"
#include "main.h"
#include "shared_state.h"
#include "sd_logger.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "time_system.h"
#include "weather_system.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "UPDATE";

#define UPDATE_CHECK_URL "http://cygm.me/version.json"
// Firmware may only ever be fetched from our own origin, over TLS.
#define FIRMWARE_URL_PREFIX "https://cygm.me/firmware/"
#define UPDATE_INTERVAL_MS (24LL * 60 * 60 * 1000)  // 24 hours
// version.json is the four contract fields, ~140 bytes. The server side is held
// to that (sync-site-version.ps1 refuses a larger file) because a document that
// does not fit here is silently cut and never parses: on 2026-09-06 a 555-byte
// version.json left every fielded device unable to see updates. Room to grow,
// and the truncation is reported instead of hidden.
#define HTTP_BUF_SIZE 768

static update_info_t update_info = {0};
static int64_t last_check_ms = 0;

static char http_buf[HTTP_BUF_SIZE];
static int http_buf_len = 0;
static bool http_buf_truncated = false;

// Set true by manual check task while polling for Install button.
// Prevents fallback task creation (its 4KB stack fragments the contiguous block).
static volatile bool manual_check_polling = false;

// Update overlay
static lv_obj_t *update_overlay = NULL;

// OTA state
static volatile bool ota_active = false;
static volatile int ota_percent = 0;

// OTA progress overlay widgets
static lv_obj_t *ota_overlay = NULL;
static lv_obj_t *ota_bar = NULL;
static lv_obj_t *ota_pct_label = NULL;
static lv_obj_t *ota_status_label = NULL;

// ==================== HTTP ====================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (http_buf_len + copy >= HTTP_BUF_SIZE - 1) {
            copy = HTTP_BUF_SIZE - 1 - http_buf_len;
            http_buf_truncated = true;
        }
        if (copy > 0) {
            memcpy(http_buf + http_buf_len, evt->data, copy);
            http_buf_len += copy;
            http_buf[http_buf_len] = '\0';
        }
    }
    return ESP_OK;
}

void update_set_manual_polling(bool polling) {
    manual_check_polling = polling;
}

// ==================== Check Logic ====================

bool update_should_check(void) {
    // Don't check within first 5 minutes of boot — let heap stabilize first.
    // Early OTA checks fail with -0x7F00 (ALLOC_FAILED) because the heap is
    // still fragmented from boot-time SSL + task allocation churn.
    int64_t uptime_us = esp_timer_get_time();
    if (uptime_us < 300000000LL) return false;  // 300s = 5 minutes

    if (last_check_ms == 0) return true;  // Never checked
    int64_t now_ms = uptime_us / 1000;
    return (now_ms - last_check_ms) >= UPDATE_INTERVAL_MS;
}

esp_err_t update_check_now(void) {
    http_buf_len = 0;
    http_buf[0] = '\0';
    http_buf_truncated = false;

    ESP_LOGI(TAG, "Checking %s ...", UPDATE_CHECK_URL);

    esp_http_client_config_t config = {
        .url = UPDATE_CHECK_URL,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 512,
        // Plain HTTP, to avoid a 6.7KB TLS buffer allocation that fails under
        // heap fragmentation. This document is NOT merely version numbers: it
        // carries firmware_url, which decides what gets installed. That field is
        // pinned to FIRMWARE_URL_PREFIX below, and the download itself verifies
        // the certificate, so a tampered manifest cannot substitute firmware.
        //
        // Redirects are refused rather than followed. http_buf is filled by the
        // event handler across a whole perform, so a followed 3xx would prepend
        // its body to the JSON and every device would report a parse failure
        // with no hint why; refusing turns that into a plain HTTP-status error.
        // cygm.me must keep serving this path without a redirect.
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    last_check_ms = esp_timer_get_time() / 1000;

    if (err != ESP_OK || status != 200) {
        sd_log(TAG, "Check failed: %s (HTTP %d)", esp_err_to_name(err), status);
        ESP_LOGW(TAG, "Check failed: %s (HTTP %d)", esp_err_to_name(err), status);
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(http_buf);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed (%d bytes%s)", http_buf_len,
                 http_buf_truncated ? ", response truncated to buffer size" : "");
        sd_log(TAG, "JSON parse failed (%d bytes%s)", http_buf_len,
               http_buf_truncated ? ", truncated" : "");
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    cJSON *ver   = cJSON_GetObjectItem(json, "version");
    cJSON *stage = cJSON_GetObjectItem(json, "stage");
    cJSON *date  = cJSON_GetObjectItem(json, "date");
    cJSON *fw_url = cJSON_GetObjectItem(json, "firmware_url");

    if (!ver || !cJSON_IsString(ver)) {
        cJSON_Delete(json);
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    snprintf(update_info.version, sizeof(update_info.version), "%s", ver->valuestring);
    if (stage && cJSON_IsString(stage))
        snprintf(update_info.stage, sizeof(update_info.stage), "%s", stage->valuestring);
    else
        update_info.stage[0] = '\0';
    if (date && cJSON_IsString(date))
        snprintf(update_info.date, sizeof(update_info.date), "%s", date->valuestring);
    else
        update_info.date[0] = '\0';
    // version.json arrives over plain HTTP, so every field in it is
    // attacker-controlled. This one decides which bytes get executed, so pin it
    // to our own origin: a rewritten manifest can then lie about the version
    // number but cannot point the device at somebody else's firmware. A prefix
    // test alone does not confine the fetch to /firmware/, because the origin
    // server resolves "../" before it looks at the path, so traversal is a
    // separate rejection.
    if (fw_url && cJSON_IsString(fw_url) &&
        strncmp(fw_url->valuestring, FIRMWARE_URL_PREFIX, strlen(FIRMWARE_URL_PREFIX)) == 0 &&
        strstr(fw_url->valuestring, "..") == NULL) {
        snprintf(update_info.firmware_url, sizeof(update_info.firmware_url), "%s", fw_url->valuestring);
    } else {
        if (fw_url && cJSON_IsString(fw_url)) {
            ESP_LOGW(TAG, "Rejecting firmware_url outside %s", FIRMWARE_URL_PREFIX);
        }
        update_info.firmware_url[0] = '\0';
    }

    int rmaj = 0, rmin = 0, rpat = 0;
    sscanf(ver->valuestring, "%d.%d.%d", &rmaj, &rmin, &rpat);

    update_info.available =
        (rmaj > CYGM_VERSION_MAJOR) ||
        (rmaj == CYGM_VERSION_MAJOR && rmin > CYGM_VERSION_MINOR) ||
        (rmaj == CYGM_VERSION_MAJOR && rmin == CYGM_VERSION_MINOR && rpat > CYGM_VERSION_PATCH);

    update_info.checked = true;
    update_info.check_failed = false;

    cJSON_Delete(json);

    ESP_LOGI(TAG, "%s (remote v%s, local v%d.%d.%d)",
             update_info.available ? "UPDATE AVAILABLE" : "Up to date",
             update_info.version,
             CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);

    if (update_info.firmware_url[0])
        ESP_LOGI(TAG, "OTA URL: %s", update_info.firmware_url);

    sd_log(TAG, "%s (remote v%s, local v%d.%d.%d)",
           update_info.available ? "UPDATE AVAILABLE" : "Up to date",
           update_info.version,
           CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);

    return ESP_OK;
}

const update_info_t *update_get_info(void) {
    return &update_info;
}

// ==================== OTA Progress Overlay ====================

void show_ota_overlay(const char *status) {
    if (ota_overlay != NULL) return;

    ota_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ota_overlay);
    lv_obj_set_size(ota_overlay, 320, 240);
    lv_obj_set_style_bg_color(ota_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // NOT clickable — can't dismiss during OTA

    lv_obj_t *card = lv_obj_create(ota_overlay);
    lv_obj_set_size(card, 280, 160);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_DOWNLOAD " Updating Firmware");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    ota_status_label = lv_label_create(card);
    lv_label_set_text(ota_status_label, status);
    lv_obj_set_style_text_color(ota_status_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(ota_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ota_status_label, LV_ALIGN_TOP_MID, 0, 42);

    ota_bar = lv_bar_create(card);
    lv_obj_set_size(ota_bar, 220, 14);
    lv_obj_align(ota_bar, LV_ALIGN_CENTER, 0, 10);
    lv_bar_set_range(ota_bar, 0, 100);
    lv_bar_set_value(ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x1e2d3d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ota_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ota_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ota_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ota_bar, 7, LV_PART_INDICATOR);

    ota_pct_label = lv_label_create(card);
    lv_label_set_text(ota_pct_label, "0%");
    lv_obj_set_style_text_color(ota_pct_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(ota_pct_label, &lv_font_montserrat_20, 0);
    lv_obj_align(ota_pct_label, LV_ALIGN_CENTER, 0, 38);

    lv_obj_t *warn = lv_label_create(card);
    lv_label_set_text(warn, "Do not power off!");
    lv_obj_set_style_text_color(warn, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void dismiss_ota_overlay(void) {
    if (ota_overlay != NULL) {
        lv_obj_t *ov = ota_overlay;
        ota_overlay = NULL;
        ota_bar = NULL;
        ota_pct_label = NULL;
        ota_status_label = NULL;
        lv_obj_del(ov);
    }
}

static void ota_update_progress(int pct, const char *status) {
    if (lvgl_port_lock(1)) {
        if (ota_bar) lv_bar_set_value(ota_bar, pct, LV_ANIM_OFF);  // LV_ANIM_ON triggers lv_anim_start (freeze risk)
        if (ota_pct_label) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(ota_pct_label, buf);
        }
        if (status && ota_status_label) {
            lv_label_set_text(ota_status_label, status);
        }
        lvgl_port_unlock();
    }
}

static void ota_result_dismiss_cb(lv_event_t *e) {
    (void)e;
    dismiss_ota_overlay();
}

static void ota_result_timer_cb(lv_timer_t *timer) {
    (void)timer;
    dismiss_ota_overlay();
}

static void ota_show_result(bool success, const char *msg) {
    bool locked = false;
    for (int retry = 0; retry < 50 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (locked) {
        dismiss_ota_overlay();

        ota_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(ota_overlay);
        lv_obj_set_size(ota_overlay, 320, 240);
        lv_obj_set_style_bg_color(ota_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_80, 0);
        lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_SCROLLABLE);

        // Tap backdrop to dismiss (failure only — success restarts)
        if (!success) {
            lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(ota_overlay, ota_result_dismiss_cb, LV_EVENT_CLICKED, NULL);
        }

        lv_obj_t *card = lv_obj_create(ota_overlay);
        lv_obj_set_size(card, 270, 130);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_color(card, success ? lv_color_hex(COLOR_GREEN) : lv_color_hex(COLOR_RED), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, success ? LV_SYMBOL_OK " Update Complete!" : LV_SYMBOL_WARNING " Update Failed");
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(icon, success ? lv_color_hex(COLOR_GREEN) : lv_color_hex(COLOR_RED), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 18);

        lv_obj_t *detail = lv_label_create(card);
        lv_label_set_text(detail, msg);
        lv_obj_set_style_text_color(detail, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(detail, 240);
        lv_obj_align(detail, LV_ALIGN_CENTER, 0, 6);

        if (!success) {
            lv_obj_t *hint = lv_label_create(card);
            lv_label_set_text(hint, "Tap to dismiss");
            lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

            // Auto-dismiss after 10 seconds
            lv_timer_t *t = lv_timer_create(ota_result_timer_cb, 10000, NULL);
            lv_timer_set_repeat_count(t, 1);
        }

        lvgl_port_unlock();
    }
}

// ==================== Memory Management ====================

void update_prepare_memory(void) {
    ESP_LOGI(TAG, "Freeing memory for update operation...");
    size_t before = esp_get_free_heap_size();

    // 1. Pause background tasks (skip network ops on next loop)
    pause_background_tasks = true;

    // 2. Close Dexcom/Libre persistent SSL connection (~15-20KB freed)
    dexcom_close_persistent_client();
    libre_close_persistent_client();

    // 3. Wait for network mutex — ensures no task is mid-SSL-operation
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
        xSemaphoreGive(network_mutex);
    } else {
        ESP_LOGW(TAG, "Could not acquire network mutex (10s timeout)");
    }

    // 4. Delete weather/time/battery tasks — frees 11KB of scattered stacks
    //    so TLSF can coalesce with the Dexcom-freed block, growing largest_block.
    delete_background_tasks_for_ssl("version-check");

    size_t after = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "Memory freed: %lu -> %lu bytes (+%lu), largest_block=%lu",
             (unsigned long)before, (unsigned long)after,
             (unsigned long)(after - before), (unsigned long)largest);
}

void update_restore_tasks(void) {
    recreate_background_tasks();
    pause_background_tasks = false;
    home_screen_active = true;
    ESP_LOGI(TAG, "Background tasks restored (heap=%lu)", (unsigned long)esp_get_free_heap_size());
}

void update_stop_all_tasks(void) {
    ESP_LOGI(TAG, "=== STOPPING ALL TASKS FOR OTA ===");
    size_t before = esp_get_free_heap_size();

    // 1. Pause first so tasks skip network ops on their next loop iteration
    pause_background_tasks = true;

    // 2. Close Dexcom/Libre persistent SSL (may already be closed — that's fine)
    dexcom_close_persistent_client();
    libre_close_persistent_client();

    // 3. Acquire + release network mutex to ensure no task is mid-operation
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
        xSemaphoreGive(network_mutex);
    }

    // 4. Wait for tasks to reach their sleep state
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 5. Stop ALL non-essential tasks to maximise contiguous heap. Apache sends
    //    16384-byte TLS records, so the dynamic SSL IN buffer needs ~17KB
    //    contiguous, and each freed stack can coalesce with its neighbours
    //    (weather 8K + glucose 4K + time 4K + battery 3K + wifi 8K + ui 8K ~ 35KB).
    //    Safe to lose them all because the WiFi driver and the LVGL port each run
    //    their own task.
    //
    //    Every one of these six takes the LVGL lock, and glucose additionally
    //    holds network_mutex across a whole fetch. FreeRTOS does not release a
    //    mutex owned by a deleted task, so a delete landing inside one of those
    //    windows freezes the display or the network permanently. The three tasks
    //    that have a cooperative park park themselves; the remaining three are
    //    deleted while this function holds the LVGL lock, which is what proves
    //    they are not inside theirs. If a stop or the lock does not come through,
    //    the task is left running: less contiguous heap means the OTA fails and
    //    the device reboots, which is recoverable, and a freeze is not.
    //
    //    weather_task_request_stop() latches its flag with no handle guard, and
    //    the weather task does not clear it at entry, so asking a task that is
    //    already gone to stop would park the next one created. Every caller has
    //    already parked weather via delete_background_tasks_for_ssl(), so that
    //    is the common case, not the rare one.
    bool had_weather = (weather_task_handle != NULL);
    bool had_time    = (time_task_handle != NULL);
    bool had_glucose = (glucose_task_handle != NULL);

    if (had_weather) weather_task_request_stop();
    glucose_task_request_stop();
    time_task_request_stop();

    // wifi_task_core0 calls ensure_tasks_running() on its reconnect path, and
    // that recreates any task whose handle is NULL with no knowledge that a
    // teardown is in progress. Deleting it before the park polls below keeps a
    // reconnect inside those seconds from resurrecting everything just parked.
    //
    // battery, wifi and ui have no park of their own yet. Holding the LVGL lock
    // is what makes deleting them safe: a task inside its own lock window cannot
    // be here, and one blocked on the 1 ms try-lock owns nothing.
    bool ui_locked = false;
    for (int retry = 0; retry < 50 && !ui_locked; retry++) {
        ui_locked = lvgl_port_lock(1);
        if (!ui_locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ui_locked) {
        if (battery_task_handle != NULL) {
            vTaskDelete(battery_task_handle);
            battery_task_handle = NULL;
            ESP_LOGI(TAG, "Deleted battery task (3KB stack freed)");
        }
        if (wifi_task_handle != NULL) {
            vTaskDelete(wifi_task_handle);
            wifi_task_handle = NULL;
            ESP_LOGI(TAG, "Deleted wifi task (8KB stack freed)");
        }
        if (ui_task_handle != NULL) {
            vTaskDelete(ui_task_handle);
            ui_task_handle = NULL;
            ESP_LOGI(TAG, "Deleted ui task (8KB stack freed)");
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "LVGL lock unavailable; battery/wifi/ui tasks left running");
    }

    if (had_weather) {
        for (int i = 0; i < 20 && weather_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (weather_task_handle == NULL) {
            ESP_LOGI(TAG, "Weather task parked (8KB stack freed)");
        } else {
            ESP_LOGW(TAG, "Weather task did not park; leaving it running");
        }
    }

    if (had_time) {
        for (int i = 0; i < 20 && !time_task_is_stopped(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (time_task_is_stopped()) {
            ESP_LOGI(TAG, "Time task parked (4KB stack freed)");
        } else {
            ESP_LOGW(TAG, "Time task did not park; leaving it running");
        }
    }

    // The glucose task can be most of a 90 s cycle deep in a provider fetch, so
    // it gets the long wait.
    if (had_glucose) {
        for (int i = 0; i < 150 && !glucose_task_is_stopped(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (glucose_task_is_stopped()) {
            ESP_LOGI(TAG, "Glucose task parked (4KB stack freed)");
        } else {
            ESP_LOGW(TAG, "Glucose task did not park; leaving it running");
        }
    }

    // 6. Unmount SD card (persistent mount) — frees ~1.5KB, helps coalesce heap
    sd_logger_shutdown();

    // 7. Yield to IDLE task so it frees the deleted tasks' memory
    vTaskDelay(pdMS_TO_TICKS(500));

    // 8. Disable WiFi power save for reliable OTA download
    esp_wifi_set_ps(WIFI_PS_NONE);

    size_t after = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "OTA memory ready: %lu -> %lu bytes (+%lu), largest_block=%lu (8bit=%lu)",
             (unsigned long)before, (unsigned long)after,
             (unsigned long)(after - before), (unsigned long)largest,
             (unsigned long)largest_8bit);
    sd_log(TAG, "OTA prep: heap=%lu, largest=%lu, 8bit=%lu (+%lu freed)",
           (unsigned long)after, (unsigned long)largest,
           (unsigned long)largest_8bit, (unsigned long)(after - before));
}

// ==================== OTA Download (runs inside caller's task) ====================

// Signal flag: set by Install button callback, polled by check task
static volatile bool ota_install_requested = false;

// Context for Range-based OTA download event handler
typedef struct {
    esp_ota_handle_t ota_handle;
    int bytes_written;
    int chunk_offset;
    bool write_error;
} ota_range_ctx_t;

// Writes incoming HTTP data directly to OTA partition
static esp_err_t ota_range_http_handler(esp_http_client_event_t *evt) {
    ota_range_ctx_t *ctx = (ota_range_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && !ctx->write_error) {
        // Only a 206 body is firmware, except at offset 0 where a server that
        // ignores the Range header legitimately answers 200 with the whole file.
        // A 200 anywhere else is that same whole file starting at byte 0, and
        // esp_ota_write is sequential, so admitting it would shift every chunk
        // after it. A redirect or error page carries a body too.
        int status = esp_http_client_get_status_code(evt->client);
        if (status != 206 && !(status == 200 && ctx->chunk_offset == 0)) {
            ESP_LOGE(TAG, "Refusing body from HTTP %d at offset %d", status, ctx->chunk_offset);
            ctx->write_error = true;
            return ESP_OK;
        }
        esp_err_t err = esp_ota_write(ctx->ota_handle, evt->data, evt->data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            ctx->write_error = true;
        } else {
            ctx->bytes_written += evt->data_len;
        }
    }
    return ESP_OK;
}

#define OTA_RANGE_CHUNK     4096
#define OTA_MAX_RETRIES     5

void update_do_ota_download(void) {
    ota_active = true;

    // The download is the one client that verifies certificates, and with
    // MBEDTLS_HAVE_TIME_DATE every certificate is checked against the clock, so
    // an unsynced clock makes every chain read as not-yet-valid. Caught here the
    // cause is in the log; left to mbedTLS it is an opaque handshake error on
    // the one path that could have shipped a fix.
    time_t clock_now;
    time(&clock_now);
    if (clock_now < 1700000000) {
        ESP_LOGE(TAG, "Clock not set (%lld); TLS would reject every certificate",
                 (long long)clock_now);
        sd_log(TAG, "OTA aborted: clock not set (%lld)", (long long)clock_now);
        ota_show_result(false, "Clock not set.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "OTA Range download: %s", update_info.firmware_url);
    sd_log(TAG, "OTA started (Range): %s", update_info.firmware_url);
    ota_update_progress(0, "Connecting...");

    size_t free_heap = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "OTA memory: heap=%lu, largest_block=%lu",
             (unsigned long)free_heap, (unsigned long)largest);

    // ---- HTTP client (reused via keep-alive for all Range chunks) ----
    ota_range_ctx_t ctx = {0};
    esp_http_client_config_t http_config = {
        .url = update_info.firmware_url,
        .event_handler = ota_range_http_handler,
        .user_data = &ctx,
        .timeout_ms = 30000,
        .buffer_size = 1536,
        .buffer_size_tx = 512,
        .keep_alive_enable = true,
        // Verify the server against the ESP-IDF root bundle. Without this,
        // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY leaves the download encrypted
        // but unauthenticated: anything able to intercept the connection can
        // serve firmware. cygm.me chains to USERTrust RSA, which is in the
        // bundle, so verification succeeds.
        .crt_bundle_attach = esp_crt_bundle_attach,
        // A followed redirect would land outside FIRMWARE_URL_PREFIX and defeat
        // the pin, so refuse them and let the 3xx surface as a failure.
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        ota_show_result(false, "Connection failed.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    // ---- HEAD request: get firmware size ----
    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    // Content-Length is int64_t; narrowing it before the range check would let
    // a value whose low 32 bits are small pass as a plausible size.
    int64_t content_len = esp_http_client_get_content_length(client);

    ESP_LOGI(TAG, "HEAD: status=%d, size=%lld", status, (long long)content_len);

    if (err != ESP_OK || status != 200 || content_len <= 0) {
        ESP_LOGE(TAG, "HEAD failed: %s (HTTP %d, size=%lld)",
                 esp_err_to_name(err), status, (long long)content_len);
        sd_log(TAG, "OTA HEAD failed: HTTP %d, size=%lld", status, (long long)content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_show_result(false, "Connection failed.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    // ---- Prepare flash partition ----
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_show_result(false, "No update partition.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    ESP_LOGI(TAG, "Target: %s (size=%lu)",
             update_partition->label, (unsigned long)update_partition->size);

    if (content_len > (int64_t)update_partition->size) {
        ESP_LOGE(TAG, "Firmware %lld bytes exceeds partition %lu",
                 (long long)content_len, (unsigned long)update_partition->size);
        sd_log(TAG, "OTA rejected: %lld bytes > partition", (long long)content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_show_result(false, "Image too large.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    int firmware_size = (int)content_len;
    sd_log(TAG, "Firmware: %d bytes", firmware_size);

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, (size_t)firmware_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        sd_log(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_show_result(false, "Flash prepare failed.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }

    ctx.ota_handle = ota_handle;

    // ---- Download in Range chunks ----
    //
    // Apache sends 16384-byte TLS records for large files, but the largest
    // contiguous block here is ~16384 bytes and mbedTLS DYNAMIC_BUFFER needs
    // record_size + ~365 on top, so a full record fails to allocate. Range requests
    // cap each response at OTA_RANGE_CHUNK (~4.5KB records), and keep-alive reuses
    // the TLS session so the whole download costs one handshake.

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    ota_update_progress(0, "Downloading...");

    int offset = 0;
    int last_pct = -1;
    bool download_ok = true;
    int retries = 0;

    while (offset < firmware_size) {
        int chunk_end = offset + OTA_RANGE_CHUNK - 1;
        if (chunk_end >= firmware_size) chunk_end = firmware_size - 1;

        char range_hdr[48];
        snprintf(range_hdr, sizeof(range_hdr), "bytes=%d-%d", offset, chunk_end);
        esp_http_client_set_header(client, "Range", range_hdr);

        ctx.bytes_written = 0;
        ctx.chunk_offset = offset;
        ctx.write_error = false;

        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);

        if (err != ESP_OK || (status != 206 && !(status == 200 && offset == 0)) ||
            ctx.write_error || ctx.bytes_written == 0) {
            retries++;
            ESP_LOGW(TAG, "Chunk fail @%d: err=%s status=%d written=%d (%d/%d)",
                     offset, esp_err_to_name(err), status, ctx.bytes_written,
                     retries, OTA_MAX_RETRIES);

            if (retries >= OTA_MAX_RETRIES) {
                ESP_LOGE(TAG, "Too many failures at %d/%d", offset, firmware_size);
                download_ok = false;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1000 * retries));

            // Recreate client after 3 failures (fresh TLS connection)
            if (retries >= 3) {
                ESP_LOGW(TAG, "Recreating HTTP client...");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                vTaskDelay(pdMS_TO_TICKS(500));
                client = esp_http_client_init(&http_config);
                if (!client) {
                    download_ok = false;
                    break;
                }
                esp_http_client_set_method(client, HTTP_METHOD_GET);
            }
            continue;
        }

        retries = 0;
        offset += ctx.bytes_written;

        int pct = offset * 100 / firmware_size;
        if (pct != last_pct) {
            ota_percent = pct;
            ota_update_progress(pct, "Downloading...");
            last_pct = pct;
            if (pct % 10 == 0) {
                ESP_LOGI(TAG, "OTA: %d%% (%d/%d, heap=%lu)", pct, offset, firmware_size,
                         (unsigned long)esp_get_free_heap_size());
            }
        }
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    // ---- Verify and finalize ----
    if (download_ok && offset >= firmware_size) {
        ota_update_progress(100, "Verifying...");
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            // The manifest is plain HTTP, so the version it advertised proves
            // nothing. The descriptor now on flash is what will actually run, so
            // that is what has to be newer — otherwise a rewritten manifest could
            // name a genuine older image of ours and roll the device back onto
            // whatever that build's defects were.
            esp_app_desc_t new_desc;
            esp_err_t desc_err = esp_ota_get_partition_description(update_partition, &new_desc);
            int nmaj = 0, nmin = 0, npat = 0;
            if (desc_err != ESP_OK ||
                sscanf(new_desc.version, "%d.%d.%d", &nmaj, &nmin, &npat) != 3) {
                ESP_LOGE(TAG, "Unreadable version in downloaded image (%s)",
                         esp_err_to_name(desc_err));
                sd_log(TAG, "OTA rejected: unreadable image version");
                err = ESP_FAIL;
            } else if (!((nmaj > CYGM_VERSION_MAJOR) ||
                         (nmaj == CYGM_VERSION_MAJOR && nmin > CYGM_VERSION_MINOR) ||
                         (nmaj == CYGM_VERSION_MAJOR && nmin == CYGM_VERSION_MINOR &&
                          npat > CYGM_VERSION_PATCH))) {
                ESP_LOGE(TAG, "Refusing downgrade: image v%d.%d.%d, running v%d.%d.%d",
                         nmaj, nmin, npat,
                         CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);
                sd_log(TAG, "OTA rejected: downgrade to v%d.%d.%d", nmaj, nmin, npat);
                err = ESP_FAIL;
            } else {
                err = esp_ota_set_boot_partition(update_partition);
            }
        }
    } else {
        esp_ota_abort(ota_handle);
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful! Restarting...");
        sd_log(TAG, "OTA complete (%d bytes), restarting", offset);
        sd_logger_flush();
        ota_show_result(true, "Restarting in 3 seconds...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s (%d/%d bytes)", esp_err_to_name(err), offset, firmware_size);
        sd_log(TAG, "OTA failed: %s at %d/%d", esp_err_to_name(err), offset, firmware_size);
        ota_show_result(false, "Download failed.\nRestarting device...");
        sd_logger_flush();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    ota_active = false;
}

// ==================== OTA Fallback Task ====================
// When the automatic update check (glucose task) shows the overlay and the user
// taps Install, no check task is polling. This fallback task handles that case.

static void ota_fallback_task_fn(void *param) {
    // Give the manual check task (if alive) 500ms to pick up the request
    vTaskDelay(pdMS_TO_TICKS(500));

    // If check task already cleared the flag or started OTA, we're not needed
    if (!ota_install_requested || ota_active) {
        ESP_LOGI(TAG, "OTA fallback: check task handled it, exiting");
        vTaskDelete(NULL);
        return;
    }

    // Nobody else is handling it — we do it
    ESP_LOGI(TAG, "OTA fallback: no check task, handling OTA directly");
    ota_install_requested = false;

    for (int retry = 0; retry < 50; retry++) {
        if (lvgl_port_lock(1)) {
            dismiss_update_overlay();
            show_ota_overlay("Stopping tasks...");
            lvgl_port_unlock();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // AGGRESSIVE cleanup: delete all non-essential tasks to maximize heap.
    // OTA needs ~20KB contiguous for TLS + HTTP buffers.
    update_stop_all_tasks();

    ota_update_progress(0, "Connecting...");

    // Run the OTA in this task. It reboots on success, and also on failure —
    // with every monitoring task deleted, a reboot is the cleanest recovery.
    update_do_ota_download();

    // If we get here, update_do_ota_download already handles reboot on failure
    vTaskDelete(NULL);
}

// ==================== Public OTA API ====================

void update_start_ota(void) {
    if (ota_active) return;
    if (!update_info.available || update_info.firmware_url[0] == '\0') return;

    ota_install_requested = true;

    if (manual_check_polling) {
        // Manual check task is alive and polling — it picks up the flag directly.
        // Do NOT create a fallback task here: its 4KB stack would fragment the
        // contiguous heap block needed for the OTA SSL buffer allocation.
        ESP_LOGI(TAG, "OTA install requested (check task will handle)");
    } else {
        // Automatic check: nothing is polling, so create a fallback task.
        ESP_LOGI(TAG, "OTA install requested (creating fallback task)");

        // Free the SSL clients and background task stacks first. With SSL alive the
        // largest block is ~3.5KB, short of the 6KB fallback stack; deleting tasks
        // frees ~11KB and brings it to ~8.7KB.
        dexcom_close_persistent_client();
        libre_close_persistent_client();
        delete_background_tasks_for_ssl("ota-install");

        BaseType_t rc = xTaskCreatePinnedToCore(
            ota_fallback_task_fn, "ota_fb", 6144, NULL, 5, NULL, 0);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA fallback task (heap=%lu, largest=%lu)",
                     esp_get_free_heap_size(),
                     heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            sd_log(TAG, "OTA fallback task FAILED: heap=%lu largest=%lu",
                   esp_get_free_heap_size(),
                   heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            ota_install_requested = false;
            // Recreate deleted tasks since OTA won't proceed
            recreate_background_tasks();
        }
    }
}

bool update_ota_install_requested(void) {
    return ota_install_requested;
}

void update_clear_ota_request(void) {
    ota_install_requested = false;
}

bool update_ota_in_progress(void) {
    return ota_active;
}

void update_confirm_boot(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA — confirming firmware");
            esp_ota_mark_app_valid_cancel_rollback();
            sd_log(TAG, "OTA boot confirmed (v%d.%d.%d)",
                   CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);
        }
    }
    ESP_LOGI(TAG, "Running from: %s (offset 0x%lx)",
             running->label, (unsigned long)running->address);
}

// ==================== Update Overlay UI ====================

static void update_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        dismiss_update_overlay();
    }
}

static void update_install_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        update_start_ota();
    }
}

void dismiss_update_overlay(void) {
    if (update_overlay != NULL) {
        lv_obj_t *ov = update_overlay;
        update_overlay = NULL;
        lv_obj_del_async(ov);
    }
}

void show_update_overlay(void) {
    if (update_overlay != NULL) return;

    const update_info_t *info = &update_info;
    bool has_ota = info->available && info->firmware_url[0] != '\0';

    // --- Dimmed backdrop ---
    update_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(update_overlay);
    lv_obj_set_size(update_overlay, 320, 240);
    lv_obj_set_style_bg_color(update_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(update_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(update_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_overlay, update_close_cb, LV_EVENT_CLICKED, NULL);

    // --- Card ---
    int card_h = info->available ? (has_ota ? 175 : 195) : 125;
    lv_obj_t *card = lv_obj_create(update_overlay);
    lv_obj_set_size(card, 270, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_add_event_cb(close_btn, update_close_cb, LV_EVENT_CLICKED, NULL);

    if (!info->checked || info->check_failed) {
        // --- Check Failed ---
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, LV_SYMBOL_WARNING " Check Failed");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ORANGE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

        lv_obj_t *msg = lv_label_create(card);
        lv_label_set_text(msg, "Could not reach server.\nCheck WiFi and try again.");
        lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 5);

    } else if (info->available) {
        // --- Update Available ---
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, LV_SYMBOL_DOWNLOAD " Update Available");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        char cur_buf[96];
        snprintf(cur_buf, sizeof(cur_buf), "Current:    v%d.%d.%d %s",
                 CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH,
                 CYGM_VERSION_STAGE);
        lv_obj_t *cur_lbl = lv_label_create(card);
        lv_label_set_text(cur_lbl, cur_buf);
        lv_obj_set_style_text_color(cur_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_align(cur_lbl, LV_ALIGN_TOP_LEFT, 20, 42);

        // Available version (green)
        char avail_buf[96];
        snprintf(avail_buf, sizeof(avail_buf), "Available: v%.15s %.15s",
                 info->version, info->stage);
        lv_obj_t *avail_lbl = lv_label_create(card);
        lv_label_set_text(avail_lbl, avail_buf);
        lv_obj_set_style_text_color(avail_lbl, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(avail_lbl, LV_ALIGN_TOP_LEFT, 20, 60);

        lv_obj_t *div = lv_obj_create(card);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, 230, 1);
        lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 82);

        if (has_ota) {
            // OTA available — show Install and Later buttons
            lv_obj_t *install_btn = lv_btn_create(card);
            lv_obj_set_size(install_btn, 110, 34);
            lv_obj_align(install_btn, LV_ALIGN_BOTTOM_MID, -60, -10);
            lv_obj_set_style_bg_color(install_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
            lv_obj_set_style_radius(install_btn, 8, 0);
            lv_obj_set_style_border_width(install_btn, 0, 0);
            lv_obj_set_style_shadow_width(install_btn, 0, 0);
            lv_obj_t *install_lbl = lv_label_create(install_btn);
            lv_label_set_text(install_lbl, LV_SYMBOL_DOWNLOAD " Install");
            lv_obj_set_style_text_color(install_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
            lv_obj_center(install_lbl);
            lv_obj_add_event_cb(install_btn, update_install_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_t *later_btn = lv_btn_create(card);
            lv_obj_set_size(later_btn, 90, 34);
            lv_obj_align(later_btn, LV_ALIGN_BOTTOM_MID, 60, -10);
            lv_obj_set_style_bg_color(later_btn, lv_color_hex(0x1e2d3d), 0);
            lv_obj_set_style_radius(later_btn, 8, 0);
            lv_obj_set_style_border_width(later_btn, 0, 0);
            lv_obj_set_style_shadow_width(later_btn, 0, 0);
            lv_obj_t *later_lbl = lv_label_create(later_btn);
            lv_label_set_text(later_lbl, "Later");
            lv_obj_set_style_text_color(later_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
            lv_obj_center(later_lbl);
            lv_obj_add_event_cb(later_btn, update_close_cb, LV_EVENT_CLICKED, NULL);
        } else {
            // No OTA URL — show web flasher instructions
            lv_obj_t *instr = lv_label_create(card);
            lv_label_set_text(instr, "Visit CYGM.me on a\ncomputer to update via\nthe web flasher.");
            lv_obj_set_style_text_color(instr, lv_color_hex(COLOR_TEXT_GRAY), 0);
            lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 92);

            lv_obj_t *btn = lv_btn_create(card);
            lv_obj_set_size(btn, 100, 28);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e2d3d), 0);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_t *btn_lbl = lv_label_create(btn);
            lv_label_set_text(btn_lbl, "Dismiss");
            lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COLOR_TEXT_GRAY), 0);
            lv_obj_center(btn_lbl);
            lv_obj_add_event_cb(btn, update_close_cb, LV_EVENT_CLICKED, NULL);
        }

    } else {
        // --- Up to Date ---
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, LV_SYMBOL_OK " Up to Date");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

        char ver_buf[48];
        snprintf(ver_buf, sizeof(ver_buf), "v%d.%d.%d %s",
                 CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH,
                 CYGM_VERSION_STAGE);
        lv_obj_t *ver_lbl = lv_label_create(card);
        lv_label_set_text(ver_lbl, ver_buf);
        lv_obj_set_style_text_color(ver_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(ver_lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(ver_lbl, LV_ALIGN_CENTER, 0, -6);

        lv_obj_t *msg = lv_label_create(card);
        lv_label_set_text(msg, "Your firmware is current.");
        lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 18);
    }
}
