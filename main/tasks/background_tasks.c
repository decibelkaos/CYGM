/*
 * background_tasks.c
 *
 * FreeRTOS background tasks implementation
 */

#include "background_tasks.h"
#include "shared_state.h"
#include "main.h"
#include "hardware/display.h"
#include "hardware/battery.h"
#include "hardware/led.h"
#include "hardware/buzzer.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "ui/home_screen.h"
#include "ui/menu_screen.h"
#include "ui/wifi_screens.h"
#include "wifi_manager.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "cgm_types.h"
#include "sd_logger.h"
#include "features/heartbeat.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "BACKGROUND_TASKS";
#define UI_INACTIVITY_RETURN_MS 30000

void ui_task_core1(void *pvParameters) {
    ESP_LOGI(TAG, "UI Task started on Core 1");

    // Initialize touch on Core 1
    add_boot_log("Init touch screen");
    ESP_ERROR_CHECK(touch_init_core1());

    // Initialize LVGL
    add_boot_log("Init LVGL");
    ESP_ERROR_CHECK(lvgl_init());

    // Initialize LVGL styles
    init_styles();

    // Create UI screens
    add_boot_log("Create UI screens");
    create_splash_screen();
    create_home_screen();
    // MEMORY OPTIMIZATION: Don't create menu/WiFi screens at boot - create on-demand to save RAM
    // create_menu_screen();      // Created when user presses settings button
    // create_wifi_list_screen();  // Created when user opens WiFi settings
    // create_wifi_password_screen();  // Created when needed

    // Show splash screen with all buffered boot messages
    lv_scr_load(screen_splash);
    add_boot_log("Display ready");

    // Update boot log with any buffered messages
    if (boot_log_label != NULL && lvgl_port_lock(0)) {
        lv_label_set_text(boot_log_label, boot_log_buffer);
        lvgl_port_unlock();
    }

    // Show splash screen for 5 seconds
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Update progress bar to 100% before transition
    add_boot_log("Loading home...");

    // Small delay to show 100% completion
    vTaskDelay(pdMS_TO_TICKS(300));

    // Switch to home screen with smooth fade animation
    if (lvgl_port_lock(0)) {
        lv_scr_load_anim(screen_home, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
        lvgl_port_unlock();
    }

    // Wait for animation to complete
    vTaskDelay(pdMS_TO_TICKS(500));

    // Mark home screen as active - allow background tasks to run
    home_screen_active = true;
    pause_background_tasks = false;
    ESP_LOGI(TAG, "Home screen loaded - background tasks enabled");

    // Show legal disclaimer overlay on boot (unless user has accepted permanently)
    if (!nvs_get_disclaimer_accepted()) {
        if (lvgl_port_lock(100)) {
            show_disclaimer_overlay();
            lvgl_port_unlock();
        }
    }

    // Stop WiFi boot blink when home screen loads
    led_stop_wifi_boot_blink();

    // Start battery monitoring task NOW (after LVGL is fully initialized)
    ESP_LOGI(TAG, "Starting battery monitor task");
    xTaskCreatePinnedToCore(battery_monitor_task, "battery_mon",
        BATTERY_STACK_SIZE, NULL, 2, &battery_task_handle, 0);

    // UI task complete - time updates handled by time_update_task()
    // Keep a global inactivity watchdog for ALL non-home screens
    while (1) {
        if (!home_screen_active) {
            uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
            if (inactive_ms >= UI_INACTIVITY_RETURN_MS) {
                ESP_LOGI(TAG, "UI inactivity timeout (%lu ms) - returning to home", (unsigned long)inactive_ms);

                if (lvgl_port_lock(100)) {
                    lv_scr_load(screen_home);
                    lvgl_port_unlock();
                }

                home_screen_active = true;
                pause_background_tasks = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void wifi_task_core0(void *pvParameters) {
    ESP_LOGI(TAG, "WiFi Task started on Core 0");

    // Initialize WiFi manager
    add_boot_log("Init WiFi");
    ESP_ERROR_CHECK(wifi_manager_init());

    // Check for saved credentials
    uint8_t saved_count = nvs_get_saved_wifi_count();

    if (saved_count == 0) {
        ESP_LOGI(TAG, "No saved WiFi credentials - setup required");
        add_boot_log("WiFi: Setup needed");
        led_set_breathing_color(127, 127, 0);  // Yellow = setup needed

        // Update WiFi status icon to show not connected (only if LVGL is initialized)
        if (label_wifi_status != NULL && lvgl_port_lock(0)) {
            lv_label_set_recolor(label_wifi_status, true);
            lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
            lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
            lvgl_port_unlock();
        }
    } else {
        // Smart WiFi connection: Try last connected network first, then fall back to others
        ESP_LOGI(TAG, "Found %d saved WiFi network(s)", saved_count);

        // Load all saved networks
        wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
        uint8_t network_count = 0;
        nvs_get_saved_wifi_networks(networks, &network_count);

        // Try to get last connected WiFi
        char last_ssid[MAX_SSID_LEN] = {0};
        bool has_last_wifi = (nvs_load_last_wifi_ssid(last_ssid, sizeof(last_ssid)) == ESP_OK);

        // Build connection order: last connected first, then others
        int connection_order[MAX_SAVED_WIFI_NETWORKS];
        int order_count = 0;

        if (has_last_wifi) {
            // Find last connected network in saved list
            for (int i = 0; i < network_count; i++) {
                if (strcmp(networks[i].ssid, last_ssid) == 0) {
                    connection_order[order_count++] = i;
                    ESP_LOGI(TAG, "Last connected WiFi found: %s (will try first)", last_ssid);
                    break;
                }
            }
        }

        // Add remaining networks to connection order
        for (int i = 0; i < network_count; i++) {
            bool already_added = false;
            for (int j = 0; j < order_count; j++) {
                if (connection_order[j] == i) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added) {
                connection_order[order_count++] = i;
            }
        }

        // Try connecting to networks in order
        bool connected = false;
        for (int attempt = 0; attempt < order_count; attempt++) {
            int idx = connection_order[attempt];
            ESP_LOGI(TAG, "Trying WiFi network %d/%d: %s", attempt + 1, order_count, networks[idx].ssid);
            add_boot_log("Connecting WiFi...");
            led_set_breathing_color(127, 127, 0);  // Yellow = connecting

            if (wifi_manager_connect_to(networks[idx].ssid, networks[idx].password) == ESP_OK) {
                // Get the ACTUAL connected SSID from WiFi stack (might differ from attempted SSID due to fallback)
                wifi_config_t wifi_config;
                if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
                    char actual_ssid[33] = {0};
                    memcpy(actual_ssid, wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));
                    actual_ssid[32] = '\0';  // Ensure null termination
                    ESP_LOGI(TAG, "Successfully connected to: %s", actual_ssid);
                    nvs_save_last_wifi_ssid(actual_ssid);  // Save as last successful connection
                } else {
                    ESP_LOGI(TAG, "Successfully connected to: %s", networks[idx].ssid);
                    nvs_save_last_wifi_ssid(networks[idx].ssid);  // Fallback to attempted SSID
                }
                connected = true;
                break;
            } else {
                ESP_LOGW(TAG, "Failed to connect to: %s", networks[idx].ssid);
                if (attempt < order_count - 1) {
                    ESP_LOGI(TAG, "Trying next saved network...");
                    vTaskDelay(pdMS_TO_TICKS(2000));  // Brief delay before next attempt
                }
            }
        }

        if (connected) {
            ESP_LOGI(TAG, "WiFi connected successfully");
            sd_log(TAG, "WiFi connected");
            add_boot_log("WiFi connected!");
            wifi_connected = true;
            led_start_wifi_boot_blink();  // Start green blink during boot

            // Update WiFi status icon - just green icon, no text (only if LVGL is initialized)
            if (label_wifi_status != NULL && lvgl_port_lock(0)) {
                lv_label_set_recolor(label_wifi_status, false);  // Disable recolor
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
                lvgl_port_unlock();
            }

            // SD card logger will be initialized after time sync (needs correct timestamp for filename)

            // Initialize SNTP for time sync
            initialize_sntp();

            // Start time update task (singleton guard)
            if (time_task_handle == NULL) {
                xTaskCreatePinnedToCore(time_update_task, "time_update",
                    TIME_STACK_SIZE, NULL, 1, &time_task_handle, tskNO_AFFINITY);
            } else {
                ESP_LOGI(TAG, "time_update task already running");
            }

            // Delay weather task until after CGM connects (SSL memory priority)
            // Will be started after CGM auth completes

            // Determine active CGM provider
            char cgm_type[MAX_CGM_TYPE_LEN] = "dexcom";
            nvs_load_cgm_type(cgm_type, sizeof(cgm_type));
            bool boot_use_libre = (strcmp(cgm_type, "libre") == 0);

            // Check for saved credentials (dispatch by provider)
            bool has_cgm_creds = boot_use_libre ? nvs_has_libre_credentials() : nvs_has_dexcom_credentials();

            if (has_cgm_creds) {
                ESP_LOGI(TAG, "Found saved %s credentials, will attempt auto-login after time sync",
                         boot_use_libre ? "Libre" : "Dexcom");

                // Load credentials into UI buffers
                if (boot_use_libre) {
                    nvs_get_libre_credentials(libre_email_buf, sizeof(libre_email_buf),
                                               libre_password_buf, sizeof(libre_password_buf));
                } else {
                    char username[64], password[64];
                    if (nvs_get_dexcom_credentials(username, sizeof(username), password, sizeof(password)) == ESP_OK) {
                        strncpy(dexcom_username_buf, username, sizeof(dexcom_username_buf) - 1);
                        strncpy(dexcom_password_buf, password, sizeof(dexcom_password_buf) - 1);
                    }
                }

                // CRITICAL: Wait for SNTP time sync (SSL certificates require accurate time!)
                ESP_LOGI(TAG, "Waiting for SNTP time sync before %s auth (required for SSL)...",
                         boot_use_libre ? "Libre" : "Dexcom");

                time_t now;
                struct tm timeinfo;
                time(&now);
                localtime_r(&now, &timeinfo);
                ESP_LOGI(TAG, "Current system time: %04d-%02d-%02d %02d:%02d:%02d",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

                if (!wait_for_time_sync(60000)) {
                    sd_log(TAG, "Time sync failed after 60s - cannot auth %s", cgm_type);
                    ESP_LOGE(TAG, "Time sync failed after 60s - CANNOT attempt %s auth (SSL will fail)", cgm_type);

                    if (glucose_task_handle == NULL) {
                        ESP_LOGI(TAG, "Starting glucose task (will auto-retry after time syncs)...");
                        xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 5, &glucose_task_handle);
                    }
                    if (weather_task_handle == NULL) {
                        ESP_LOGI(TAG, "Starting weather task (time sync failed but WiFi up)...");
                        xTaskCreatePinnedToCore(weather_update_task, "weather_update",
                            WEATHER_STACK_SIZE, NULL, 3, &weather_task_handle, 0);
                    }
                } else {
                    ESP_LOGI(TAG, "Time synced successfully, proceeding with %s auto-login", cgm_type);

                    int max_retries = 5;
                    int retry_delay_ms = 2000;

                    // Libre: try restoring saved session before full login
                    bool session_restored = false;
                    if (boot_use_libre) {
                        if (libre_restore_session() == ESP_OK) {
                            ESP_LOGI(TAG, "Libre session restored from NVS — skipping login");
                            sd_log(TAG, "Libre session restored (skip login)");
                            session_restored = true;

                            // Fetch initial glucose with restored session
                            cgm_glucose_t glucose = {0};
                            esp_err_t fetch_ret = libre_fetch_glucose(&glucose);

                            if (fetch_ret == ESP_OK) {
                                last_glucose_fetch_time = esp_timer_get_time() / 1000;
                            }

                            if (fetch_ret == ESP_OK && glucose.valid) {
                                current_glucose = glucose.value;
                                current_trend = glucose.trend;
                                glucose_timestamp = glucose.timestamp;
                                glucose_data_valid = true;
                                glucose_status = glucose.status;

                                sd_log(TAG, "Initial glucose: %d mg/dL, trend: %s",
                                       glucose.value, cgm_trend_description(glucose.trend));

                                if (!first_glucose_received) {
                                    first_glucose_received = true;
                                    play_success_sound();
                                }

                                check_glucose_alarms(current_glucose);
                                update_glucose_display();
                            } else {
                                glucose_data_valid = false;
                                glucose_status = glucose.status;
                                update_glucose_display();
                            }
                        } else {
                            ESP_LOGI(TAG, "Libre session restore failed — will do full login");
                        }
                    }

                    for (int attempt = 1; attempt <= max_retries && !session_restored; attempt++) {
                        ESP_LOGI(TAG, "%s auth attempt %d/%d...", cgm_type, attempt, max_retries);

                        esp_err_t ret;
                        if (boot_use_libre) {
                            ret = libre_authenticate(libre_email_buf, libre_password_buf);
                        } else {
                            ret = dexcom_authenticate(dexcom_username_buf, dexcom_password_buf);
                        }

                        if (ret == ESP_OK) {
                            sd_log(TAG, "%s auto-login OK (attempt %d)", cgm_type, attempt);
                            ESP_LOGI(TAG, "%s auto-login successful on attempt %d!", cgm_type, attempt);

                            // Fetch initial glucose reading
                            cgm_glucose_t glucose = {0};
                            esp_err_t fetch_ret;
                            if (boot_use_libre) {
                                fetch_ret = libre_fetch_glucose(&glucose);
                            } else {
                                fetch_ret = dexcom_fetch_glucose(&glucose);
                            }

                            if (fetch_ret == ESP_OK) {
                                last_glucose_fetch_time = esp_timer_get_time() / 1000;
                            }

                            if (fetch_ret == ESP_OK && glucose.valid) {
                                current_glucose = glucose.value;
                                current_trend = glucose.trend;
                                glucose_timestamp = glucose.timestamp;
                                glucose_data_valid = true;
                                glucose_status = glucose.status;

                                sd_log(TAG, "Initial glucose: %d mg/dL, trend: %s",
                                       glucose.value, cgm_trend_description(glucose.trend));
                                ESP_LOGI(TAG, "Initial glucose: %d mg/dL, trend: %s",
                                         glucose.value, cgm_trend_description(glucose.trend));

                                if (!first_glucose_received) {
                                    first_glucose_received = true;
                                    ESP_LOGI(TAG, "First glucose reading received - playing success sound");
                                    play_success_sound();
                                }

                                check_glucose_alarms(current_glucose);
                                update_glucose_display();
                            } else {
                                glucose_data_valid = false;
                                glucose_status = glucose.status;
                                update_glucose_display();
                            }

                            ESP_LOGI(TAG, "===== MEMORY CHECKPOINT: After initial %s fetch =====", cgm_type);
                            ESP_LOGI(TAG, "Free heap: %lu bytes (SSL alive)", esp_get_free_heap_size());
                            ESP_LOGI(TAG, "Minimum free heap ever: %lu bytes", esp_get_minimum_free_heap_size());

                            sd_logger_flush();

                            // Send "device online" heartbeat (HTTP, ~500ms)
                            // network_mutex not needed here — no other tasks running yet
                            heartbeat_send();

                            if (glucose_task_handle == NULL) {
                                ESP_LOGI(TAG, "Starting glucose update task (90 second interval)...");
                                xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 5, &glucose_task_handle);
                            }
                            if (weather_task_handle == NULL) {
                                ESP_LOGI(TAG, "Starting weather task (after %s, HTTP mode)...", cgm_type);
                                xTaskCreatePinnedToCore(weather_update_task, "weather_update",
                                    WEATHER_STACK_SIZE, NULL, 3, &weather_task_handle, 0);
                            }

                            break;  // Success - exit retry loop
                        } else {
                            sd_log(TAG, "%s auth attempt %d failed: %s", cgm_type, attempt, esp_err_to_name(ret));
                            ESP_LOGW(TAG, "%s auth attempt %d failed: %s", cgm_type, attempt, esp_err_to_name(ret));

                            if (attempt < max_retries) {
                                ESP_LOGI(TAG, "Retrying in %d ms...", retry_delay_ms);
                                vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                                retry_delay_ms *= 2;
                            } else {
                                sd_log(TAG, "CRITICAL: %s login failed after %d attempts", cgm_type, max_retries);
                                ESP_LOGE(TAG, "%s auto-login failed after %d attempts", cgm_type, max_retries);
                                ESP_LOGE(TAG, "CRITICAL: Starting tasks anyway - glucose task will auto-retry");
                            }
                        }
                    }

                    // Ensure glucose and weather tasks start even if auth failed
                    if (glucose_task_handle == NULL) {
                        ESP_LOGI(TAG, "Starting glucose task (will auto-reconnect to %s)...", cgm_type);
                        xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 5, &glucose_task_handle);
                    }
                    if (weather_task_handle == NULL) {
                        ESP_LOGI(TAG, "Starting weather task (auth failed but WiFi connected)...");
                        xTaskCreatePinnedToCore(weather_update_task, "weather_update",
                            WEATHER_STACK_SIZE, NULL, 3, &weather_task_handle, 0);
                    }
                }
            } else {
                // No CGM credentials - start weather task
                ESP_LOGI(TAG, "No %s credentials found, starting weather task (HTTP mode)...", cgm_type);
                if (weather_task_handle == NULL) {
                    xTaskCreatePinnedToCore(weather_update_task, "weather_update",
                        WEATHER_STACK_SIZE, NULL, 3, &weather_task_handle, 0);
                }
            }
        } else {
            sd_log(TAG, "WiFi connection failed");
            ESP_LOGE(TAG, "WiFi connection failed");
            led_start_error_blink();  // Red blink for error

            // Update WiFi status on home screen - gray WiFi icon with RED warning (only if LVGL is initialized)
            if (label_wifi_status != NULL && lvgl_port_lock(0)) {
                lv_label_set_recolor(label_wifi_status, true);
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
                lvgl_port_unlock();
            }
        }
    }

    // Monitor WiFi connection and auto-reconnect.
    // SAFETY: WiFi reconnection is NEVER paused regardless of active screen.
    // Without WiFi there is no glucose data, which is life-critical.
    bool was_connected = wifi_connected;
    bool wifi_overlay_shown = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        if (!wifi_connected) {
            // Update home screen icon if we just lost connection
            if (was_connected) {
                sd_log(TAG, "WiFi connection lost - auto-reconnecting");
                ESP_LOGW(TAG, "WiFi connection lost - will attempt auto-reconnect");
                update_wifi_status_display(false);
                was_connected = false;
            }

            ESP_LOGI(TAG, "Auto-reconnect: scanning for known networks...");
            esp_err_t ret = wifi_manager_try_reconnect();

            if (ret == ESP_OK) {
                wifi_connected = true;
                was_connected = true;
                sd_log(TAG, "WiFi auto-reconnect successful");
                ESP_LOGI(TAG, "Auto-reconnect successful");
                update_wifi_status_display(true);

                // Dismiss WiFi overlay if it was shown
                if (wifi_overlay_shown) {
                    if (lvgl_port_lock(200)) {
                        dismiss_wifi_disconnected_overlay();
                        lvgl_port_unlock();
                    }
                    wifi_overlay_shown = false;
                }

                // Re-initialize SNTP in case time drifted
                initialize_sntp();
            } else if (!wifi_overlay_shown && home_screen_active) {
                // First reconnect attempt failed — show WiFi overlay
                if (lvgl_port_lock(200)) {
                    show_wifi_disconnected_overlay();
                    lvgl_port_unlock();
                }
                wifi_overlay_shown = true;
            }
        } else {
            if (!was_connected) {
                // Connection restored externally (e.g. event handler retry succeeded)
                was_connected = true;
                update_wifi_status_display(true);

                // Dismiss WiFi overlay if it was shown
                if (wifi_overlay_shown) {
                    if (lvgl_port_lock(200)) {
                        dismiss_wifi_disconnected_overlay();
                        lvgl_port_unlock();
                    }
                    wifi_overlay_shown = false;
                }
            }

            // Reset overlay flag when connected (so it can show again on next disconnect)
            wifi_overlay_shown = false;
        }
    }
}
