/*
 * cgm_screens.c
 *
 * CGM provider configuration screens implementation
 */

#include "cgm_screens.h"
#include "shared_state.h"
#include "main.h"
#include "dexcom_api.h"
#include "nvs_config.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CGM_SCREENS";
static volatile bool dexcom_login_in_progress = false;

static void dexcom_login_task(void *pvParameters) {
    char username[64] = {0};
    char password[64] = {0};
    strncpy(username, dexcom_username_buf, sizeof(username) - 1);
    strncpy(password, dexcom_password_buf, sizeof(password) - 1);

    ESP_LOGI(TAG, "Dexcom login task started for user: %s", username);

    if (dexcom_authenticate(username, password) == ESP_OK) {
        ESP_LOGI(TAG, "Dexcom login successful");
        nvs_set_dexcom_credentials(username, password);

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after first-time login...");
            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
        }

        ESP_LOGI(TAG, "Triggering immediate glucose fetch after successful login");
        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(100)) {
            lv_scr_load(screen_home);
            home_screen_active = true;
            pause_background_tasks = false;
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "Failed to lock LVGL to return to home screen after login");
        }
    } else {
        ESP_LOGE(TAG, "Dexcom login failed");
    }

    dexcom_login_in_progress = false;
    vTaskDelete(NULL);
}

// Forward declarations for internal event handlers
static void dexcom_back_event_cb(lv_event_t *e);
static void dexcom_logout_btn_event_cb(lv_event_t *e);
static void cgm_back_event_cb(lv_event_t *e);
static void dexcom_btn_event_cb(lv_event_t *e);
static void librelinkup_btn_event_cb(lv_event_t *e);
static void dexcom_view_status_btn_event_cb(lv_event_t *e);
static void dexcom_open_login_event_cb(lv_event_t *e);
static void dexcom_username_tap_event_cb(lv_event_t *e);
static void dexcom_password_tap_event_cb(lv_event_t *e);
static void dexcom_login_btn_event_cb(lv_event_t *e);
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e);
static void dexcom_username_entry_field_event_cb(lv_event_t *e);
static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e);
static void dexcom_password_entry_field_event_cb(lv_event_t *e);

// Dexcom back button event handler
static void dexcom_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_cgm_menu != NULL) {
                lv_scr_load(screen_cgm_menu);
            }
            lvgl_port_unlock();
        }
    }
}

// Dexcom logout button event handler
static void dexcom_logout_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Dexcom logout requested");

        // Clear credentials from NVS
        nvs_clear_dexcom_credentials();

        // Logout from API
        dexcom_logout();

        // Clear local buffers
        memset(dexcom_username_buf, 0, sizeof(dexcom_username_buf));
        memset(dexcom_password_buf, 0, sizeof(dexcom_password_buf));

        // Clear glucose data
        glucose_data_valid = false;
        current_glucose = 0;
        glucose_status = GLUCOSE_STATUS_NOT_AUTHENTICATED;
        first_glucose_received = false;  // Reset so success sound plays on next login
        update_glucose_display();

        ESP_LOGI(TAG, "Dexcom logout complete");

        // Recreate auth screen to show login option
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            lvgl_port_unlock();
        }
    }
}

// CGM back button event handler
static void cgm_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_menu);
            lvgl_port_unlock();
        }
    }
}

// Dexcom button event handler
static void dexcom_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            // Create screen on demand
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            lvgl_port_unlock();
        }
    }
}

// LibreLinkUp button event handler (placeholder)
static void librelinkup_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "LibreLinkUp not yet implemented");
    }
}

// Dexcom view status button event handler
static void dexcom_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            // Create screen on demand
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            create_dexcom_status_screen();
            lv_scr_load(screen_dexcom_status);
            lvgl_port_unlock();
        }
    }
}

// Dexcom open login button event handler
static void dexcom_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            // Create screen on demand
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
            }
            create_dexcom_login_screen();
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    }
}

// Dexcom username tap event handler
static void dexcom_username_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_username_entry != NULL) {
                lv_obj_del(screen_dexcom_username_entry);
                screen_dexcom_username_entry = NULL;
            }
            create_dexcom_username_entry_screen();
            lv_scr_load(screen_dexcom_username_entry);
            lvgl_port_unlock();
        }
    }
}

