/*
 * menu_screen.c - settings menu, About/Erase overlays, OTA check, power-off.
 */

#include "menu_screen.h"
#include "shared_state.h"
#include "main.h"
#include "features/update_checker.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include "sd_logger.h"
#include "nvs_config.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "ui/home_screen.h"

static const char *TAG = "MENU_SCREEN";

// ==================== Hardware Shutdown ====================

static void prepare_for_sleep(void) {
    // Turn off LCD display output (controller stops driving the panel = black screen)
    if (lcd_panel != NULL) {
        esp_lcd_panel_disp_on_off(lcd_panel, false);
    }

    // Stop LEDC on LED channels (idle_level=1 for active-low LEDs = LED off)
    ledc_stop(LEDC_LOW_SPEED_MODE, LED_RED_CHANNEL, 1);
    ledc_stop(LEDC_LOW_SPEED_MODE, LED_GREEN_CHANNEL, 1);
    ledc_stop(LEDC_LOW_SPEED_MODE, LED_BLUE_CHANNEL, 1);

    // Stop buzzer
    ledc_stop(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);

    // Reset LED GPIOs to plain output and drive high (active-low = off)
    gpio_reset_pin(LED_RED);
    gpio_reset_pin(LED_GREEN);
    gpio_reset_pin(LED_BLUE);
    gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_RED, 1);
    gpio_set_level(LED_GREEN, 1);
    gpio_set_level(LED_BLUE, 1);

    // Hold LED_RED (GPIO 4) state during deep sleep (it's an RTC GPIO)
    gpio_hold_en(LED_RED);

    // Turn off LCD backlight and hold low during deep sleep (GPIO 27 = RTC GPIO)
    gpio_reset_pin(LCD_BL);
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 0);
    gpio_hold_en(LCD_BL);
}

// ==================== About Overlay ====================

static lv_obj_t *about_overlay = NULL;
static lv_obj_t *erase_overlay = NULL;
static lv_obj_t *update_check_overlay = NULL;
static volatile bool update_check_in_progress = false;

static void about_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (about_overlay != NULL) {
            lv_obj_t *ov = about_overlay;
            about_overlay = NULL;
            lv_obj_del_async(ov);
        }
    }
}

static void dismiss_about_overlay(void) {
    if (about_overlay != NULL) {
        lv_obj_t *ov = about_overlay;
        about_overlay = NULL;
        lv_obj_del_async(ov);
    }
}

static void check_update_btn_cb(lv_event_t *e);  // forward decl

static void device_guide_btn_cb(lv_event_t *e) {
    (void)e;
    dismiss_about_overlay();
    show_welcome_overlay();
}

static void sd_log_checkbox_cb(lv_event_t *e) {
    lv_obj_t *cb = lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
    sd_glucose_logging_enabled = checked;
    nvs_save_sd_glucose_logging(checked);
    ESP_LOGI(TAG, "SD glucose logging: %s", checked ? "ON" : "OFF");
}

// ==================== Erase Device Confirmation Overlay ====================

static void dismiss_erase_overlay(void) {
    if (erase_overlay != NULL) {
        lv_obj_t *ov = erase_overlay;
        erase_overlay = NULL;
        lv_obj_del_async(ov);
    }
}

static void erase_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        dismiss_erase_overlay();
    }
}

static void erase_confirm_cb(lv_event_t *e) {
    (void)e;
    dismiss_erase_overlay();
    nvs_factory_reset();
    esp_restart();
}

static void show_erase_overlay(void);  // forward decl

static void erase_device_btn_cb(lv_event_t *e) {
    (void)e;
    dismiss_about_overlay();
    show_erase_overlay();
}

