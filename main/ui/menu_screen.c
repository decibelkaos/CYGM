/*
 * menu_screen.c
 *
 * Main settings menu screen with power-off overlay
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
    lv_obj_t *card = lv_obj_create(about_overlay);
    lv_obj_set_size(card, 275, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Close button (×) — top-right
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
    lv_obj_add_event_cb(close_btn, about_close_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Top section: Title + Version ---
    lv_obj_t *app_name = lv_label_create(card);
    lv_label_set_text(app_name, "CYGM");
    lv_obj_set_style_text_font(app_name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(app_name, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(app_name, LV_ALIGN_TOP_MID, 0, 10);

    // Version + stage
    lv_obj_t *ver = lv_label_create(card);
    char ver_buf[48];
    snprintf(ver_buf, sizeof(ver_buf), "v%d.%d.%d %s",
             CYGM_VERSION_MAJOR, CYGM_VERSION_MINOR, CYGM_VERSION_PATCH,
             CYGM_VERSION_STAGE);
    lv_label_set_text(ver, ver_buf);
    lv_obj_set_style_text_color(ver, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 42);

    // Build date
    lv_obj_t *date_lbl = lv_label_create(card);
    lv_label_set_text(date_lbl, "Built " CYGM_VERSION_DATE);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 58);

    // Divider
    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 235, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 76);

    // --- Bottom section: Developer info (left) + QR placeholder (right) ---

    // Developer info — left-aligned
    lv_obj_t *dev_by = lv_label_create(card);
    lv_label_set_text(dev_by, "Developed by");
    lv_obj_set_style_text_color(dev_by, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(dev_by, LV_ALIGN_TOP_LEFT, 20, 84);

    lv_obj_t *dev_name = lv_label_create(card);
    lv_label_set_text(dev_name, "Carl Brothers");
    lv_obj_set_style_text_color(dev_name, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(dev_name, &lv_font_montserrat_14, 0);
    lv_obj_align(dev_name, LV_ALIGN_TOP_LEFT, 20, 100);

    lv_obj_t *dev_url = lv_label_create(card);
    lv_label_set_text(dev_url, "DigitalKaos.tech");
    lv_obj_set_style_text_color(dev_url, lv_color_hex(COLOR_ACCENT_LIGHT), 0);
    lv_obj_align(dev_url, LV_ALIGN_TOP_LEFT, 20, 118);

    // Check for Updates button — ghost outline style
    lv_obj_t *upd_btn = lv_btn_create(card);
    lv_obj_set_size(upd_btn, 148, 24);
    lv_obj_align(upd_btn, LV_ALIGN_TOP_LEFT, 16, 138);
    lv_obj_set_style_bg_opa(upd_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(upd_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(upd_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(upd_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(upd_btn, 1, 0);
    lv_obj_set_style_border_opa(upd_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(upd_btn, 6, 0);
    lv_obj_set_style_shadow_width(upd_btn, 0, 0);
    lv_obj_t *upd_lbl = lv_label_create(upd_btn);
    lv_label_set_text(upd_lbl, LV_SYMBOL_REFRESH " Check for Updates");
    lv_obj_set_style_text_color(upd_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(upd_lbl);
    lv_obj_add_event_cb(upd_btn, check_update_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dev_disc = lv_label_create(card);
    lv_label_set_text(dev_disc, "Not a medical device");
    lv_obj_set_style_text_color(dev_disc, lv_color_hex(0x556677), 0);
    lv_obj_align(dev_disc, LV_ALIGN_TOP_LEFT, 20, 174);

    // QR code — links to DigitalKaos.tech
    static const char *qr_url = "http://DigitalKaos.tech";
    lv_obj_t *qr = lv_qrcode_create(card, 75,
                                     lv_color_hex(COLOR_TEXT_WHITE),
                                     lv_color_hex(0x0d1520));
    lv_qrcode_update(qr, qr_url, strlen(qr_url));
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -16, 84);
    lv_obj_set_style_border_color(qr, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(qr, 2, 0);
}

// Background task: check for updates then show result overlay
static void check_update_task_fn(void *param) {
    if (xSemaphoreTake(network_mutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
        update_check_now();
        xSemaphoreGive(network_mutex);
    }
    if (lvgl_port_lock(200)) {
        show_update_overlay();
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void check_update_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dismiss_about_overlay();
    xTaskCreatePinnedToCore(check_update_task_fn, "upd_chk", 8192, NULL, 5, NULL, 0);
}

static void about_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_about_overlay();
    }
}

// ==================== Power Off Overlay ====================

static lv_obj_t *power_overlay = NULL;
static lv_obj_t *power_slider = NULL;
static lv_obj_t *power_slide_label = NULL;

static bool power_off_initiated = false;

static void power_btn_event_cb(lv_event_t *e);
static void power_close_event_cb(lv_event_t *e);
static void power_slider_event_cb(lv_event_t *e);
static void menu_unload_event_cb(lv_event_t *e);

// Shutdown task — runs on Core 0 to avoid deadlocking the LVGL task on Core 1
static void power_off_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(100));  // Let LVGL event settle

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
        power_slider = NULL;
        power_slide_label = NULL;
        power_off_initiated = false;
        lv_obj_del_async(ov);
    }
}

static void power_close_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        dismiss_power_overlay();
    }
}

static void power_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (power_slider == NULL) return;

    if (code == LV_EVENT_VALUE_CHANGED) {
        int val = lv_slider_get_value(power_slider);

        // Fade out the hint text as knob slides right
        if (power_slide_label != NULL) {
            lv_opa_t opa = (lv_opa_t)(255 - (val * 255 / 100));
            lv_obj_set_style_text_opa(power_slide_label, opa, 0);
        }

        // Trigger at 97% — spawn shutdown on Core 0 (LVGL runs on Core 1)
        if (val >= 97 && !power_off_initiated) {
            power_off_initiated = true;
            ESP_LOGI(TAG, "Power off confirmed via slider");
            xTaskCreatePinnedToCore(power_off_task, "power_off", 4096, NULL, 10, NULL, 0);
        }
    } else if (code == LV_EVENT_RELEASED) {
        // Spring back if not slid far enough
        if (lv_slider_get_value(power_slider) < 97) {
            lv_slider_set_value(power_slider, 0, LV_ANIM_ON);
            if (power_slide_label != NULL) {
                lv_obj_set_style_text_opa(power_slide_label, LV_OPA_COVER, 0);
            }
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
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141c2b), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263040), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Close button (X) — top-right corner
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
    lv_obj_set_style_text_color(note, lv_color_hex(0x556677), 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 70);

    // --- Slide-to-confirm slider ---
    power_slider = lv_slider_create(card);
    lv_obj_set_size(power_slider, 220, 30);
    lv_obj_align(power_slider, LV_ALIGN_TOP_MID, 0, 94);
    lv_slider_set_range(power_slider, 0, 100);
    lv_slider_set_value(power_slider, 0, LV_ANIM_OFF);
    lv_obj_add_flag(power_slider, LV_OBJ_FLAG_ADV_HITTEST);  // Must grab knob
    lv_obj_set_style_anim_time(power_slider, 300, LV_PART_MAIN);  // Snap-back speed

    // Track — dark pill
    lv_obj_set_style_bg_color(power_slider, lv_color_hex(0x0d1520), LV_PART_MAIN);
    lv_obj_set_style_radius(power_slider, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(power_slider, lv_color_hex(0x1e2d3d), LV_PART_MAIN);
    lv_obj_set_style_border_width(power_slider, 1, LV_PART_MAIN);

    // Indicator — red fill as you slide
    lv_obj_set_style_bg_color(power_slider, lv_color_hex(COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(power_slider, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(power_slider, 16, LV_PART_INDICATOR);

    // Knob — white circle, slightly larger than track
    lv_obj_set_style_bg_color(power_slider, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
    lv_obj_set_style_radius(power_slider, 16, LV_PART_KNOB);
    lv_obj_set_style_pad_all(power_slider, 1, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(power_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(power_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(power_slider, LV_OPA_40, LV_PART_KNOB);

    lv_obj_add_event_cb(power_slider, power_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(power_slider, power_slider_event_cb, LV_EVENT_RELEASED, NULL);

    // "slide to power off" hint text overlaid on track
    power_slide_label = lv_label_create(card);
    lv_label_set_text(power_slide_label, "slide to power off  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(power_slide_label, lv_color_hex(0x44556a), 0);
    lv_obj_align_to(power_slide_label, power_slider, LV_ALIGN_CENTER, 14, 0);
    lv_obj_clear_flag(power_slide_label, LV_OBJ_FLAG_CLICKABLE);
}

static void power_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_power_overlay();
    }
}

static void menu_unload_event_cb(lv_event_t *e) {
    dismiss_power_overlay();
    dismiss_about_overlay();
}

// ==================== Menu Screen ====================

void create_menu_screen(void) {
    screen_menu = lv_obj_create(NULL);
    lv_obj_add_style(screen_menu, &style_bg, 0);
    lv_obj_add_event_cb(screen_menu, menu_unload_event_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    // --- Header ---
    lv_obj_t *header = lv_label_create(screen_menu);
    lv_label_set_text(header, "Settings");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button (top left)
    lv_obj_t *back_btn = lv_btn_create(screen_menu);
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
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Ghost about button (top right, left of power)
    lv_obj_t *about_btn = lv_btn_create(screen_menu);
    lv_obj_set_size(about_btn, 36, 36);
    lv_obj_align(about_btn, LV_ALIGN_TOP_RIGHT, -44, 5);
    lv_obj_set_style_bg_opa(about_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(about_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(about_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(about_btn, 0, 0);
    lv_obj_set_style_shadow_width(about_btn, 0, 0);
    lv_obj_set_style_radius(about_btn, 8, 0);
    lv_obj_t *about_icon = lv_label_create(about_btn);
    lv_label_set_text(about_icon, "?");
    lv_obj_set_style_text_font(about_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(about_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(about_icon);
    lv_obj_add_event_cb(about_btn, about_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Ghost power button (top right)
    lv_obj_t *power_btn = lv_btn_create(screen_menu);
    lv_obj_set_size(power_btn, 36, 36);
    lv_obj_align(power_btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_opa(power_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(power_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(power_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(power_btn, 0, 0);
    lv_obj_set_style_shadow_width(power_btn, 0, 0);
    lv_obj_set_style_radius(power_btn, 8, 0);
    lv_obj_t *pwr_icon = lv_label_create(power_btn);
    lv_label_set_text(pwr_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(pwr_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pwr_icon, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(pwr_icon);
    lv_obj_add_event_cb(power_btn, power_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Menu items as individual styled cards ---
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

        // Card styling
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        // Press highlight
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

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
