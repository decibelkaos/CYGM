/*
 * background_tasks.c
 *
 * FreeRTOS background tasks implementation
 */

#include "background_tasks.h"
#include "shared_state.h"
#include "hardware/display.h"
#include "hardware/battery.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "ui/home_screen.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "BACKGROUND_TASKS";

void ui_task_core1(void *pvParameters) {
    ESP_LOGI(TAG, "UI Task started on Core 1");

    // Initialize touch and keyboard on Core 1
    add_boot_log("Init touch screen");
    ESP_ERROR_CHECK(touch_init_core1());
    add_boot_log("Init keyboard");
    ESP_ERROR_CHECK(cardkb_init());

    // Initialize LVGL
    add_boot_log("Init LVGL");
    ESP_ERROR_CHECK(lvgl_init());

    // Initialize LVGL styles
    init_styles();

    // Create UI screens
    add_boot_log("Create UI screens");
    create_splash_screen();
    create_home_screen();
    create_menu_screen();
    create_wifi_list_screen();
    // Don't create password screen yet - create on demand
    // create_wifi_password_screen();

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

    // Switch to home screen
    lv_scr_load(screen_home);

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
    // Just keep task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void wifi_task_core0(void *pvParameters) {
    ESP_LOGI(TAG, "WiFi Task started on Core 0");

    // Initialize WiFi manager
    add_boot_log("Init WiFi");
    ESP_ERROR_CHECK(wifi_manager_init());

    // Check for saved credentials
    wifi_credentials_t creds;
    bool has_credentials = (nvs_load_wifi_credentials(&creds) == ESP_OK);

    if (!has_credentials) {
        ESP_LOGI(TAG, "No saved WiFi credentials - setup required");
        add_boot_log("WiFi: Setup needed");
        led_set_breathing_color(127, 127, 0);  // Yellow = setup needed
        // TODO: Implement WiFi setup UI
    } else {
        ESP_LOGI(TAG, "Connecting to WiFi: %s", creds.ssid);
        add_boot_log("Connecting WiFi...");
        led_set_breathing_color(127, 127, 0);  // Yellow = connecting

        if (wifi_manager_connect() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected successfully");
            add_boot_log("WiFi connected!");
            wifi_connected = true;
            led_set_breathing_color(0, 127, 0);  // Green = connected

            // Update WiFi status icon color
            if (lvgl_port_lock(0)) {
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

            // Start time update task
            xTaskCreate(time_update_task, "time_update", 4096, NULL, 1, NULL);

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
                        ESP_LOGE(TAG, "Time sync failed after 60s - CANNOT attempt Dexcom auth (SSL will fail)");
                        ESP_LOGE(TAG, "System time is still: %04d-%02d-%02d (needs to be >= 2020)",
                                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
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
                                ESP_LOGI(TAG, "Dexcom auto-login successful on attempt %d!", attempt);

                                // Fetch initial glucose reading
                                dexcom_glucose_t glucose = {0};  // Initialize to prevent garbage data
                                if (dexcom_fetch_glucose(&glucose) == ESP_OK && glucose.valid) {
                                    current_glucose = glucose.value;
                                    current_trend = glucose.trend;
                                    glucose_timestamp = glucose.timestamp;
                                    glucose_data_valid = true;
                                    glucose_status = glucose.status;
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

                                // Start weather task (uses HTTP - no SSL overhead!)
                                if (weather_task_handle == NULL) {
                                    ESP_LOGI(TAG, "Starting weather task (after Dexcom, HTTP mode)...");
                                    xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
                                }

                                // Start glucose update task (90 second periodic fetches)
                                if (glucose_task_handle == NULL) {
                                    ESP_LOGI(TAG, "Starting glucose update task (90 second interval)...");
                                    xTaskCreate(glucose_update_task, "glucose_update", 4096, NULL, 1, &glucose_task_handle);
                                }

                                break;  // Success - exit retry loop
                            } else {
                                ESP_LOGW(TAG, "Dexcom auth attempt %d failed: %s", attempt, esp_err_to_name(ret));

                                if (attempt < max_retries) {
                                    ESP_LOGI(TAG, "Retrying in %d ms...", retry_delay_ms);
                                    vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                                    retry_delay_ms *= 2;  // Exponential backoff: 2s, 4s, 8s, 16s
                                } else {
                                    ESP_LOGE(TAG, "Dexcom auto-login failed after %d attempts", max_retries);
                                    ESP_LOGE(TAG, "CRITICAL: Will continue trying in background task");
                                    // TODO: Create background retry task for life-critical reconnection
                                }
                            }
                        }
                    }
                }
            } else {
                // No Dexcom credentials - start weather task immediately
                ESP_LOGI(TAG, "No Dexcom credentials found, starting weather task (HTTP mode)...");
                if (weather_task_handle == NULL) {
                    xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
                }
            }
        } else {
            ESP_LOGE(TAG, "WiFi connection failed");
            led_set_breathing_color(127, 0, 0);  // Red = failed

            // Update WiFi status on home screen
            if (lvgl_port_lock(0)) {
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " Failed");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
                lvgl_port_unlock();
            }
        }
    }

    // Monitor WiFi connection
    while (1) {
        // TODO: Periodic WiFi health check
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