static void show_erase_overlay(void) {
    if (erase_overlay != NULL) return;

    // Full-screen dimmed backdrop
    erase_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(erase_overlay);
    lv_obj_set_size(erase_overlay, 320, 240);
    lv_obj_set_style_bg_color(erase_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(erase_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(erase_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(erase_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(erase_overlay, erase_close_event_cb, LV_EVENT_CLICKED, NULL);

    // Red-accented card
    lv_obj_t *card = lv_obj_create(erase_overlay);
    lv_obj_set_size(card, 280, 185);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_RED), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Erase Device?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_RED), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    // Warning message
    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "All settings, WiFi networks,\n"
        "Dexcom credentials, and alarms\n"
        "will be permanently deleted.\n\n"
        "Device will restart.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 260);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 56);

    // Cancel button — ghost outline, blue text, left side
    lv_obj_t *cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, CYGM_BTN_W_TEXT, CYGM_BTN_H);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    cygm_apply_ghost_btn(cancel_btn);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, erase_close_event_cb, LV_EVENT_CLICKED, NULL);

    // Erase & Reset button — solid red, white text, right side
    lv_obj_t *confirm_btn = lv_btn_create(card);
    lv_obj_set_size(confirm_btn, CYGM_BTN_W_TEXT, CYGM_BTN_H);
    lv_obj_align(confirm_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
    cygm_apply_ghost_btn(confirm_btn);
    // Solid destructive fill overrides the ghost base at both states
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(COLOR_PRESSED_RED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(confirm_btn, 0, 0);
    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, "Erase & Reset");
    lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(confirm_lbl);
    lv_obj_add_event_cb(confirm_btn, erase_confirm_cb, LV_EVENT_CLICKED, NULL);
}

static void show_about_overlay(void) {
    if (about_overlay != NULL) return;

    // --- Full-screen dimmed backdrop ---
    about_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(about_overlay);
    lv_obj_set_size(about_overlay, 320, 240);
    lv_obj_set_style_bg_color(about_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(about_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(about_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(about_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(about_overlay, about_close_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Centered card ---
    bool sd_available = sd_logger_available();
    int card_h = sd_available ? 212 : 190;
    lv_obj_t *card = lv_obj_create(about_overlay);
    lv_obj_set_size(card, 290, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Close button (×) — top-right
    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_update_layout(close_btn);  // under 34px — helper needs the resolved size
    cygm_apply_ghost_btn(close_btn);
    lv_obj_set_style_border_width(close_btn, 0, 0);  // borderless round glyph button
    lv_obj_set_style_radius(close_btn, 16, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, about_close_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Top section: Title + Version + Build (compact) ---
    lv_obj_t *app_name = lv_label_create(card);
    lv_label_set_text(app_name, "CYGM");
    lv_obj_set_style_text_font(app_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(app_name, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(app_name, LV_ALIGN_TOP_MID, 0, 8);

    // Version + stage + build date on one line
    lv_obj_t *ver = lv_label_create(card);
    char ver_buf[64];
    snprintf(ver_buf, sizeof(ver_buf), "v%d.%d.%d %s | %s",
             CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH,
             CYGM_VERSION_STAGE, CYGM_VERSION_DATE);
    lv_label_set_text(ver, ver_buf);
    lv_obj_set_style_text_color(ver, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 34);

    // Divider
    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 255, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 50);

    // --- Middle section: Developer info (left) + QR (right) ---
    lv_obj_t *dev_by = lv_label_create(card);
    lv_label_set_text(dev_by, "Developed by");
    lv_obj_set_style_text_color(dev_by, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(dev_by, LV_ALIGN_TOP_LEFT, 16, 56);

    lv_obj_t *dev_name = lv_label_create(card);
    lv_label_set_text(dev_name, "Carl Brothers");
    lv_obj_set_style_text_color(dev_name, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(dev_name, &lv_font_montserrat_14, 0);
    lv_obj_align(dev_name, LV_ALIGN_TOP_LEFT, 16, 70);

    lv_obj_t *dev_url = lv_label_create(card);
    lv_label_set_text(dev_url, "CYGM.me");
    lv_obj_set_style_text_color(dev_url, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(dev_url, LV_ALIGN_TOP_LEFT, 16, 88);

    lv_obj_t *dev_disc = lv_label_create(card);
    lv_label_set_text(dev_disc, "Not a medical device");
    lv_obj_set_style_text_color(dev_disc, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(dev_disc, LV_ALIGN_TOP_LEFT, 16, 104);

    // Device Guide button
    lv_obj_t *guide_btn = lv_btn_create(card);
    lv_obj_set_size(guide_btn, 160, 28);
    lv_obj_align(guide_btn, LV_ALIGN_TOP_LEFT, 14, 122);
    lv_obj_update_layout(guide_btn);  // 28px tall — helper needs the resolved size
    cygm_apply_ghost_btn(guide_btn);
    lv_obj_t *guide_lbl = lv_label_create(guide_btn);
    lv_label_set_text(guide_lbl, LV_SYMBOL_LIST " Device Guide");
    lv_obj_set_style_text_color(guide_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(guide_lbl);
    lv_obj_add_event_cb(guide_btn, device_guide_btn_cb, LV_EVENT_CLICKED, NULL);

    // SD glucose logging checkbox (only when SD card is present)
    int bottom_section_y = 154;  // Default y for bottom row when no SD checkbox
    if (sd_available) {
        lv_obj_t *sd_cb = lv_checkbox_create(card);
        lv_checkbox_set_text(sd_cb, "Save CGM data to SD");
        lv_obj_set_style_text_color(sd_cb, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_bg_color(sd_cb, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(sd_cb, lv_color_hex(COLOR_TEXT_DIM), LV_PART_INDICATOR);
        lv_obj_align(sd_cb, LV_ALIGN_TOP_LEFT, 14, 154);
        // A checkbox gets no ghost frame, but its touch box still grows
        lv_obj_set_ext_click_area(sd_cb, CYGM_BTN_EXT_CLICK);
        if (sd_glucose_logging_enabled) {
            lv_obj_add_state(sd_cb, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sd_cb, sd_log_checkbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
        bottom_section_y = 178;  // Push bottom row down when SD checkbox present
    }

    // Divider before bottom buttons
    lv_obj_t *divider2 = lv_obj_create(card);
    lv_obj_remove_style_all(divider2);
    lv_obj_set_size(divider2, 255, 1);
    lv_obj_set_style_bg_color(divider2, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(divider2, LV_OPA_COVER, 0);
    lv_obj_align(divider2, LV_ALIGN_TOP_MID, 0, bottom_section_y);

    // Bottom row: Check for Updates (left) + Erase Device (right)
    lv_obj_t *upd_btn = lv_btn_create(card);
    lv_obj_set_size(upd_btn, 128, 28);
    lv_obj_align(upd_btn, LV_ALIGN_TOP_LEFT, 8, bottom_section_y + 5);
    lv_obj_update_layout(upd_btn);  // 28px tall — helper needs the resolved size
    cygm_apply_ghost_btn(upd_btn);
    lv_obj_t *upd_lbl = lv_label_create(upd_btn);
    lv_label_set_text(upd_lbl, LV_SYMBOL_REFRESH " Updates");
    lv_obj_set_style_text_color(upd_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(upd_lbl);
    lv_obj_add_event_cb(upd_btn, check_update_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *erase_btn = lv_btn_create(card);
    lv_obj_set_size(erase_btn, 128, 28);
    lv_obj_align(erase_btn, LV_ALIGN_TOP_RIGHT, -8, bottom_section_y + 5);
    lv_obj_update_layout(erase_btn);  // 28px tall — helper needs the resolved size
    cygm_apply_ghost_btn(erase_btn);
    // Destructive accent: red outline, red press fill
    lv_obj_set_style_border_color(erase_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_color(erase_btn, lv_color_hex(COLOR_PRESSED_RED), LV_STATE_PRESSED);
    lv_obj_t *erase_lbl = lv_label_create(erase_btn);
    lv_label_set_text(erase_lbl, LV_SYMBOL_TRASH " Erase Device");
    lv_obj_set_style_text_color(erase_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(erase_lbl);
    lv_obj_add_event_cb(erase_btn, erase_device_btn_cb, LV_EVENT_CLICKED, NULL);

    // QR code — links to CYGM.me
    static const char *qr_url = "http://cygm.me";
    lv_obj_t *qr = lv_qrcode_create(card, 75,
                                     lv_color_hex(COLOR_TEXT_WHITE),
                                     lv_color_hex(COLOR_MODAL_BG));
    lv_qrcode_update(qr, qr_url, strlen(qr_url));
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -16, 56);
    lv_obj_set_style_border_color(qr, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(qr, 2, 0);
}

// 8KB stack for HTTPS/TLS. Only created after update_stop_all_tasks() frees ~35KB.
static void ota_download_task_fn(void *param) {
    update_do_ota_download();
    // On success: reboots inside. On failure: also reboots (tasks were deleted).
    vTaskDelete(NULL);
}

// Waits for the user to tap Install or Dismiss. No TLS here, so 3KB is enough.
static void update_ota_wait_task_fn(void *param) {
    update_set_manual_polling(true);
    update_clear_ota_request();

    const update_info_t *info = update_get_info();
    for (int i = 0; i < 1200; i++) {  // 120s timeout
        if (update_ota_install_requested()) break;
        if (!info->available) break;  // Overlay was dismissed
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    update_set_manual_polling(false);

    if (update_ota_install_requested()) {
        update_clear_ota_request();

        for (int retry = 0; retry < 50; retry++) {
            if (lvgl_port_lock(1)) {
                dismiss_update_overlay();
                show_ota_overlay("Stopping tasks...");
                lvgl_port_unlock();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Delete ALL tasks to maximize heap for OTA TLS (~35KB freed)
        update_stop_all_tasks();

        // Create OTA download task with proper 8KB stack (heap now has ~40KB+ free)
        BaseType_t ret = xTaskCreatePinnedToCore(
            ota_download_task_fn, "ota_dl", 8192, NULL, 5, NULL, 0);
        if (ret != pdPASS) {
            ESP_LOGE("MENU", "Failed to create OTA task — rebooting");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        vTaskDelete(NULL);  // OTA task handles reboot
    }

    // User dismissed or timeout — restore background tasks
    update_restore_tasks();
    vTaskDelete(NULL);
}

// Progress overlay shown while the check worker runs. LVGL context only.
static void show_update_check_overlay(void) {
    if (update_check_overlay != NULL) return;

    update_check_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(update_check_overlay);
    lv_obj_set_size(update_check_overlay, 320, 240);
    lv_obj_set_style_bg_color(update_check_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(update_check_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(update_check_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_check_overlay, LV_OBJ_FLAG_CLICKABLE);  // swallow taps while busy

    lv_obj_t *card = lv_obj_create(update_check_overlay);
    lv_obj_set_size(card, 220, 118);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(card, 1000, 60);
    lv_obj_set_size(spinner, 36, 36);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_arc_width(spinner, 4, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Checking for updates");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "Contacting update server...");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 82);
}

static void hide_update_check_overlay(void) {
    if (update_check_overlay != NULL) {
        lv_obj_t *ov = update_check_overlay;
        update_check_overlay = NULL;
        lv_obj_del(ov);
    }
}

// The version check is a blocking HTTPS round-trip and must not run inline in
// the button callback, which would freeze LVGL for its duration. Sized to reuse
// the blocks freed by the caller so the big contiguous block stays free for TLS.
static void update_check_task_fn(void *param) {
    (void)param;

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "Update check: largest_block=%lu (worker task)", (unsigned long)largest);

    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
        update_check_now();
        xSemaphoreGive(network_mutex);
    } else {
        ESP_LOGW(TAG, "Update check: network_mutex busy, check skipped");
    }

    // Marshal the result back onto the UI. lvgl_port_lock(1) is a try-lock, so
    // keep retrying — giving up early would strand the progress overlay.
    bool ui_updated = false;
    for (int retry = 0; retry < 300; retry++) {
        if (lvgl_port_lock(1)) {
            hide_update_check_overlay();
            show_update_overlay();
            lvgl_port_unlock();
            ui_updated = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ui_updated) {
        ESP_LOGE(TAG, "Update check: LVGL lock never acquired, result not shown");
    }

    const update_info_t *info = update_get_info();
    if (info->available && info->firmware_url[0] != '\0') {
        // Hand off to the small Install/Dismiss poller so this larger stack is
        // freed before the user's 120s decision window.
        BaseType_t ret = xTaskCreatePinnedToCore(
            update_ota_wait_task_fn, "upd_ota", 3072, NULL, 5, NULL, 0);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA wait task");
            update_restore_tasks();
        }
    } else {
        // No OTA available — restore background tasks immediately
        update_restore_tasks();
    }

    update_check_in_progress = false;
    vTaskDelete(NULL);
}

static void check_update_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    // The About overlay is torn down asynchronously, so the button survives
    // long enough to be tapped twice.
    if (update_check_in_progress) return;
    update_check_in_progress = true;

    lv_obj_t *btn = lv_event_get_target(e);
    if (btn != NULL) lv_obj_add_state(btn, LV_STATE_DISABLED);

    // Navigate to home screen — update overlay shows on lv_layer_top()
    dismiss_about_overlay();
    lv_scr_load(screen_home);
    home_screen_active = true;
    pause_background_tasks = true;

    // Delete menu screen — reclaims LVGL memory
    if (screen_menu != NULL) {
        lv_obj_del(screen_menu);
        screen_menu = NULL;
    }

    show_update_check_overlay();

    // Free CGM SSL clients (~15-20KB freed)
    dexcom_close_persistent_client();
    libre_close_persistent_client();

    // Delete background tasks to coalesce heap (~11KB stacks freed). This must
    // stay ahead of the worker: with the SSL clients alive largest_block is
    // ~3.5KB and the task creation would fail outright.
    delete_background_tasks_for_ssl("update-check");

    // 6144 covers the TLS check, the SD-mount-on-write path in sd_log, and the
    // overlay build — same budget as the provider login tasks.
    if (xTaskCreatePinnedToCore(update_check_task_fn, "upd_chk", 6144, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create update check task (largest=%lu)",
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        hide_update_check_overlay();
        update_restore_tasks();
        update_check_in_progress = false;
    }
}

static void about_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_about_overlay();
    }
}

// ==================== Power Off Overlay ====================

static lv_obj_t *power_overlay = NULL;
static lv_obj_t *power_hold_bar = NULL;
static uint32_t power_hold_start_ms = 0;

static bool power_off_initiated = false;

static void power_btn_event_cb(lv_event_t *e);
static void power_close_event_cb(lv_event_t *e);
static void power_hold_event_cb(lv_event_t *e);
static void menu_unload_event_cb(lv_event_t *e);

// Shutdown task — runs on Core 0 to avoid deadlocking the LVGL task on Core 1
static void power_off_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(100));  // Let LVGL event settle

    sd_log(TAG, "POWER OFF: user-initiated shutdown");
    sd_logger_flush();  // Flush before shutdown — last chance to write

    ESP_LOGI(TAG, "Stopping WiFi...");
    esp_wifi_stop();

    ESP_LOGI(TAG, "Shutting down hardware...");
    prepare_for_sleep();

    ESP_LOGI(TAG, "Entering deep sleep (wake on boot button)...");
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();

    vTaskDelete(NULL);  // Never reached
}

static void dismiss_power_overlay(void) {
    if (power_overlay != NULL) {
        lv_obj_t *ov = power_overlay;
        power_overlay = NULL;
        power_hold_bar = NULL;
        power_off_initiated = false;
        lv_obj_del_async(ov);
    }
}

static void power_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        dismiss_power_overlay();
    }
}

#define POWER_HOLD_DURATION_MS 2000

static void power_hold_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (power_hold_bar == NULL) return;

    if (code == LV_EVENT_PRESSED) {
        power_hold_start_ms = lv_tick_get();
    } else if (code == LV_EVENT_PRESSING) {
        uint32_t elapsed = lv_tick_elaps(power_hold_start_ms);
        int progress = (elapsed * 100) / POWER_HOLD_DURATION_MS;
        if (progress > 100) progress = 100;
        lv_bar_set_value(power_hold_bar, progress, LV_ANIM_OFF);

        if (progress >= 100 && !power_off_initiated) {
            power_off_initiated = true;
            ESP_LOGI(TAG, "Power off confirmed via hold button");

            // Free SSL + background tasks to reclaim contiguous heap.
            // Post-reconnect fragmentation leaves largest_block ~2.9KB,
            // not enough for the 3KB power_off task stack.
            dexcom_close_persistent_client();
            libre_close_persistent_client();
            delete_background_tasks_for_ssl("power-off");

            BaseType_t rc = xTaskCreatePinnedToCore(
                power_off_task, "power_off", 3072, NULL, 10, NULL, 0);
            if (rc != pdPASS) {
                ESP_LOGE(TAG, "Failed to create power_off task — forcing shutdown");
                // Last resort: shut down directly from LVGL context
                sd_logger_flush();
                esp_wifi_stop();
                prepare_for_sleep();
                esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
                esp_deep_sleep_start();
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!power_off_initiated) {
            lv_bar_set_value(power_hold_bar, 0, LV_ANIM_OFF);  // LV_ANIM_ON triggers lv_anim_start (freeze risk)
        }
    }
}

static void show_power_overlay(void) {
    if (power_overlay != NULL) return;

    // --- Full-screen dimmed backdrop ---
    power_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(power_overlay);
    lv_obj_set_size(power_overlay, 320, 240);
    lv_obj_set_style_bg_color(power_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(power_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(power_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(power_overlay, power_close_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Centered popup card ---
    lv_obj_t *card = lv_obj_create(power_overlay);
    lv_obj_set_size(card, 270, 148);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Close button (X) — top-right corner
    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_update_layout(close_btn);  // under 34px — helper needs the resolved size
    cygm_apply_ghost_btn(close_btn);
    lv_obj_set_style_border_width(close_btn, 0, 0);  // borderless round glyph button
    lv_obj_set_style_radius(close_btn, 16, 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, power_close_event_cb, LV_EVENT_CLICKED, NULL);

    // Power icon — large, red, centered at top
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_RED), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);

    // "Power Off" title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Power Off");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    // Disclaimer
    lv_obj_t *note = lv_label_create(card);
    lv_label_set_text(note, "Battery may slowly drain while off");
    lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 70);

    // --- Hold-to-confirm button ---
    lv_obj_t *hold_container = lv_obj_create(card);
    lv_obj_remove_style_all(hold_container);
    lv_obj_set_size(hold_container, 220, 34);
    lv_obj_align(hold_container, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_color(hold_container, lv_color_hex(COLOR_MODAL_BG), 0);
    lv_obj_set_style_bg_opa(hold_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hold_container, 17, 0);
    lv_obj_set_style_border_color(hold_container, lv_color_hex(COLOR_MODAL_BORDER), 0);
    lv_obj_set_style_border_width(hold_container, 1, 0);
    lv_obj_set_style_border_opa(hold_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hold_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hold_container, LV_OBJ_FLAG_CLICKABLE);
    // Shared pressed feedback + touch box; the pill's own radius/border above win
    cygm_apply_ghost_btn(hold_container);

    // Progress bar (fills red as you hold)
    power_hold_bar = lv_bar_create(hold_container);
    lv_obj_set_size(power_hold_bar, 218, 32);
    lv_obj_center(power_hold_bar);
    lv_bar_set_range(power_hold_bar, 0, 100);
    lv_bar_set_value(power_hold_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(power_hold_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(power_hold_bar, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(power_hold_bar, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(power_hold_bar, 16, LV_PART_MAIN);
    lv_obj_set_style_radius(power_hold_bar, 16, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(power_hold_bar, 300, LV_PART_MAIN);  // Snap-back speed
    lv_obj_clear_flag(power_hold_bar, LV_OBJ_FLAG_CLICKABLE);  // Pass clicks to container

    // Label on top
    lv_obj_t *hold_label = lv_label_create(hold_container);
    lv_label_set_text(hold_label, LV_SYMBOL_POWER "  Hold to Power Off");
    lv_obj_set_style_text_color(hold_label, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(hold_label);
    lv_obj_clear_flag(hold_label, LV_OBJ_FLAG_CLICKABLE);  // Pass clicks to container

    // Events on container
    lv_obj_add_event_cb(hold_container, power_hold_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hold_container, power_hold_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hold_container, power_hold_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(hold_container, power_hold_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void power_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_power_overlay();
    }
}

static void menu_unload_event_cb(lv_event_t *e) {
    dismiss_power_overlay();
    dismiss_about_overlay();
    dismiss_erase_overlay();
}

// ==================== Menu Screen ====================

// Canonical back button: same chevron at the same corner on every screen, so
// "back" never shifts under the thumb. Each UI module keeps its own copy.
static lv_obj_t *menu_std_back_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
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

void create_menu_screen(void) {
    // The inactivity watchdog can reclaim a screen mid-flow, so rebuilding
    // without this guard orphans the previous menu in the LVGL pool. Deleting
    // the ACTIVE screen synchronously would panic, hence the async branch.
    if (screen_menu != NULL) {
        if (screen_menu == lv_scr_act()) {
            lv_obj_del_async(screen_menu);
        } else {
            lv_obj_del(screen_menu);
        }
        screen_menu = NULL;
    }

    screen_menu = lv_obj_create(NULL);
    lv_obj_add_style(screen_menu, &style_bg, 0);
    // Four fixed rows always fit; without this the screen drag-scrolls and the
    // bottom row bounces against the panel edge.
    lv_obj_clear_flag(screen_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_menu, menu_unload_event_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    // --- Header ---
    lv_obj_t *header = lv_label_create(screen_menu);
    lv_label_set_text(header, "Settings");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    menu_std_back_btn(screen_menu, back_btn_event_cb, NULL);

    // Ghost about button (top right, left of power)
    lv_obj_t *about_btn = lv_btn_create(screen_menu);
    lv_obj_set_size(about_btn, CYGM_BTN_H, CYGM_BTN_H);
    lv_obj_align(about_btn, LV_ALIGN_TOP_RIGHT, -44, 4);
    cygm_apply_ghost_btn(about_btn);
    // Only 3px separates this from the power button, so cap the touch-box growth
    // — a wider one would swallow taps meant for its neighbour.
    lv_obj_set_ext_click_area(about_btn, 3);
    lv_obj_set_style_border_width(about_btn, 0, 0);  // borderless header glyph button
    lv_obj_t *about_icon = lv_label_create(about_btn);
    lv_label_set_text(about_icon, "?");
    lv_obj_set_style_text_font(about_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(about_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(about_icon);
    lv_obj_add_event_cb(about_btn, about_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Ghost power button (top right)
    lv_obj_t *power_btn = lv_btn_create(screen_menu);
    lv_obj_set_size(power_btn, CYGM_BTN_H, CYGM_BTN_H);
    lv_obj_align(power_btn, LV_ALIGN_TOP_RIGHT, -5, 4);
    cygm_apply_ghost_btn(power_btn);
    lv_obj_set_ext_click_area(power_btn, 3);  // see the about button above
    lv_obj_set_style_border_width(power_btn, 0, 0);  // borderless header glyph button
    lv_obj_t *pwr_icon = lv_label_create(power_btn);
    lv_label_set_text(pwr_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(pwr_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pwr_icon, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(pwr_icon);
    lv_obj_add_event_cb(power_btn, power_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Menu items as individual styled cards ---
    // A row label has 227px between the icon and the chevron; keep new entries
    // under that at the inherited montserrat_14.
    const char *items[] = {"CGM Settings", "WiFi Setup", "Time / Weather", "Alarms"};
    const char *icons[] = {LV_SYMBOL_EYE_OPEN, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS, LV_SYMBOL_BELL};
    const uint32_t icon_colors[] = {COLOR_ACCENT_BLUE, COLOR_GREEN, COLOR_ORANGE, COLOR_YELLOW};

    #define MENU_CARD_W     290
    #define MENU_CARD_H     40
    #define MENU_CARD_GAP   7
    #define MENU_START_Y    50

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = lv_btn_create(screen_menu);
        lv_obj_set_size(row, MENU_CARD_W, MENU_CARD_H);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, MENU_START_Y + i * (MENU_CARD_H + MENU_CARD_GAP));

        cygm_apply_ghost_btn(row);
        // Rows sit MENU_CARD_GAP apart, so the helper's 8px growth would make
        // adjacent hit boxes overlap and the lower sibling would win the gap.
        lv_obj_set_ext_click_area(row, 3);

        // Card styling — a filled card, so the ghost base is overridden here
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        // Press highlight
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_MODAL_BORDER), LV_STATE_PRESSED);

        // Colored left accent bar
        lv_obj_t *accent = lv_obj_create(row);
        lv_obj_remove_style_all(accent);
        lv_obj_set_size(accent, 3, 26);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, -9, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(icon_colors[i]), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 2, 0);

        // Colored icon
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_color(icon, lv_color_hex(icon_colors[i]), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        // Label
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, items[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 28, 0);

        // Right arrow
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

        // Click handler (indices 0-3 match menu_item_event_cb cases)
        lv_obj_add_event_cb(row, menu_item_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}