// Dexcom password tap event handler
static void dexcom_password_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_password_entry != NULL) {
                lv_obj_del(screen_dexcom_password_entry);
                screen_dexcom_password_entry = NULL;
            }
            create_dexcom_password_entry_screen();
            lv_scr_load(screen_dexcom_password_entry);
            lvgl_port_unlock();
        }
    }
}

// Dexcom login button event handler
static void dexcom_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (dexcom_login_in_progress) {
            ESP_LOGW(TAG, "Dexcom login already in progress");
            return;
        }

        dexcom_login_in_progress = true;
        if (xTaskCreate(dexcom_login_task, "dexcom_login", 6144, NULL, 2, NULL) != pdPASS) {
            dexcom_login_in_progress = false;
            ESP_LOGE(TAG, "Failed to create Dexcom login task");
        }
    }
}

// Dexcom username entry keyboard event handler
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Handle button clicks with simulated event codes
    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Username entry cancelled");
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_dexcom_login);
                lvgl_port_unlock();
            }
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Username entry completed: %s", dexcom_username_buf);
            if (dexcom_username_label != NULL) {
                lv_label_set_text(dexcom_username_label, dexcom_username_buf);
                if (strlen(dexcom_username_buf) > 0) {
                    lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                } else {
                    lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                }
            }
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_dexcom_login);
                lvgl_port_unlock();
            }
        }
    }
}

// Dexcom username entry field event handler (updates display label as user types)
static void dexcom_username_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                // Update buffer
                strncpy(dexcom_username_buf, text, sizeof(dexcom_username_buf) - 1);
                dexcom_username_buf[sizeof(dexcom_username_buf) - 1] = '\0';

                // Update display label - placeholder disappears when typing starts
                if (display_label != NULL) {
                    if (strlen(text) > 0) {
                        lv_label_set_text(display_label, text);
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "Username");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

// Dexcom password entry keyboard event handler
static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Handle button clicks with simulated event codes
    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Password entry cancelled");
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_dexcom_login);
                lvgl_port_unlock();
            }
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Password entry completed (masked)");
            if (dexcom_password_label != NULL) {
                int len = strlen(dexcom_password_buf);
                if (len == 0) {
                    lv_label_set_text(dexcom_password_label, "Tap to enter");
                    lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                } else {
                    char display[65];
                    for (int i = 0; i < len; i++) {
                        display[i] = '*';
                    }
                    display[len] = '\0';
                    lv_label_set_text(dexcom_password_label, display);
                    lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                }
            }
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_dexcom_login);
                lvgl_port_unlock();
            }
        }
    }
}

// Dexcom password entry field event handler (shows last character, masks rest)
static void dexcom_password_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);

        if (ta != NULL) {
            const char *text = lv_textarea_get_text(ta);
            if (text != NULL) {
                // Update buffer
                strncpy(dexcom_password_buf, text, sizeof(dexcom_password_buf) - 1);
                dexcom_password_buf[sizeof(dexcom_password_buf) - 1] = '\0';

                // Update display label - show last character, mask the rest
                if (display_label != NULL) {
                    int len = strlen(text);
                    if (len > 0) {
                        char display[65];
                        // Mask all characters except the last one
                        for (int i = 0; i < len - 1; i++) {
                            display[i] = '*';
                        }
                        // Show the last character
                        display[len - 1] = text[len - 1];
                        display[len] = '\0';
                        lv_label_set_text(display_label, display);
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    } else {
                        lv_label_set_text(display_label, "Password");
                        lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
                    }
                }
            }
        }
    }
}

