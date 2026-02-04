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
#include <string.h>

static const char *TAG = "CGM_SCREENS";

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
        lv_scr_load(screen_cgm_menu);
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
            lv_scr_load(screen_dexcom_username_entry);
            lvgl_port_unlock();
        }
    }
}

// Dexcom password tap event handler
static void dexcom_password_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_password_entry);
            lvgl_port_unlock();
        }
    }
}

// Dexcom login button event handler
static void dexcom_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Logging in to Dexcom with username: %s", dexcom_username_buf);

        // Authenticate with Dexcom API
        if (dexcom_authenticate(dexcom_username_buf, dexcom_password_buf) == ESP_OK) {
            ESP_LOGI(TAG, "Dexcom login successful");
            nvs_set_dexcom_credentials(dexcom_username_buf, dexcom_password_buf);
            if (lvgl_port_lock(0)) {
                lv_scr_load(screen_dexcom_status);
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGE(TAG, "Dexcom login failed");
        }
    }
}

// Dexcom username entry keyboard event handler
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Username entry cancelled");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    } else if (code == LV_EVENT_READY) {
        ESP_LOGI(TAG, "Username entry completed: %s", dexcom_username_buf);
        if (dexcom_username_label != NULL) {
            lv_label_set_text(dexcom_username_label, dexcom_username_buf);
        }
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    }
}

// Dexcom username entry field event handler
static void dexcom_username_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_textarea_get_text(dexcom_username_entry_ta);
        strncpy(dexcom_username_buf, text, sizeof(dexcom_username_buf) - 1);
        dexcom_username_buf[sizeof(dexcom_username_buf) - 1] = '\0';
    }
}

// Dexcom password entry keyboard event handler
static void dexcom_password_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Password entry cancelled");
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    } else if (code == LV_EVENT_READY) {
        ESP_LOGI(TAG, "Password entry completed (masked)");
        if (dexcom_password_label != NULL) {
            int len = strlen(dexcom_password_buf);
            if (len == 0) {
                lv_label_set_text(dexcom_password_label, "");
            } else {
                char display[65];
                for (int i = 0; i < len; i++) {
                    display[i] = '*';
                }
                display[len] = '\0';
                lv_label_set_text(dexcom_password_label, display);
            }
        }
        if (lvgl_port_lock(0)) {
            lv_scr_load(screen_dexcom_login);
            lvgl_port_unlock();
        }
    }
}

// Dexcom password entry field event handler
static void dexcom_password_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        const char *text = lv_textarea_get_text(dexcom_password_entry_ta);
        strncpy(dexcom_password_buf, text, sizeof(dexcom_password_buf) - 1);
        dexcom_password_buf[sizeof(dexcom_password_buf) - 1] = '\0';
    }
}

