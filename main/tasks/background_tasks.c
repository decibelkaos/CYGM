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
#include "web_server.h"
#include "sd_logger.h"
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

    // Stop WiFi boot blink when home screen loads
    led_stop_wifi_boot_blink();

    // Start battery monitoring task NOW (after LVGL is fully initialized)
    ESP_LOGI(TAG, "Starting battery monitor task");
    xTaskCreatePinnedToCore(
        battery_monitor_task,
        "battery_mon",
        3072,
        NULL,
        2,  // Low priority
        NULL,
        0   // Core 0
    );

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

#if ENABLE_WEB_SERVER
            // Start web server (DISABLED by default to save ~30KB heap for SSL)
            ESP_LOGI(TAG, "Starting web server...");
            if (start_webserver() == ESP_OK) {
                ESP_LOGI(TAG, "Web server started successfully");
                add_boot_log("Web server started");
            } else {
                ESP_LOGE(TAG, "Failed to start web server");
            }
#endif

            // Initialize SNTP for time sync
            initialize_sntp();

            // Start time update task (singleton guard)
            if (time_task_handle == NULL) {
                xTaskCreate(time_update_task, "time_update", 4096, NULL, 1, &time_task_handle);
            } else {
                ESP_LOGI(TAG, "time_update task already running");
            }

            // Delay weather task until after Dexcom connects (SSL memory priority)
            // Will be started after Dexcom auth completes

            // Auto-login to Dexcom if credentials saved (CRITICAL: Wait for time sync first!)
            if (nvs_has_dexcom_credentials()) {
                ESP_LOGI(TAG, "Found saved Dexcom credentials, will attempt auto-login after time sync");
                char username[64], password[64];
                if (nvs_get_dexcom_credentials(username, sizeof(username), password, sizeof(password)) == ESP_OK) {
                    // Copy to buffers
                    strncpy(dexcom_username_buf, username, sizeof(dexcom_username_buf) - 1);
                    strncpy(dexcom_password_buf, password, sizeof(dexcom_password_buf) - 1);

                    // CRITICAL: Wait for SNTP time sync (SSL certificates require accurate time!)
                    ESP_LOGI(TAG, "Waiting for SNTP time sync before Dexcom auth (required for SSL)...");

                    // Check current time before waiting
                    time_t now;
                    struct tm timeinfo;
                    time(&now);
                    localtime_r(&now, &timeinfo);
                    ESP_LOGI(TAG, "Current system time: %04d-%02d-%02d %02d:%02d:%02d",
                             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

                    if (!wait_for_time_sync(60000)) {  // Increased to 60 seconds
                        sd_log(TAG, "Time sync failed after 60s - cannot auth Dexcom");
                        ESP_LOGE(TAG, "Time sync failed after 60s - CANNOT attempt Dexcom auth (SSL will fail)");
                        ESP_LOGE(TAG, "System time is still: %04d-%02d-%02d (needs to be >= 2020)",
                                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

                        // Still start tasks - glucose task will retry auth once time syncs
                        if (weather_task_handle == NULL) {
                            ESP_LOGI(TAG, "Starting weather task (time sync failed but WiFi up)...");
                            xTaskCreate(weather_update_task, "weather_update", 6144, NULL, 1, &weather_task_handle);
                        }
                        if (glucose_task_handle == NULL) {
                            ESP_LOGI(TAG, "Starting glucose task (will auto-retry after time syncs)...");
                            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
                        }
                    } else {
                        // Time is synced, now safe to authenticate
                        ESP_LOGI(TAG, "Time synced successfully, proceeding with Dexcom auto-login");

                        // Retry logic with exponential backoff (life-critical - never give up!)
                        int max_retries = 5;
                        int retry_delay_ms = 2000;  // Start with 2 seconds

                        for (int attempt = 1; attempt <= max_retries; attempt++) {
                            ESP_LOGI(TAG, "Dexcom auth attempt %d/%d...", attempt, max_retries);

                            esp_err_t ret = dexcom_authenticate(username, password);
                            if (ret == ESP_OK) {
                                sd_log(TAG, "Dexcom auto-login OK (attempt %d)", attempt);
                            ESP_LOGI(TAG, "Dexcom auto-login successful on attempt %d!", attempt);

                                // Fetch initial glucose reading
                                dexcom_glucose_t glucose = {0};  // Initialize to prevent garbage data
                                if (dexcom_fetch_glucose(&glucose) == ESP_OK && glucose.valid) {
                                    current_glucose = glucose.value;
                                    current_trend = glucose.trend;
                                    glucose_timestamp = glucose.timestamp;
                                    glucose_data_valid = true;
                                    glucose_status = glucose.status;

                                    // Update timer for progress bar
                                    last_glucose_fetch_time = esp_timer_get_time() / 1000;  // milliseconds

                                    sd_log(TAG, "Initial glucose: %d mg/dL, trend: %s",
                                           glucose.value, dexcom_trend_description(glucose.trend));
                                    ESP_LOGI(TAG, "Initial glucose: %d mg/dL, trend: %s",
                                             glucose.value, dexcom_trend_description(glucose.trend));

                                    // Play success sound on first glucose reading
                                    if (!first_glucose_received) {
                                        first_glucose_received = true;
                                        ESP_LOGI(TAG, "First glucose reading received - playing success sound");
                                        play_success_sound();
                                    }

                                    // Check alarms
                                    check_glucose_alarms(current_glucose);

                                    update_glucose_display();
                                } else {
                                    // No data yet - update status
                                    glucose_data_valid = false;
                                    glucose_status = glucose.status;
                                    update_glucose_display();
                                }

                                // Keep SSL connection alive — glucose task will reuse it
                                // Closing here forces a new ~50KB handshake on a ~24KB heap = guaranteed failure
                                ESP_LOGI(TAG, "===== MEMORY CHECKPOINT: After initial Dexcom fetch =====");
                                ESP_LOGI(TAG, "Free heap: %lu bytes (SSL kept alive for glucose task)", esp_get_free_heap_size());
                                ESP_LOGI(TAG, "Minimum free heap ever: %lu bytes", esp_get_minimum_free_heap_size());

                                // Flush SD logs after initial fetch
                                sd_logger_flush();

                                // Start weather task (uses HTTP - no SSL overhead!)
                                if (weather_task_handle == NULL) {
                                    ESP_LOGI(TAG, "Starting weather task (after Dexcom, HTTP mode)...");
                                    xTaskCreate(weather_update_task, "weather_update", 6144, NULL, 1, &weather_task_handle);
                                }

                                // Wait for weather to complete first fetch before starting glucose task
                                // This prevents both tasks from running simultaneously and competing for memory
                                ESP_LOGI(TAG, "Waiting 30 seconds for weather to complete first fetch...");
                                vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds (weather waits 15s + fetch time)

                                // Start glucose update task (90 second periodic fetches)
                                if (glucose_task_handle == NULL) {
                                    ESP_LOGI(TAG, "===== MEMORY CHECKPOINT: Before starting glucose task =====");
                                    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
                                    ESP_LOGI(TAG, "Minimum free heap ever: %lu bytes", esp_get_minimum_free_heap_size());
                                    ESP_LOGI(TAG, "Starting glucose update task (90 second interval)...");
                                    xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
                                }

                                break;  // Success - exit retry loop
                            } else {
                                sd_log(TAG, "Dexcom auth attempt %d failed: %s", attempt, esp_err_to_name(ret));
                                ESP_LOGW(TAG, "Dexcom auth attempt %d failed: %s", attempt, esp_err_to_name(ret));

                                if (attempt < max_retries) {
                                    ESP_LOGI(TAG, "Retrying in %d ms...", retry_delay_ms);
                                    vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                                    retry_delay_ms *= 2;  // Exponential backoff: 2s, 4s, 8s, 16s
                                } else {
                                    sd_log(TAG, "CRITICAL: Dexcom login failed after %d attempts", max_retries);
                                    ESP_LOGE(TAG, "Dexcom auto-login failed after %d attempts", max_retries);
                                    ESP_LOGE(TAG, "CRITICAL: Starting tasks anyway - glucose task will auto-retry");
                                }
                            }
                        }

                        // Ensure weather and glucose tasks start even if auth failed
                        // The glucose task has auto-re-auth logic and will keep trying
                        if (weather_task_handle == NULL) {
                            ESP_LOGI(TAG, "Starting weather task (auth failed but WiFi connected)...");
                            xTaskCreate(weather_update_task, "weather_update", 6144, NULL, 1, &weather_task_handle);
                        }
                        if (glucose_task_handle == NULL) {
                            ESP_LOGI(TAG, "Starting glucose task (will auto-reconnect to Dexcom)...");
                            xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
                        }
                    }
                }
            } else {
                // No Dexcom credentials - start weather task immediately
                ESP_LOGI(TAG, "No Dexcom credentials found, starting weather task (HTTP mode)...");
                if (weather_task_handle == NULL) {
                    xTaskCreate(weather_update_task, "weather_update", 6144, NULL, 1, &weather_task_handle);
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
