/*
 * cgm_screens.c
 *
 * CGM provider configuration screens implementation
 */

#include "cgm_screens.h"
#include "shared_state.h"
#include "main.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "cgm_types.h"
#include "nvs_config.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include "sd_logger.h"
#include "ui/menu_screen.h"
#include "ui/home_screen.h"

static const char *TAG = "CGM_SCREENS";
static volatile bool dexcom_login_in_progress = false;

// Password visibility toggle
static bool dexcom_pwd_visible = false;
static lv_obj_t *dexcom_pwd_eye_label = NULL;

static void dexcom_login_task(void *pvParameters) {
    char username[64] = {0};
    char password[64] = {0};
    strncpy(username, dexcom_username_buf, sizeof(username) - 1);
    strncpy(password, dexcom_password_buf, sizeof(password) - 1);

    ESP_LOGI(TAG, "Dexcom login task started for user: %s", username);

    if (dexcom_authenticate(username, password) == ESP_OK) {
        ESP_LOGI(TAG, "Dexcom login successful");
        sd_log(TAG, "Dexcom: login OK for %s", username);
        nvs_set_dexcom_credentials(username, password);
        nvs_save_cgm_type("dexcom");

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after Dexcom login...");
            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 5, &glucose_task_handle);
        }

        ESP_LOGI(TAG, "Triggering immediate glucose fetch after successful login");
        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(100)) {
            lv_scr_load(screen_home);
            // Clean up all CGM screens
            if (screen_dexcom_login != NULL) { lv_obj_del(screen_dexcom_login); screen_dexcom_login = NULL; }
            if (screen_dexcom_auth != NULL) { lv_obj_del(screen_dexcom_auth); screen_dexcom_auth = NULL; }
            if (screen_cgm_menu != NULL) { lv_obj_del(screen_cgm_menu); screen_cgm_menu = NULL; }
            home_screen_active = true;
            pause_background_tasks = false;

            // Show one-time login success overlay
            show_login_success_overlay_ui();

            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "Failed to lock LVGL to return to home screen after login");
        }
    } else {
        ESP_LOGE(TAG, "Dexcom login failed");
        sd_log(TAG, "Dexcom: login FAILED for %s", username);
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
static void dexcom_to_auth_event_cb(lv_event_t *e);

// Dexcom back button event handler — return to CGM menu
static void dexcom_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_cgm_menu_screen();
            lv_scr_load(screen_cgm_menu);
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
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
        sd_log(TAG, "Dexcom: user logout");

        // Recreate auth screen to show login option
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Delete status screen we came from
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// CGM back button event handler
static void cgm_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_menu_screen();
            lv_scr_load(screen_menu);
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Dexcom button event handler
static void dexcom_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Single-screen rule: delete cgm_menu after loading auth
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// LibreLinkUp button event handler
static void librelinkup_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "LibreLinkUp button pressed");
        if (lvgl_port_lock(0)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_cgm_menu != NULL) {
                lv_obj_del(screen_cgm_menu);
                screen_cgm_menu = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Dexcom view status button event handler
static void dexcom_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            create_dexcom_status_screen();
            lv_scr_load(screen_dexcom_status);
            // Single-screen rule: delete auth after loading status
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            lvgl_port_unlock();
        }
    }
}

// Dexcom open login button event handler
static void dexcom_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
            }
            create_dexcom_login_screen();
            lv_scr_load(screen_dexcom_login);
            // Single-screen rule: delete auth after loading login
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
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
            // Single-screen rule: delete login after loading entry
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
                dexcom_username_label = NULL;
                dexcom_password_label = NULL;
            }
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
            // Single-screen rule: delete login after loading entry
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
                dexcom_username_label = NULL;
                dexcom_password_label = NULL;
            }
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

        // Stop existing glucose task — it reads provider type at start,
        // so it must be deleted and recreated after provider switch.
        if (glucose_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting glucose task for provider switch");
            vTaskDelete(glucose_task_handle);
            glucose_task_handle = NULL;
        }

        // Close any existing Libre connection before switching
        if (libre_is_authenticated()) {
            ESP_LOGI(TAG, "Closing Libre connection for provider switch");
            libre_close_persistent_client();
            libre_logout();
        }

        if (xTaskCreate(dexcom_login_task, "dexcom_login", 6144, NULL, 2, NULL) != pdPASS) {
            dexcom_login_in_progress = false;
            ESP_LOGE(TAG, "Failed to create Dexcom login task");
        }
    }
}

// Dexcom username entry keyboard event handler
static void dexcom_username_entry_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Username entry cancelled");
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Username entry completed: %s", dexcom_username_buf);
        }
        // Both CANCEL and READY return to login screen
        if (simulated_code == LV_EVENT_CANCEL || simulated_code == LV_EVENT_READY) {
            if (lvgl_port_lock(0)) {
                // Recreate login (was deleted; labels auto-populate from buffers)
                create_dexcom_login_screen();
                lv_scr_load(screen_dexcom_login);
                if (screen_dexcom_username_entry != NULL) {
                    lv_obj_del(screen_dexcom_username_entry);
                    screen_dexcom_username_entry = NULL;
                }
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

    if (code == LV_EVENT_CLICKED) {
        lv_event_code_t simulated_code = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);
        if (simulated_code == LV_EVENT_CANCEL) {
            ESP_LOGI(TAG, "Password entry cancelled");
        } else if (simulated_code == LV_EVENT_READY) {
            ESP_LOGI(TAG, "Password entry completed (masked)");
        }
        // Both CANCEL and READY return to login screen
        if (simulated_code == LV_EVENT_CANCEL || simulated_code == LV_EVENT_READY) {
            if (lvgl_port_lock(0)) {
                // Recreate login (was deleted; labels auto-populate from buffers)
                create_dexcom_login_screen();
                lv_scr_load(screen_dexcom_login);
                if (screen_dexcom_password_entry != NULL) {
                    lv_obj_del(screen_dexcom_password_entry);
                    screen_dexcom_password_entry = NULL;
                }
                lvgl_port_unlock();
            }
        }
    }
}