void create_cgm_menu_screen(void) {
    screen_cgm_menu = lv_obj_create(NULL);
    lv_obj_add_style(screen_cgm_menu, &style_bg, 0);

    // Header
    lv_obj_t *title = lv_label_create(screen_cgm_menu);
    lv_label_set_text(title, "CGM Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, cgm_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Dexcom button card
    lv_obj_t *dexcom_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(dexcom_btn, 290, 50);
    lv_obj_align(dexcom_btn, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(dexcom_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dexcom_btn, 10, 0);
    lv_obj_set_style_border_width(dexcom_btn, 0, 0);
    lv_obj_set_style_shadow_width(dexcom_btn, 0, 0);
    lv_obj_set_style_pad_left(dexcom_btn, 14, 0);
    lv_obj_set_style_bg_color(dexcom_btn, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

    lv_obj_t *dexcom_icon = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(dexcom_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(dexcom_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dexcom_label = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_label, "Dexcom Share");
    lv_obj_set_style_text_font(dexcom_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dexcom_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *dexcom_arrow = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(dexcom_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(dexcom_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(dexcom_btn, dexcom_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // LibreLinkUp button card (disabled)
    lv_obj_t *libre_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(libre_btn, 290, 50);
    lv_obj_align(libre_btn, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(libre_btn, LV_OPA_40, 0);
    lv_obj_set_style_radius(libre_btn, 10, 0);
    lv_obj_set_style_border_width(libre_btn, 0, 0);
    lv_obj_set_style_shadow_width(libre_btn, 0, 0);
    lv_obj_set_style_pad_left(libre_btn, 14, 0);

    lv_obj_t *libre_icon = lv_label_create(libre_btn);
    lv_label_set_text(libre_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(libre_icon, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(libre_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *libre_label = lv_label_create(libre_btn);
    lv_label_set_text(libre_label, "LibreLinkUp");
    lv_obj_set_style_text_font(libre_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(libre_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(libre_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *libre_soon = lv_label_create(libre_btn);
    lv_label_set_text(libre_soon, "Soon");
    lv_obj_set_style_text_font(libre_soon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(libre_soon, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(libre_soon, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(libre_btn, librelinkup_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_auth_screen(void) {
    screen_dexcom_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_auth, &style_bg, 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_auth);
    lv_label_set_text(title, "Dexcom");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_auth);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, dexcom_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Status check
    bool authenticated = dexcom_is_authenticated();

    if (authenticated) {
        // Status card
        lv_obj_t *status_card = lv_obj_create(screen_dexcom_auth);
        lv_obj_set_size(status_card, 290, 60);
        lv_obj_align(status_card, LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_set_style_bg_color(status_card, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(status_card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(status_card, 10, 0);
        lv_obj_set_style_border_width(status_card, 0, 0);
        lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

        // Green accent bar
        lv_obj_t *accent = lv_obj_create(status_card);
        lv_obj_remove_style_all(accent);
        lv_obj_set_size(accent, 3, 40);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 2, 0);

        lv_obj_t *status_icon = lv_label_create(status_card);
        lv_label_set_text(status_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(status_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(status_icon, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t *status = lv_label_create(status_card);
        lv_label_set_text(status, "Connected");
        lv_obj_set_style_text_font(status, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(status, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(status, LV_ALIGN_LEFT_MID, 48, -8);

        lv_obj_t *username_label = lv_label_create(status_card);
        lv_label_set_text(username_label, dexcom_username_buf);
        lv_obj_set_style_text_font(username_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(username_label, LV_ALIGN_LEFT_MID, 48, 10);

        // View Status button (card style)
        lv_obj_t *status_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(status_btn, 290, 40);
        lv_obj_align(status_btn, LV_ALIGN_TOP_MID, 0, 120);
        lv_obj_set_style_bg_color(status_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(status_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(status_btn, 10, 0);
        lv_obj_set_style_border_width(status_btn, 0, 0);
        lv_obj_set_style_shadow_width(status_btn, 0, 0);
        lv_obj_set_style_pad_left(status_btn, 14, 0);
        lv_obj_set_style_bg_color(status_btn, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

        lv_obj_t *status_icon2 = lv_label_create(status_btn);
        lv_label_set_text(status_icon2, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(status_icon2, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(status_icon2, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *status_lbl = lv_label_create(status_btn);
        lv_label_set_text(status_lbl, "View Account Status");
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(status_lbl, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *status_arrow = lv_label_create(status_btn);
        lv_label_set_text(status_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(status_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(status_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(status_btn, dexcom_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Not connected - show login prompt
        lv_obj_t *info = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(info, "Connect your Dexcom Share\naccount to view glucose data");
        lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(info, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, -30);

        // Login button (action)
        lv_obj_t *login_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(login_btn, 250, 48);
        lv_obj_align(login_btn, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Login with Username");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, dexcom_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *note = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(note, "Requires Dexcom Share enabled");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -15);
    }
}

void create_dexcom_login_screen(void) {
    screen_dexcom_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_login, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_dexcom_login);
    lv_label_set_text(header, "Dexcom Login");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_login);
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

    // Username section label
    lv_obj_t *username_title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(username_title, "USERNAME");
    lv_obj_set_style_text_font(username_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(username_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(username_title, 2, 0);
    lv_obj_align(username_title, LV_ALIGN_TOP_LEFT, 18, 48);

    // Username card (tappable)
    lv_obj_t *username_container = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(username_container, 290, 40);
    lv_obj_align(username_container, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(username_container, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(username_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(username_container, 10, 0);
    lv_obj_set_style_border_width(username_container, 0, 0);
    lv_obj_add_flag(username_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(username_container, dexcom_username_tap_event_cb, LV_EVENT_CLICKED, NULL);

    dexcom_username_label = lv_label_create(username_container);
    lv_label_set_text(dexcom_username_label, dexcom_username_buf);
    lv_obj_set_style_text_font(dexcom_username_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_username_label, LV_ALIGN_LEFT_MID, 14, 0);

    if (strlen(dexcom_username_buf) == 0) {
        lv_label_set_text(dexcom_username_label, "Tap to enter");
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }

    lv_obj_t *user_arrow = lv_label_create(username_container);
    lv_label_set_text(user_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(user_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(user_arrow, LV_ALIGN_RIGHT_MID, -4, 0);

    // Password section label
    lv_obj_t *password_title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(password_title, "PASSWORD");
    lv_obj_set_style_text_font(password_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(password_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(password_title, 2, 0);
    lv_obj_align(password_title, LV_ALIGN_TOP_LEFT, 18, 110);

    // Password card (tappable)
    lv_obj_t *password_container = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(password_container, 290, 40);
    lv_obj_align(password_container, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_bg_color(password_container, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(password_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(password_container, 10, 0);
    lv_obj_set_style_border_width(password_container, 0, 0);
    lv_obj_add_flag(password_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(password_container, dexcom_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    dexcom_password_label = lv_label_create(password_container);
    if (strlen(dexcom_password_buf) > 0) {
        int len = strlen(dexcom_password_buf);
        char display[65];
        for (int i = 0; i < len; i++) {
            display[i] = '*';
        }
        display[len] = '\0';
        lv_label_set_text(dexcom_password_label, display);
        lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_password_label, "Tap to enter");
        lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_set_style_text_font(dexcom_password_label, &lv_font_montserrat_14, 0);
    lv_obj_align(dexcom_password_label, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *pass_arrow = lv_label_create(password_container);
    lv_label_set_text(pass_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(pass_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(pass_arrow, LV_ALIGN_RIGHT_MID, -4, 0);

    // Login button (action)
    lv_obj_t *login_btn = lv_btn_create(screen_dexcom_login);
    lv_obj_set_size(login_btn, 250, 48);
    lv_obj_align(login_btn, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Login");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, dexcom_login_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_username_entry_screen(void) {
    screen_dexcom_username_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_username_entry, &style_bg, 0);

    // Ghost cancel button (top left)
    lv_obj_t *cancel_btn = lv_btn_create(screen_dexcom_username_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel_btn, dexcom_username_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);

    // OK button (top right, accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_username_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_username_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Text display at top center between buttons - shows "Username" placeholder or typed text
    dexcom_username_entry_label = lv_label_create(screen_dexcom_username_entry);
    if (strlen(dexcom_username_buf) > 0) {
        lv_label_set_text(dexcom_username_entry_label, dexcom_username_buf);
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_username_entry_label, "Username");
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_set_style_text_font(dexcom_username_entry_label, &lv_font_montserrat_20, 0);
    lv_obj_align(dexcom_username_entry_label, LV_ALIGN_TOP_MID, 0, 12);

    // Hidden textarea for keyboard input
    dexcom_username_entry_ta = lv_textarea_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_username_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_username_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_username_entry_ta, true);
    lv_textarea_set_max_length(dexcom_username_entry_ta, 63);
    lv_textarea_set_text(dexcom_username_entry_ta, dexcom_username_buf);

    // Keyboard
    dexcom_username_entry_keyboard = lv_keyboard_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_keyboard, 320, 165);
    lv_obj_align(dexcom_username_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_username_entry_keyboard, dexcom_username_entry_ta);
    lv_keyboard_set_mode(dexcom_username_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, 0);

    // Wire up event handler with display_label as user_data
    lv_obj_add_event_cb(dexcom_username_entry_ta, dexcom_username_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_username_entry_label);
}

void create_dexcom_password_entry_screen(void) {
    screen_dexcom_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_password_entry, &style_bg, 0);

    // Ghost cancel button (top left)
    lv_obj_t *cancel_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel_btn, dexcom_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);

    // OK button (top right, accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Text display at top center between buttons - shows "Password" placeholder or masked password
    dexcom_password_entry_label = lv_label_create(screen_dexcom_password_entry);
    int len = strlen(dexcom_password_buf);
    if (len > 0) {
        char display[65];
        for (int i = 0; i < len; i++) {
            display[i] = '*';
        }
        display[len] = '\0';
        lv_label_set_text(dexcom_password_entry_label, display);
        lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_password_entry_label, "Password");
        lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_set_style_text_font(dexcom_password_entry_label, &lv_font_montserrat_20, 0);
    lv_obj_align(dexcom_password_entry_label, LV_ALIGN_TOP_MID, 0, 12);

    // Hidden textarea for keyboard input
    dexcom_password_entry_ta = lv_textarea_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_password_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_password_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_password_entry_ta, true);
    lv_textarea_set_password_mode(dexcom_password_entry_ta, false);  // We handle masking
    lv_textarea_set_max_length(dexcom_password_entry_ta, 63);
    lv_textarea_set_text(dexcom_password_entry_ta, dexcom_password_buf);

    // Keyboard
    dexcom_password_entry_keyboard = lv_keyboard_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_keyboard, 320, 165);
    lv_obj_align(dexcom_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_password_entry_keyboard, dexcom_password_entry_ta);
    lv_keyboard_set_mode(dexcom_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, 0);

    // Wire up event handler with display_label as user_data
    lv_obj_add_event_cb(dexcom_password_entry_ta, dexcom_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_password_entry_label);
}

void create_dexcom_status_screen(void) {
    screen_dexcom_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_status, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_dexcom_status);
    lv_label_set_text(header, "Account");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_status);
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
    lv_obj_add_event_cb(back_btn, dexcom_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Status card with green accent bar
    lv_obj_t *status_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(status_card, 290, 60);
    lv_obj_align(status_card, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_card, 10, 0);
    lv_obj_set_style_border_width(status_card, 0, 0);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(status_card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 3, 40);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 2, 0);

    lv_obj_t *status_icon = lv_label_create(status_card);
    lv_label_set_text(status_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status_icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_icon, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *connected_label = lv_label_create(status_card);
    lv_label_set_text(connected_label, "Connected");
    lv_obj_set_style_text_font(connected_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(connected_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(connected_label, LV_ALIGN_LEFT_MID, 48, 0);

    // Info card (CGM Type + Account)
    lv_obj_t *info_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(info_card, 290, 90);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 8);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "Dexcom Share");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 22);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *user_title = lv_label_create(info_card);
    lv_label_set_text(user_title, "ACCOUNT");
    lv_obj_set_style_text_font(user_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(user_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(user_title, 2, 0);
    lv_obj_align(user_title, LV_ALIGN_TOP_LEFT, 14, 50);

    lv_obj_t *user_value = lv_label_create(info_card);
    lv_label_set_text(user_value, dexcom_username_buf);
    lv_obj_set_style_text_font(user_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(user_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(user_value, LV_ALIGN_TOP_LEFT, 14, 64);

    // Logout button (red action)
    lv_obj_t *logout_btn = lv_btn_create(screen_dexcom_status);
    lv_obj_set_size(logout_btn, 200, 40);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_border_width(logout_btn, 0, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Logout");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, dexcom_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

