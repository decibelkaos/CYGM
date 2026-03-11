/*
 * update_checker.c
 *
 * Checks website/version.json for newer firmware.
 * Lightweight: 256-byte static buffer, no dynamic allocation.
 * HTTPS without cert bundle (server CA not in ESP-IDF bundle).
 */

#include "update_checker.h"
#include "shared_state.h"
#include "sd_logger.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "UPDATE";

#define UPDATE_CHECK_URL "https://digitalkaos.tech/version.json"
#define UPDATE_INTERVAL_MS (24LL * 60 * 60 * 1000)  // 24 hours
#define HTTP_BUF_SIZE 256

static update_info_t update_info = {0};
static int64_t last_check_ms = 0;

// HTTP response buffer (version.json is <100 bytes)
static char http_buf[HTTP_BUF_SIZE];
static int http_buf_len = 0;

// Overlay
static lv_obj_t *update_overlay = NULL;

// ==================== HTTP ====================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (http_buf_len + copy >= HTTP_BUF_SIZE - 1)
            copy = HTTP_BUF_SIZE - 1 - http_buf_len;
        if (copy > 0) {
            memcpy(http_buf + http_buf_len, evt->data, copy);
            http_buf_len += copy;
            http_buf[http_buf_len] = '\0';
        }
    }
    return ESP_OK;
}

// ==================== Check Logic ====================

bool update_should_check(void) {
    if (last_check_ms == 0) return true;
    int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms - last_check_ms) >= UPDATE_INTERVAL_MS;
}

esp_err_t update_check_now(void) {
    http_buf_len = 0;
    http_buf[0] = '\0';

    ESP_LOGI(TAG, "Checking %s ...", UPDATE_CHECK_URL);

    esp_http_client_config_t config = {
        .url = UPDATE_CHECK_URL,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 512,
        // Skip cert bundle — digitalkaos.tech uses a CA not in ESP-IDF's bundle.
        // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y handles TLS without verification.
        // This is fine for a non-sensitive version check.
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
    esp_http_client_cleanup(client);

    last_check_ms = esp_timer_get_time() / 1000;

    if (err != ESP_OK || status != 200) {
        sd_log(TAG, "Check failed: %s (HTTP %d)", esp_err_to_name(err), status);
        ESP_LOGW(TAG, "Check failed: %s (HTTP %d)", esp_err_to_name(err), status);
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    // Parse JSON
    cJSON *json = cJSON_Parse(http_buf);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed");
        update_info.check_failed = true;
        update_info.checked = true;
        return ESP_FAIL;
    }

    cJSON *ver   = cJSON_GetObjectItem(json, "version");
    cJSON *stage = cJSON_GetObjectItem(json, "stage");
    cJSON *date  = cJSON_GetObjectItem(json, "date");

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

    // Compare versions
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

    sd_log(TAG, "%s (remote v%s, local v%d.%d.%d)",
           update_info.available ? "UPDATE AVAILABLE" : "Up to date",
           update_info.version,
           CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH);

    return ESP_OK;
}

const update_info_t *update_get_info(void) {
    return &update_info;
}

// ==================== Update Overlay UI ====================

static void update_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        dismiss_update_overlay();
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

    // --- Dimmed backdrop ---
    update_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(update_overlay);
    lv_obj_set_size(update_overlay, 320, 240);
    lv_obj_set_style_bg_color(update_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(update_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(update_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_overlay, update_close_cb, LV_EVENT_CLICKED, NULL);

    // --- Card (sized to content) ---
    int card_h = info->available ? 195 : 125;
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

    // Close button
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

        // Current version
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
        snprintf(avail_buf, sizeof(avail_buf), "Available: v%s %s",
                 info->version, info->stage);
        lv_obj_t *avail_lbl = lv_label_create(card);
        lv_label_set_text(avail_lbl, avail_buf);
        lv_obj_set_style_text_color(avail_lbl, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(avail_lbl, LV_ALIGN_TOP_LEFT, 20, 60);

        // Divider
        lv_obj_t *div = lv_obj_create(card);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, 230, 1);
        lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 82);

        // Instructions
        lv_obj_t *instr = lv_label_create(card);
        lv_label_set_text(instr, "Visit DigitalKaos.tech on a\ncomputer to update via\nthe web flasher.");
        lv_obj_set_style_text_color(instr, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 92);

        // Dismiss button
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