// Dexcom password entry field event handler (shows last character, masks rest)
// Eye toggle callback for Dexcom password screen
static void dexcom_eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dexcom_pwd_visible = !dexcom_pwd_visible;
    if (dexcom_pwd_eye_label) {
        lv_label_set_text(dexcom_pwd_eye_label,
                          dexcom_pwd_visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
    if (dexcom_password_entry_label) {
        int len = strlen(dexcom_password_buf);
        if (len > 0) {
            if (dexcom_pwd_visible) {
                lv_label_set_text(dexcom_password_entry_label, dexcom_password_buf);
            } else {
                char display[65];
                for (int i = 0; i < len; i++) display[i] = '*';
                display[len] = '\0';
                lv_label_set_text(dexcom_password_entry_label, display);
            }
            lv_obj_set_style_text_color(dexcom_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        }
    }
}

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

                // Update display label based on visibility state
                if (display_label != NULL) {
                    int len = strlen(text);
                    if (len > 0) {
                        if (dexcom_pwd_visible) {
                            // Show all characters when visible
                            lv_label_set_text(display_label, text);
                        } else {
                            char display[65];
                            // Mask all characters except the last one
                            for (int i = 0; i < len - 1; i++) {
                                display[i] = '*';
                            }
                            // Show the last character
                            display[len - 1] = text[len - 1];
                            display[len] = '\0';
                            lv_label_set_text(display_label, display);
                        }
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

// Navigate back to dexcom auth screen (used by login and status screens)
static void dexcom_to_auth_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            // Recreate auth screen (was deleted when navigating deeper)
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            // Delete whichever screen we came from
            if (screen_dexcom_login != NULL) {
                lv_obj_del(screen_dexcom_login);
                screen_dexcom_login = NULL;
            }
            if (screen_dexcom_status != NULL) {
                lv_obj_del(screen_dexcom_status);
                screen_dexcom_status = NULL;
            }
            lvgl_port_unlock();
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

    // LibreLinkUp button card (active)
    lv_obj_t *libre_btn = lv_btn_create(screen_cgm_menu);
    lv_obj_set_size(libre_btn, 290, 50);
    lv_obj_align(libre_btn, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(libre_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(libre_btn, 10, 0);
    lv_obj_set_style_border_width(libre_btn, 0, 0);
    lv_obj_set_style_shadow_width(libre_btn, 0, 0);
    lv_obj_set_style_pad_left(libre_btn, 14, 0);
    lv_obj_set_style_bg_color(libre_btn, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

    lv_obj_t *libre_icon = lv_label_create(libre_btn);
    lv_label_set_text(libre_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(libre_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(libre_icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *libre_label = lv_label_create(libre_btn);
    lv_label_set_text(libre_label, "LibreLinkUp");
    lv_obj_set_style_text_font(libre_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(libre_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(libre_label, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *libre_arrow = lv_label_create(libre_btn);
    lv_label_set_text(libre_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(libre_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(libre_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(libre_btn, librelinkup_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void create_dexcom_auth_screen(void) {
    screen_dexcom_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_auth, &style_bg, 0);

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

    bool authenticated = dexcom_is_authenticated();

    if (authenticated) {
        // Title
        lv_obj_t *title = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(title, "Dexcom");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Green hero circle with checkmark
        lv_obj_t *hero = lv_obj_create(screen_dexcom_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 52, 52);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_width(hero, 2, 0);
        lv_obj_set_style_border_opa(hero, LV_OPA_COVER, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(hero_icon);

        // "Connected" status
        lv_obj_t *connected = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(connected, "Connected");
        lv_obj_set_style_text_font(connected, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(connected, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(connected, LV_ALIGN_TOP_MID, 0, 112);

        // Username
        lv_obj_t *username = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(username, dexcom_username_buf);
        lv_obj_set_style_text_font(username, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(username, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(username, LV_ALIGN_TOP_MID, 0, 134);

        // Account Details card-button
        lv_obj_t *view_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(view_btn, 290, 44);
        lv_obj_align(view_btn, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(view_btn, 10, 0);
        lv_obj_set_style_border_width(view_btn, 0, 0);
        lv_obj_set_style_shadow_width(view_btn, 0, 0);
        lv_obj_set_style_pad_left(view_btn, 14, 0);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

        lv_obj_t *view_icon = lv_label_create(view_btn);
        lv_label_set_text(view_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(view_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(view_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *view_label = lv_label_create(view_btn);
        lv_label_set_text(view_label, "Account Details");
        lv_obj_set_style_text_font(view_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(view_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(view_label, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *view_arrow = lv_label_create(view_btn);
        lv_label_set_text(view_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(view_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(view_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(view_btn, dexcom_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Hero circle with connection icon
        lv_obj_t *hero = lv_obj_create(screen_dexcom_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 56, 56);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(hero_icon);

        // Brand title
        lv_obj_t *title = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(title, "Dexcom Share");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 106);

        // Subtitle
        lv_obj_t *subtitle = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(subtitle, "Connect your account to view\nreal-time glucose readings");
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 130);

        // Sign In button
        lv_obj_t *login_btn = lv_btn_create(screen_dexcom_auth);
        lv_obj_set_size(login_btn, 290, 46);
        lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 174);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Sign In");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, dexcom_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        // Footer note
        lv_obj_t *note = lv_label_create(screen_dexcom_auth);
        lv_label_set_text(note, "Requires Dexcom Share enabled");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

void create_dexcom_login_screen(void) {
    screen_dexcom_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_login, &style_bg, 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_login);
    lv_label_set_text(title, "Sign In");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

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
    lv_obj_add_event_cb(back_btn, dexcom_to_auth_event_cb, LV_EVENT_CLICKED, NULL);

    // Unified form card with username + password rows
    lv_obj_t *form_card = lv_obj_create(screen_dexcom_login);
    lv_obj_set_size(form_card, 290, 88);
    lv_obj_align(form_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(form_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(form_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(form_card, 10, 0);
    lv_obj_set_style_border_width(form_card, 0, 0);
    lv_obj_set_style_pad_all(form_card, 0, 0);
    lv_obj_clear_flag(form_card, LV_OBJ_FLAG_SCROLLABLE);

    // Username row (top half, clickable)
    lv_obj_t *user_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(user_row);
    lv_obj_set_size(user_row, 290, 43);
    lv_obj_align(user_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(user_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(user_row, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(user_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *user_icon = lv_label_create(user_row);
    lv_label_set_text(user_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(user_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(user_icon, LV_ALIGN_LEFT_MID, 14, 0);

    dexcom_username_label = lv_label_create(user_row);
    lv_obj_set_style_text_font(dexcom_username_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(dexcom_username_label, 195);
    lv_label_set_long_mode(dexcom_username_label, LV_LABEL_LONG_DOT);
    if (strlen(dexcom_username_buf) > 0) {
        lv_label_set_text(dexcom_username_label, dexcom_username_buf);
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_username_label, "Username");
        lv_obj_set_style_text_color(dexcom_username_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_username_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *user_arrow = lv_label_create(user_row);
    lv_label_set_text(user_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(user_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(user_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(user_row, dexcom_username_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Divider between rows
    lv_obj_t *div = lv_obj_create(form_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 258, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Password row (bottom half, clickable)
    lv_obj_t *pass_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_size(pass_row, 290, 44);
    lv_obj_align(pass_row, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(pass_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pass_row, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pass_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *pass_icon = lv_label_create(pass_row);
    lv_label_set_text(pass_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(pass_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(pass_icon, LV_ALIGN_LEFT_MID, 14, 0);

    dexcom_password_label = lv_label_create(pass_row);
    lv_obj_set_style_text_font(dexcom_password_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(dexcom_password_label, 195);
    lv_label_set_long_mode(dexcom_password_label, LV_LABEL_LONG_DOT);
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
        lv_label_set_text(dexcom_password_label, "Password");
        lv_obj_set_style_text_color(dexcom_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_password_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *pass_arrow = lv_label_create(pass_row);
    lv_label_set_text(pass_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(pass_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(pass_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(pass_row, dexcom_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Sign In button
    lv_obj_t *login_btn = lv_btn_create(screen_dexcom_login);
    lv_obj_set_size(login_btn, 290, 48);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_icon = lv_label_create(login_btn);
    lv_label_set_text(login_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(login_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(login_icon, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Sign In");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, dexcom_login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Hint text
    lv_obj_t *hint = lv_label_create(screen_dexcom_login);
    lv_label_set_text(hint, "Tap a field to enter credentials");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void create_dexcom_username_entry_screen(void) {
    screen_dexcom_username_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_username_entry, &style_bg, 0);

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_dexcom_username_entry);
    lv_label_set_text(field_title, "USERNAME");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    // Ghost cancel button
    lv_obj_t *cancel_btn = lv_btn_create(screen_dexcom_username_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 19);
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

    // OK button (accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_username_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 19);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_username_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Input display label (centered between buttons)
    dexcom_username_entry_label = lv_label_create(screen_dexcom_username_entry);
    lv_obj_set_style_text_font(dexcom_username_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dexcom_username_entry_label, 210);
    lv_label_set_long_mode(dexcom_username_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(dexcom_username_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(dexcom_username_buf) > 0) {
        lv_label_set_text(dexcom_username_entry_label, dexcom_username_buf);
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(dexcom_username_entry_label, "Username");
        lv_obj_set_style_text_color(dexcom_username_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(dexcom_username_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Blue accent underline
    lv_obj_t *accent = lv_obj_create(screen_dexcom_username_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea for keyboard input
    dexcom_username_entry_ta = lv_textarea_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_username_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_username_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_username_entry_ta, true);
    lv_textarea_set_max_length(dexcom_username_entry_ta, 63);
    lv_textarea_set_text(dexcom_username_entry_ta, dexcom_username_buf);

    // Keyboard — glass theme
    dexcom_username_entry_keyboard = lv_keyboard_create(screen_dexcom_username_entry);
    lv_obj_set_size(dexcom_username_entry_keyboard, 320, 175);
    lv_obj_align(dexcom_username_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_username_entry_keyboard, dexcom_username_entry_ta);
    lv_keyboard_set_mode(dexcom_username_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard background
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dexcom_username_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(dexcom_username_entry_keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(dexcom_username_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(dexcom_username_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(dexcom_username_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(dexcom_username_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(dexcom_username_entry_keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight
    lv_obj_set_style_bg_color(dexcom_username_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(dexcom_username_entry_ta, dexcom_username_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_username_entry_label);
}

void create_dexcom_password_entry_screen(void) {
    screen_dexcom_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_password_entry, &style_bg, 0);

    // Reset password visibility state
    dexcom_pwd_visible = false;

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_dexcom_password_entry);
    lv_label_set_text(field_title, "PASSWORD");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    // Ghost cancel button
    lv_obj_t *cancel_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 19);
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

    // Eye toggle button (next to OK)
    lv_obj_t *eye_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(eye_btn, 36, 36);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, -45, 19);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(0x2a3545), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(eye_btn, 0, 0);
    lv_obj_set_style_shadow_width(eye_btn, 0, 0);
    lv_obj_set_style_radius(eye_btn, 8, 0);

    dexcom_pwd_eye_label = lv_label_create(eye_btn);
    lv_label_set_text(dexcom_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(dexcom_pwd_eye_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_center(dexcom_pwd_eye_label);
    lv_obj_add_event_cb(eye_btn, dexcom_eye_btn_cb, LV_EVENT_CLICKED, NULL);

    // OK button (accent blue)
    lv_obj_t *ok_btn = lv_btn_create(screen_dexcom_password_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 19);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, dexcom_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Input display label (centered between buttons)
    dexcom_password_entry_label = lv_label_create(screen_dexcom_password_entry);
    lv_obj_set_style_text_font(dexcom_password_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dexcom_password_entry_label, 210);
    lv_label_set_long_mode(dexcom_password_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(dexcom_password_entry_label, LV_TEXT_ALIGN_CENTER, 0);
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
    lv_obj_align(dexcom_password_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Blue accent underline
    lv_obj_t *accent = lv_obj_create(screen_dexcom_password_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea for keyboard input
    dexcom_password_entry_ta = lv_textarea_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_ta, 1, 1);
    lv_obj_set_pos(dexcom_password_entry_ta, -100, -100);
    lv_obj_add_flag(dexcom_password_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(dexcom_password_entry_ta, true);
    lv_textarea_set_password_mode(dexcom_password_entry_ta, false);
    lv_textarea_set_max_length(dexcom_password_entry_ta, 63);
    lv_textarea_set_text(dexcom_password_entry_ta, dexcom_password_buf);

    // Keyboard — glass theme
    dexcom_password_entry_keyboard = lv_keyboard_create(screen_dexcom_password_entry);
    lv_obj_set_size(dexcom_password_entry_keyboard, 320, 175);
    lv_obj_align(dexcom_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(dexcom_password_entry_keyboard, dexcom_password_entry_ta);
    lv_keyboard_set_mode(dexcom_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard background
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dexcom_password_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(dexcom_password_entry_keyboard, 2, 0);

    // Glass key styling
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(dexcom_password_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(dexcom_password_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(dexcom_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(dexcom_password_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(dexcom_password_entry_keyboard, 0, LV_PART_ITEMS);

    // Pressed key highlight
    lv_obj_set_style_bg_color(dexcom_password_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(dexcom_password_entry_ta, dexcom_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, dexcom_password_entry_label);
}

void create_dexcom_status_screen(void) {
    screen_dexcom_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_dexcom_status, &style_bg, 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_dexcom_status);
    lv_label_set_text(title, "Account");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button (navigates to auth screen, not CGM menu)
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
    lv_obj_add_event_cb(back_btn, dexcom_to_auth_event_cb, LV_EVENT_CLICKED, NULL);

    // Profile card with avatar, username, and status
    lv_obj_t *profile_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(profile_card, 290, 60);
    lv_obj_align(profile_card, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(profile_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(profile_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(profile_card, 10, 0);
    lv_obj_set_style_border_width(profile_card, 0, 0);
    lv_obj_clear_flag(profile_card, LV_OBJ_FLAG_SCROLLABLE);

    // Green accent bar
    lv_obj_t *accent = lv_obj_create(profile_card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 3, 40);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 2, 0);

    // Avatar circle with initial letter
    lv_obj_t *avatar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);

    char initial[2] = {0};
    if (strlen(dexcom_username_buf) > 0) {
        initial[0] = dexcom_username_buf[0];
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    } else {
        initial[0] = 'D';
    }
    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(initial_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(initial_label);

    // Username (truncate with ellipsis if too long)
    lv_obj_t *name_label = lv_label_create(profile_card);
    lv_label_set_text(name_label, dexcom_username_buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 210);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, -8);

    // Connected status
    lv_obj_t *status_label = lv_label_create(profile_card);
    lv_label_set_text(status_label, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 62, 10);

    // Info card (CGM Type + Account)
    lv_obj_t *info_card = lv_obj_create(screen_dexcom_status);
    lv_obj_set_size(info_card, 290, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 114);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_set_style_pad_all(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "Dexcom Share");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 20);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *user_title = lv_label_create(info_card);
    lv_label_set_text(user_title, "ACCOUNT");
    lv_obj_set_style_text_font(user_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(user_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(user_title, 2, 0);
    lv_obj_align(user_title, LV_ALIGN_TOP_LEFT, 14, 46);

    lv_obj_t *user_value = lv_label_create(info_card);
    lv_label_set_text(user_value, dexcom_username_buf);
    lv_obj_set_style_text_font(user_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(user_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(user_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(user_value, 260);
    lv_obj_align(user_value, LV_ALIGN_TOP_LEFT, 14, 60);

    // Ghost red logout button
    lv_obj_t *logout_btn = lv_btn_create(screen_dexcom_status);
    lv_obj_set_size(logout_btn, 180, 38);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(logout_btn, 1, 0);
    lv_obj_set_style_border_opa(logout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Sign Out");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, dexcom_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
// LibreLinkUp Screens
// ============================================================================

static volatile bool libre_login_in_progress = false;

// Libre password visibility toggle
static bool libre_pwd_visible = false;
static lv_obj_t *libre_pwd_eye_label = NULL;

// Libre UI widget references (module-local)
static lv_obj_t *libre_email_label = NULL;
static lv_obj_t *libre_password_label = NULL;
static lv_obj_t *libre_email_entry_label = NULL;
static lv_obj_t *libre_email_entry_ta = NULL;
static lv_obj_t *libre_email_entry_keyboard = NULL;
static lv_obj_t *libre_password_entry_label = NULL;
static lv_obj_t *libre_password_entry_ta = NULL;
static lv_obj_t *libre_password_entry_keyboard = NULL;

// Forward declarations for Libre event handlers
static void libre_back_event_cb(lv_event_t *e);
static void libre_to_auth_event_cb(lv_event_t *e);
static void libre_open_login_event_cb(lv_event_t *e);
static void libre_view_status_btn_event_cb(lv_event_t *e);
static void libre_email_tap_event_cb(lv_event_t *e);
static void libre_password_tap_event_cb(lv_event_t *e);
static void libre_login_btn_event_cb(lv_event_t *e);
static void libre_email_entry_keyboard_event_cb(lv_event_t *e);
static void libre_email_entry_field_event_cb(lv_event_t *e);
static void libre_password_entry_keyboard_event_cb(lv_event_t *e);
static void libre_password_entry_field_event_cb(lv_event_t *e);
static void libre_logout_btn_event_cb(lv_event_t *e);

// Helper: delete all Libre screens
static void libre_cleanup_screens(void) {
    if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
    if (screen_libre_email_entry != NULL) { lv_obj_del(screen_libre_email_entry); screen_libre_email_entry = NULL; }
    if (screen_libre_password_entry != NULL) { lv_obj_del(screen_libre_password_entry); screen_libre_password_entry = NULL; }
    if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
    if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
}

// Libre login task (runs on separate FreeRTOS task to avoid blocking LVGL)
static void libre_login_task(void *pvParameters) {
    char email[64] = {0};
    char password[64] = {0};
    strncpy(email, libre_email_buf, sizeof(email) - 1);
    strncpy(password, libre_password_buf, sizeof(password) - 1);

    ESP_LOGI(TAG, "Libre login task started for email: %s", email);

    if (libre_authenticate(email, password) == ESP_OK) {
        ESP_LOGI(TAG, "Libre login successful");
        sd_log(TAG, "Libre: login OK for %s", email);
        nvs_set_libre_credentials(email, password);
        nvs_save_cgm_type("libre");

        if (glucose_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting glucose update task after Libre login...");
            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 5, &glucose_task_handle);
        }

        glucose_force_fetch_requested = true;

        if (lvgl_port_lock(100)) {
            lv_scr_load(screen_home);
            libre_cleanup_screens();
            if (screen_cgm_menu != NULL) { lv_obj_del(screen_cgm_menu); screen_cgm_menu = NULL; }
            home_screen_active = true;
            pause_background_tasks = false;
            show_login_success_overlay_ui();
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGE(TAG, "Libre login failed");
        sd_log(TAG, "Libre: login FAILED for %s", email);
    }

    libre_login_in_progress = false;
    vTaskDelete(NULL);
}

// Libre back → CGM menu
static void libre_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_cgm_menu_screen();
            lv_scr_load(screen_cgm_menu);
            libre_cleanup_screens();
            lvgl_port_unlock();
        }
    }
}

// Libre status/login → auth screen
static void libre_to_auth_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Open Libre login screen
static void libre_open_login_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// View Libre status
static void libre_view_status_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_libre_status_screen();
            lv_scr_load(screen_libre_status);
            if (screen_libre_auth != NULL) { lv_obj_del(screen_libre_auth); screen_libre_auth = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap email field
static void libre_email_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_libre_email_entry_screen();
            lv_scr_load(screen_libre_email_entry);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Tap password field
static void libre_password_tap_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            create_libre_password_entry_screen();
            lv_scr_load(screen_libre_password_entry);
            if (screen_libre_login != NULL) { lv_obj_del(screen_libre_login); screen_libre_login = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Sign In button
static void libre_login_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (libre_login_in_progress) {
            ESP_LOGW(TAG, "Libre login already in progress");
            return;
        }
        if (strlen(libre_email_buf) == 0 || strlen(libre_password_buf) == 0) {
            ESP_LOGW(TAG, "Email or password is empty");
            return;
        }
        libre_login_in_progress = true;

        // Stop existing glucose task — it reads provider type at start,
        // so it must be deleted and recreated after provider switch.
        if (glucose_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting glucose task for provider switch");
            vTaskDelete(glucose_task_handle);
            glucose_task_handle = NULL;
        }

        // Close any existing Dexcom connection before switching
        if (dexcom_is_authenticated()) {
            ESP_LOGI(TAG, "Closing Dexcom connection for provider switch");
            dexcom_close_persistent_client();
            dexcom_logout();
        }

        xTaskCreate(libre_login_task, "libre_login", 6144, NULL, 5, NULL);
    }
}

// Email entry keyboard events
static void libre_email_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            // OK — save text and go back to login
            const char *text = lv_textarea_get_text(libre_email_entry_ta);
            strncpy(libre_email_buf, text, sizeof(libre_email_buf) - 1);
            libre_email_buf[sizeof(libre_email_buf) - 1] = '\0';
        }
        // Both OK and Cancel go back to login
        if (lvgl_port_lock(0)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_email_entry != NULL) { lv_obj_del(screen_libre_email_entry); screen_libre_email_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Email entry field value changed
static void libre_email_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);
        const char *text = lv_textarea_get_text(libre_email_entry_ta);
        if (strlen(text) > 0) {
            lv_label_set_text(display_label, text);
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        } else {
            lv_label_set_text(display_label, "Email");
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }
}

// Password entry keyboard events
static void libre_password_entry_keyboard_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_event_code_t action = (lv_event_code_t)(intptr_t)lv_event_get_user_data(e);

        if (action == LV_EVENT_READY) {
            const char *text = lv_textarea_get_text(libre_password_entry_ta);
            strncpy(libre_password_buf, text, sizeof(libre_password_buf) - 1);
            libre_password_buf[sizeof(libre_password_buf) - 1] = '\0';
        }
        if (lvgl_port_lock(0)) {
            create_libre_login_screen();
            lv_scr_load(screen_libre_login);
            if (screen_libre_password_entry != NULL) { lv_obj_del(screen_libre_password_entry); screen_libre_password_entry = NULL; }
            lvgl_port_unlock();
        }
    }
}

// Password entry field value changed (masked display)
static void libre_password_entry_field_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *display_label = (lv_obj_t *)lv_event_get_user_data(e);
        const char *text = lv_textarea_get_text(libre_password_entry_ta);
        int len = strlen(text);
        if (len > 0) {
            if (libre_pwd_visible) {
                lv_label_set_text(display_label, text);
            } else {
                char masked[65];
                for (int i = 0; i < len && i < 64; i++) masked[i] = '*';
                masked[len < 64 ? len : 64] = '\0';
                lv_label_set_text(display_label, masked);
            }
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        } else {
            lv_label_set_text(display_label, "Password");
            lv_obj_set_style_text_color(display_label, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }
}

// Logout
static void libre_logout_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Libre logout requested");
        libre_logout();
        nvs_clear_libre_credentials();

        // Clear UI buffers
        memset(libre_email_buf, 0, sizeof(libre_email_buf));
        memset(libre_password_buf, 0, sizeof(libre_password_buf));

        if (lvgl_port_lock(0)) {
            create_libre_auth_screen();
            lv_scr_load(screen_libre_auth);
            if (screen_libre_status != NULL) { lv_obj_del(screen_libre_status); screen_libre_status = NULL; }
            lvgl_port_unlock();
        }
    }
}

// ==================== Libre Auth Screen ====================

void create_libre_auth_screen(void) {
    screen_libre_auth = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_auth, &style_bg, 0);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_libre_auth);
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
    lv_obj_add_event_cb(back_btn, libre_back_event_cb, LV_EVENT_CLICKED, NULL);

    bool authenticated = libre_is_authenticated();

    if (authenticated) {
        // Title
        lv_obj_t *title = lv_label_create(screen_libre_auth);
        lv_label_set_text(title, "LibreLinkUp");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        // Green hero circle
        lv_obj_t *hero = lv_obj_create(screen_libre_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 52, 52);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_border_width(hero, 2, 0);
        lv_obj_set_style_border_opa(hero, LV_OPA_COVER, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_center(hero_icon);

        // "Connected" status
        lv_obj_t *connected = lv_label_create(screen_libre_auth);
        lv_label_set_text(connected, "Connected");
        lv_obj_set_style_text_font(connected, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(connected, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_align(connected, LV_ALIGN_TOP_MID, 0, 112);

        // Email
        lv_obj_t *email_lbl = lv_label_create(screen_libre_auth);
        lv_label_set_text(email_lbl, libre_email_buf);
        lv_obj_set_style_text_font(email_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(email_lbl, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(email_lbl, LV_ALIGN_TOP_MID, 0, 134);

        // Account Details card-button
        lv_obj_t *view_btn = lv_btn_create(screen_libre_auth);
        lv_obj_set_size(view_btn, 290, 44);
        lv_obj_align(view_btn, LV_ALIGN_TOP_MID, 0, 164);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(COLOR_CARD_BG), 0);
        lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(view_btn, 10, 0);
        lv_obj_set_style_border_width(view_btn, 0, 0);
        lv_obj_set_style_shadow_width(view_btn, 0, 0);
        lv_obj_set_style_pad_left(view_btn, 14, 0);
        lv_obj_set_style_bg_color(view_btn, lv_color_hex(0x2a3a4a), LV_STATE_PRESSED);

        lv_obj_t *view_icon = lv_label_create(view_btn);
        lv_label_set_text(view_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(view_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_align(view_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *view_label = lv_label_create(view_btn);
        lv_label_set_text(view_label, "Account Details");
        lv_obj_set_style_text_font(view_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(view_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(view_label, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *view_arrow = lv_label_create(view_btn);
        lv_label_set_text(view_arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(view_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(view_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_add_event_cb(view_btn, libre_view_status_btn_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        // Hero circle
        lv_obj_t *hero = lv_obj_create(screen_libre_auth);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, 56, 56);
        lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_set_style_bg_color(hero, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_20, 0);
        lv_obj_set_style_radius(hero, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *hero_icon = lv_label_create(hero);
        lv_label_set_text(hero_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(hero_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(hero_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_center(hero_icon);

        // Brand title
        lv_obj_t *title = lv_label_create(screen_libre_auth);
        lv_label_set_text(title, "LibreLinkUp");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 106);

        // Subtitle
        lv_obj_t *subtitle = lv_label_create(screen_libre_auth);
        lv_label_set_text(subtitle, "Connect your FreeStyle Libre\naccount for glucose readings");
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_GRAY), 0);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 130);

        // Sign In button
        lv_obj_t *login_btn = lv_btn_create(screen_libre_auth);
        lv_obj_set_size(login_btn, 290, 46);
        lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 174);
        lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
        lv_obj_set_style_radius(login_btn, 10, 0);
        lv_obj_set_style_shadow_width(login_btn, 0, 0);
        lv_obj_set_style_border_width(login_btn, 0, 0);

        lv_obj_t *login_label = lv_label_create(login_btn);
        lv_label_set_text(login_label, "Sign In");
        lv_obj_set_style_text_font(login_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(login_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_center(login_label);
        lv_obj_add_event_cb(login_btn, libre_open_login_event_cb, LV_EVENT_CLICKED, NULL);

        // Footer note
        lv_obj_t *note = lv_label_create(screen_libre_auth);
        lv_label_set_text(note, "Requires LibreLinkUp follower account");
        lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

// ==================== Libre Login Screen ====================

void create_libre_login_screen(void) {
    screen_libre_login = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_login, &style_bg, 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_libre_login);
    lv_label_set_text(title, "Sign In");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_libre_login);
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
    lv_obj_add_event_cb(back_btn, libre_to_auth_event_cb, LV_EVENT_CLICKED, NULL);

    // Unified form card
    lv_obj_t *form_card = lv_obj_create(screen_libre_login);
    lv_obj_set_size(form_card, 290, 88);
    lv_obj_align(form_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(form_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(form_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(form_card, 10, 0);
    lv_obj_set_style_border_width(form_card, 0, 0);
    lv_obj_set_style_pad_all(form_card, 0, 0);
    lv_obj_clear_flag(form_card, LV_OBJ_FLAG_SCROLLABLE);

    // Email row (top half)
    lv_obj_t *email_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(email_row);
    lv_obj_set_size(email_row, 290, 43);
    lv_obj_align(email_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(email_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(email_row, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(email_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *email_icon = lv_label_create(email_row);
    lv_label_set_text(email_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(email_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(email_icon, LV_ALIGN_LEFT_MID, 14, 0);

    libre_email_label = lv_label_create(email_row);
    lv_obj_set_style_text_font(libre_email_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(libre_email_label, 195);
    lv_label_set_long_mode(libre_email_label, LV_LABEL_LONG_DOT);
    if (strlen(libre_email_buf) > 0) {
        lv_label_set_text(libre_email_label, libre_email_buf);
        lv_obj_set_style_text_color(libre_email_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_email_label, "Email");
        lv_obj_set_style_text_color(libre_email_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_email_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *email_arrow = lv_label_create(email_row);
    lv_label_set_text(email_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(email_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(email_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(email_row, libre_email_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Divider
    lv_obj_t *div = lv_obj_create(form_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 258, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Password row (bottom half)
    lv_obj_t *pass_row = lv_obj_create(form_card);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_size(pass_row, 290, 44);
    lv_obj_align(pass_row, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(pass_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pass_row, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pass_row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *pass_icon = lv_label_create(pass_row);
    lv_label_set_text(pass_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(pass_icon, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_align(pass_icon, LV_ALIGN_LEFT_MID, 14, 0);

    libre_password_label = lv_label_create(pass_row);
    lv_obj_set_style_text_font(libre_password_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(libre_password_label, 195);
    lv_label_set_long_mode(libre_password_label, LV_LABEL_LONG_DOT);
    if (strlen(libre_password_buf) > 0) {
        int len = strlen(libre_password_buf);
        char display[65];
        for (int i = 0; i < len; i++) display[i] = '*';
        display[len] = '\0';
        lv_label_set_text(libre_password_label, display);
        lv_obj_set_style_text_color(libre_password_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_password_label, "Password");
        lv_obj_set_style_text_color(libre_password_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_password_label, LV_ALIGN_LEFT_MID, 38, 0);

    lv_obj_t *pass_arrow = lv_label_create(pass_row);
    lv_label_set_text(pass_arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(pass_arrow, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(pass_arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(pass_row, libre_password_tap_event_cb, LV_EVENT_CLICKED, NULL);

    // Sign In button
    lv_obj_t *login_btn = lv_btn_create(screen_libre_login);
    lv_obj_set_size(login_btn, 290, 48);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(login_btn, 10, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);

    lv_obj_t *login_icon = lv_label_create(login_btn);
    lv_label_set_text(login_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(login_icon, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(login_icon, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *login_lbl = lv_label_create(login_btn);
    lv_label_set_text(login_lbl, "Sign In");
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(login_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, libre_login_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Hint text
    lv_obj_t *hint = lv_label_create(screen_libre_login);
    lv_label_set_text(hint, "Tap a field to enter credentials");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ==================== Libre Email Entry Screen ====================

void create_libre_email_entry_screen(void) {
    screen_libre_email_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_email_entry, &style_bg, 0);

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_libre_email_entry);
    lv_label_set_text(field_title, "EMAIL");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(screen_libre_email_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 19);
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
    lv_obj_add_event_cb(cancel_btn, libre_email_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_libre_email_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 19);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, libre_email_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    libre_email_entry_label = lv_label_create(screen_libre_email_entry);
    lv_obj_set_style_text_font(libre_email_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(libre_email_entry_label, 210);
    lv_label_set_long_mode(libre_email_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(libre_email_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(libre_email_buf) > 0) {
        lv_label_set_text(libre_email_entry_label, libre_email_buf);
        lv_obj_set_style_text_color(libre_email_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_email_entry_label, "Email");
        lv_obj_set_style_text_color(libre_email_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_email_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_libre_email_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    libre_email_entry_ta = lv_textarea_create(screen_libre_email_entry);
    lv_obj_set_size(libre_email_entry_ta, 1, 1);
    lv_obj_set_pos(libre_email_entry_ta, -100, -100);
    lv_obj_add_flag(libre_email_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(libre_email_entry_ta, true);
    lv_textarea_set_max_length(libre_email_entry_ta, 63);
    lv_textarea_set_text(libre_email_entry_ta, libre_email_buf);

    // Keyboard
    libre_email_entry_keyboard = lv_keyboard_create(screen_libre_email_entry);
    lv_obj_set_size(libre_email_entry_keyboard, 320, 175);
    lv_obj_align(libre_email_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(libre_email_entry_keyboard, libre_email_entry_ta);
    lv_keyboard_set_mode(libre_email_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(libre_email_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(libre_email_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(libre_email_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(libre_email_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(libre_email_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(libre_email_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(libre_email_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(libre_email_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(libre_email_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(libre_email_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(libre_email_entry_ta, libre_email_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, libre_email_entry_label);
}

// ==================== Libre Password Entry Screen ====================

void create_libre_password_entry_screen(void) {
    screen_libre_password_entry = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_password_entry, &style_bg, 0);

    libre_pwd_visible = false;

    // Section header
    lv_obj_t *field_title = lv_label_create(screen_libre_password_entry);
    lv_label_set_text(field_title, "PASSWORD");
    lv_obj_set_style_text_font(field_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(field_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(field_title, 2, 0);
    lv_obj_align(field_title, LV_ALIGN_TOP_MID, 0, 5);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(screen_libre_password_entry);
    lv_obj_set_size(cancel_btn, 36, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 19);
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
    lv_obj_add_event_cb(cancel_btn, libre_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_CANCEL);

    // OK button
    lv_obj_t *ok_btn = lv_btn_create(screen_libre_password_entry);
    lv_obj_set_size(ok_btn, 36, 36);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -5, 19);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok_btn, libre_password_entry_keyboard_event_cb, LV_EVENT_CLICKED, (void*)LV_EVENT_READY);

    // Display label
    libre_password_entry_label = lv_label_create(screen_libre_password_entry);
    lv_obj_set_style_text_font(libre_password_entry_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(libre_password_entry_label, 210);
    lv_label_set_long_mode(libre_password_entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(libre_password_entry_label, LV_TEXT_ALIGN_CENTER, 0);
    if (strlen(libre_password_buf) > 0) {
        int len = strlen(libre_password_buf);
        char masked[65];
        for (int i = 0; i < len && i < 64; i++) masked[i] = '*';
        masked[len < 64 ? len : 64] = '\0';
        lv_label_set_text(libre_password_entry_label, masked);
        lv_obj_set_style_text_color(libre_password_entry_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    } else {
        lv_label_set_text(libre_password_entry_label, "Password");
        lv_obj_set_style_text_color(libre_password_entry_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    }
    lv_obj_align(libre_password_entry_label, LV_ALIGN_TOP_MID, 0, 30);

    // Accent underline
    lv_obj_t *accent = lv_obj_create(screen_libre_password_entry);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 200, 2);
    lv_obj_align(accent, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 1, 0);

    // Hidden textarea
    libre_password_entry_ta = lv_textarea_create(screen_libre_password_entry);
    lv_obj_set_size(libre_password_entry_ta, 1, 1);
    lv_obj_set_pos(libre_password_entry_ta, -100, -100);
    lv_obj_add_flag(libre_password_entry_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(libre_password_entry_ta, true);
    lv_textarea_set_max_length(libre_password_entry_ta, 63);
    lv_textarea_set_text(libre_password_entry_ta, libre_password_buf);

    // Keyboard
    libre_password_entry_keyboard = lv_keyboard_create(screen_libre_password_entry);
    lv_obj_set_size(libre_password_entry_keyboard, 320, 175);
    lv_obj_align(libre_password_entry_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(libre_password_entry_keyboard, libre_password_entry_ta);
    lv_keyboard_set_mode(libre_password_entry_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Glass keyboard styling
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(0x0F1419), 0);
    lv_obj_set_style_bg_opa(libre_password_entry_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(libre_password_entry_keyboard, 0, 0);
    lv_obj_set_style_pad_all(libre_password_entry_keyboard, 2, 0);
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(COLOR_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(libre_password_entry_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(libre_password_entry_keyboard, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(libre_password_entry_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_width(libre_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(libre_password_entry_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(libre_password_entry_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(libre_password_entry_keyboard, lv_color_hex(COLOR_ACCENT_BLUE),
                               LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(libre_password_entry_ta, libre_password_entry_field_event_cb, LV_EVENT_VALUE_CHANGED, libre_password_entry_label);
}

// ==================== Libre Status Screen ====================

void create_libre_status_screen(void) {
    screen_libre_status = lv_obj_create(NULL);
    lv_obj_add_style(screen_libre_status, &style_bg, 0);

    // Title
    lv_obj_t *title = lv_label_create(screen_libre_status);
    lv_label_set_text(title, "Account");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Ghost back button
    lv_obj_t *back_btn = lv_btn_create(screen_libre_status);
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
    lv_obj_add_event_cb(back_btn, libre_to_auth_event_cb, LV_EVENT_CLICKED, NULL);

    // Profile card
    lv_obj_t *profile_card = lv_obj_create(screen_libre_status);
    lv_obj_set_size(profile_card, 290, 60);
    lv_obj_align(profile_card, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(profile_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(profile_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(profile_card, 10, 0);
    lv_obj_set_style_border_width(profile_card, 0, 0);
    lv_obj_clear_flag(profile_card, LV_OBJ_FLAG_SCROLLABLE);

    // Green accent bar
    lv_obj_t *accent_bar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(accent_bar);
    lv_obj_set_size(accent_bar, 3, 40);
    lv_obj_align(accent_bar, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(accent_bar, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(accent_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent_bar, 2, 0);

    // Avatar circle
    lv_obj_t *avatar = lv_obj_create(profile_card);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);

    char initial[2] = {0};
    if (strlen(libre_email_buf) > 0) {
        initial[0] = libre_email_buf[0];
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    } else {
        initial[0] = 'L';
    }
    lv_obj_t *initial_label = lv_label_create(avatar);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(initial_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_center(initial_label);

    // Email
    lv_obj_t *name_label = lv_label_create(profile_card);
    lv_label_set_text(name_label, libre_email_buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 210);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 62, -8);

    // Connected status
    lv_obj_t *status_label = lv_label_create(profile_card);
    lv_label_set_text(status_label, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 62, 10);

    // Info card (CGM Type + Account)
    lv_obj_t *info_card = lv_obj_create(screen_libre_status);
    lv_obj_set_size(info_card, 290, 80);
    lv_obj_align(info_card, LV_ALIGN_TOP_MID, 0, 114);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0);
    lv_obj_set_style_pad_all(info_card, 0, 0);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cgm_title = lv_label_create(info_card);
    lv_label_set_text(cgm_title, "CGM TYPE");
    lv_obj_set_style_text_font(cgm_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(cgm_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(cgm_title, 2, 0);
    lv_obj_align(cgm_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *cgm_value = lv_label_create(info_card);
    lv_label_set_text(cgm_value, "LibreLinkUp");
    lv_obj_set_style_text_font(cgm_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cgm_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_align(cgm_value, LV_ALIGN_TOP_LEFT, 14, 20);

    // Divider
    lv_obj_t *div = lv_obj_create(info_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 262, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x263040), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    lv_obj_t *acct_title = lv_label_create(info_card);
    lv_label_set_text(acct_title, "ACCOUNT");
    lv_obj_set_style_text_font(acct_title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(acct_title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(acct_title, 2, 0);
    lv_obj_align(acct_title, LV_ALIGN_TOP_LEFT, 14, 46);

    lv_obj_t *acct_value = lv_label_create(info_card);
    lv_label_set_text(acct_value, libre_email_buf);
    lv_obj_set_style_text_font(acct_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(acct_value, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_long_mode(acct_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(acct_value, 260);
    lv_obj_align(acct_value, LV_ALIGN_TOP_LEFT, 14, 60);

    // Ghost red logout button
    lv_obj_t *logout_btn = lv_btn_create(screen_libre_status);
    lv_obj_set_size(logout_btn, 180, 38);
    lv_obj_align(logout_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(logout_btn, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_border_width(logout_btn, 1, 0);
    lv_obj_set_style_border_opa(logout_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logout_btn, 10, 0);
    lv_obj_set_style_shadow_width(logout_btn, 0, 0);
    lv_obj_set_style_bg_color(logout_btn, lv_color_hex(COLOR_RED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(logout_btn, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t *logout_lbl = lv_label_create(logout_btn);
    lv_label_set_text(logout_lbl, "Sign Out");
    lv_obj_set_style_text_font(logout_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logout_lbl, lv_color_hex(COLOR_RED), 0);
    lv_obj_center(logout_lbl);
    lv_obj_add_event_cb(logout_btn, libre_logout_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