void create_cgm_menu_screen(void) {
    screen_cgm_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_cgm_menu, lv_color_hex(COLOR_BG), 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_cgm_menu);
    lv_label_set_text(title, "CGM Data Source");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Back button (matching alarm settings style)
    lv_obj_t *back_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, cgm_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Dexcom button (active)
    lv_obj_t *dexcom_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(dexcom_btn, 280, 50);
    lv_obj_align(dexcom_btn, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(dexcom_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_t *dexcom_label = lv_label_create(dexcom_btn);
    lv_label_set_text(dexcom_label, "Dexcom");
    lv_obj_set_style_text_font(dexcom_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(dexcom_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(dexcom_label);
    lv_obj_add_event_cb(dexcom_btn, dexcom_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // LibreLinkUp button (placeholder)
    lv_obj_t *libre_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(libre_btn, 280, 50);
    lv_obj_align(libre_btn, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(libre_btn, LV_OPA_50, 0);
    lv_obj_t *libre_label = lv_label_create(libre_btn);
    lv_label_set_text(libre_label, "LibreLinkUp (Coming Soon)");
    lv_obj_set_style_text_font(libre_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(libre_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(libre_label);
    lv_obj_add_event_cb(libre_btn, librelinkup_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_auth_screen(void) {
    screen_dexcom_auth = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_dexcom_auth, lv_color_hex(COLOR_BG), 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_auth);
    lv_label_set_text(title, "Dexcom CGM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Back button (matching alarm settings style)
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_auth);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, dexcom_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Status check
    bool authenticated = dexcom_is_authenticated();

    if (authenticated) {
        // Show connected status (moved up 20px)
        lv_obj_t *status = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(status, LV_SYMBOL_OK " Connected to Dexcom");
        lv_obj_set_style_text_font(status, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(status, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(status, LV_ALIGN_CENTER, 0, -60);

        // Show username
        lv_obj_t *username_label = lv_label_create(screen_dexcom_auth);
        char username_text[96];
        snprintf(username_text, sizeof(username_text), "Account: %s", dexcom_username_buf);
        lv_label_set_text(username_label, username_text);
        lv_obj_set_style_text_font(username_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(username_label, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_align(username_label, LV_ALIGN_CENTER, 0, -30);

        // View Status button (settings moved to main Settings > Alarm)
        lv_obj_t *status_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(status_btn, 220, 45);
        lv_obj_align(status_btn, LV_ALIGN_CENTER, 0, 15);
        lv_obj_add_style(status_btn, &style_card, 0);
        lv_obj_set_style_bg_color(status_btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);

        lv_obj_t *status_lbl = lv_label_create(status_btn);
        lv_label_set_text(status_lbl, "View Account Status");
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(status_lbl);
        lv_obj_add_event_cb(status_btn, dexcom_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Not connected - show login prompt
        lv_obj_t *info = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(info, "Connect your Dexcom Share account");
        lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(info, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, -40);

        lv_obj_t *info2 = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(info2, "Use your Dexcom app credentials");
        lv_obj_set_style_text_font(info2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(info2, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(info2, LV_ALIGN_CENTER, 0, -10);

        // Login button
        lv_obj_t *login_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(login_btn, 220, 50);
        lv_obj_align(login_btn, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
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
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_login);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Username label
    lv_obj_t *username_title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(username_title, "Username:");
    lv_obj_set_style_text_font(username_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(username_title, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(username_title, LV_ALIGN_TOP_LEFT, 10, 50);

    // Username container (tappable)
    lv_obj_t *username_container = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(username_container, 300, 35);
    lv_obj_align(username_container, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_add_style(username_container, &style_card, 0);
    lv_obj_add_flag(username_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(username_container, dexcom_username_tap_event_cb, LV_EVENT_CLICKED, NULL);

    dexcom_username_label = lv_label_create(username_container);
    lv_label_set_text(dexcom_username_label, dexcom_username_buf);  // Show existing value
    lv_obj_set_style_text_font(dexcom_username_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_username_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Placeholder text if empty
    if (strlen(dexcom_username_buf) == 0) {
        lv_label_set_text(dexcom_username_label, "Tap to enter");
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }

    // Password label
    lv_obj_t *password_title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(password_title, "Password:");
    lv_obj_set_style_text_font(password_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(password_title, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(password_title, LV_ALIGN_TOP_LEFT, 10, 110);

    // Password container (tappable)
    lv_obj_t *password_container = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(password_container, 300, 35);
    lv_obj_align(password_container, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_add_style(password_container, &style_card, 0);
    lv_obj_add_flag(password_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(password_container, dexcom_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    dexcom_password_label = lv_label_create(password_container);
    // Show masked password if exists
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
    lv_obj_set_style_text_font(dexcom_password_label, &lv_font_montserrat_16, 0);
    lv_obj_align(dexcom_password_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Login button
    lv_obj_t *login_btn = lv_btn_create(screen_dexcom_login);
    lv_obj_set_size(login_btn, 200, 45);
    lv_obj_align(login_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_style(login_btn, &style_card, 0);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), LV_STATE_PRESSED);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Login");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, dexcom_login_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_username_entry_screen(void) {
    screen_dexcom_username_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_username_entry, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_dexcom_username_entry);
    lv_label_set_text(header, "Enter Username");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Username display
    lv_obj_t *display_container = lv_obj_create(screen_dexcom_username_entry);
    lv_obj_set_size(display_container, 300, 35);
    lv_obj_align(display_container, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_style(display_container, &style_card, 0);

    dexcom_username_entry_label = lv_label_create(display_container);
    lv_label_set_text(dexcom_username_entry_label, dexcom_username_buf);  // Show existing value
    lv_obj_set_style_text_font(dexcom_username_entry_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_username_entry_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Hidden textarea for keyboard input
    dexcom_username_entry_ta = lv_textarea_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_ta, 1, 1);  // Hidden
    lv_obj_set_pos(dexcom_username_entry_ta, -100, -100);  // Off screen
    lv_textarea_set_one_line(dexcom_username_entry_ta, true);
    lv_textarea_set_max_length(dexcom_username_entry_ta, 63);
    lv_textarea_set_text(dexcom_username_entry_ta, dexcom_username_buf);  // Load existing value

    // LVGL keyboard
    dexcom_username_entry_keyboard = lv_keyboard_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_keyboard, 320, 165);
    lv_obj_align(dexcom_username_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_username_entry_keyboard, dexcom_username_entry_ta);
    lv_keyboard_set_mode(dexcom_username_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Style keyboard
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, 0);

    // Add event handlers
    lv_obj_add_event_cb(dexcom_username_entry_keyboard, dexcom_username_entry_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(dexcom_username_entry_ta, dexcom_username_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void create_dexcom_password_entry_screen(void) {
    screen_dexcom_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_password_entry, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_dexcom_password_entry);
    lv_label_set_text(header, "Enter Password");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    // Password display (masked)
    lv_obj_t *display_container = lv_obj_create(screen_dexcom_password_entry);
    lv_obj_set_size(display_container, 300, 35);
    lv_obj_align(display_container, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_style(display_container, &style_card, 0);

    dexcom_password_entry_label = lv_label_create(display_container);
    // Show masked password
    int len = strlen(dexcom_password_buf);
    if (len > 0) {
        char display[65];
        for (int i = 0; i < len; i++) {
            display[i] = '*';
        }
        display[len] = '\0';
        lv_label_set_text(dexcom_password_entry_label, display);
    } else {
        lv_label_set_text(dexcom_password_entry_label, "");
    }
    lv_obj_set_style_text_font(dexcom_password_entry_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(dexcom_password_entry_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Hidden textarea for keyboard input
    dexcom_password_entry_ta = lv_textarea_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_ta, 1, 1);  // Hidden
    lv_obj_set_pos(dexcom_password_entry_ta, -100, -100);  // Off screen
    lv_textarea_set_one_line(dexcom_password_entry_ta, true);
    lv_textarea_set_password_mode(dexcom_password_entry_ta, false);  // We handle masking ourselves
    lv_textarea_set_max_length(dexcom_password_entry_ta, 63);
    lv_textarea_set_text(dexcom_password_entry_ta, dexcom_password_buf);  // Load existing value

    // LVGL keyboard
    dexcom_password_entry_keyboard = lv_keyboard_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_keyboard, 320, 165);
    lv_obj_align(dexcom_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_password_entry_keyboard, dexcom_password_entry_ta);
    lv_keyboard_set_mode(dexcom_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Style keyboard
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, 0);

    // Add event handlers
    lv_obj_add_event_cb(dexcom_password_entry_keyboard, dexcom_password_entry_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(dexcom_password_entry_ta, dexcom_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void create_dexcom_status_screen(void) {
    screen_dexcom_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_status, &style_bg, 0);

    // Header
    lv_obj_t *header = lv_label_create(screen_dexcom_status);
    lv_label_set_text(header, "Dexcom Account");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(screen_dexcom_status);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_style(back_btn, &style_card, 0);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, dexcom_back_event_cb, LV_EVENT_CLICKED, NULL);

    // Status Card Container (Y=45, height=70)
    lv_obj_t *status_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(status_card, 280, 70);
    lv_obj_align(status_card, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_add_style(status_card, &style_card, 0);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(0x1a3d1a), 0);  // Dark green
    lv_obj_set_style_border_width(status_card, 0, 0);

    // Status icon inside card
    lv_obj_t *status_icon = lv_label_create(status_card);
    lv_label_set_text(status_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(status_icon, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_icon, LV_ALIGN_LEFT_MID, 18, 0);

    // Connected status text
    lv_obj_t *connected_label = lv_label_create(status_card);
    lv_label_set_text(connected_label, "Connected");
    lv_obj_set_style_text_font(connected_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(connected_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(connected_label, LV_ALIGN_CENTER, 12, 0);

    // Info Section starts at Y=125, each field is 30px tall
    // CGM Type (Y=125-155)
    lv_obj_t *cgm_title = lv_label_create(screen_dexcom_status);
    lv_label_set_text(cgm_title, "CGM Type");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 25, 125);

    lv_obj_t *cgm_value = lv_label_create(screen_dexcom_status);
    lv_label_set_text(cgm_value, "Dexcom Share");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 25, 141);

    // Account (Y=160-190)
    lv_obj_t *user_title = lv_label_create(screen_dexcom_status);
    lv_label_set_text(user_title, "Account");
    lv_obj_set_style_text_font(user_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(user_title, lv_color_hex(COLOR_TEXT_GRAY), 0);
    lv_obj_align(user_title, LV_ALIGN_TOP_LEFT, 25, 160);

    lv_obj_t *user_value = lv_label_create(screen_dexcom_status);
    lv_label_set_text(user_value, dexcom_username_buf);
    lv_obj_set_style_text_font(user_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(user_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(user_value, LV_ALIGN_TOP_LEFT, 25, 176);

    // Logout button (Y=195, height=40, smaller and positioned absolutely)
    lv_obj_t *logout_btn = lv_btn_create(screen_dexcom_status);
    lv_obj_set_size(logout_btn, 160, 38);
    lv_obj_align(logout_btn, LV_ALIGN_TOP_MID, 0, 197);
    lv_obj_add_style(logout_btn, &style_card, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), 0);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Logout");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, dexcom_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

