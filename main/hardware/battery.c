/*
 * battery.c
 *
 * Battery monitoring implementation
 */

#include "battery.h"
#include "buzzer.h"
#include "shared_state.h"
#include "ui/home_screen.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "BATTERY";

// Helper function prototypes
static float battery_read_voltage(void);
static int battery_voltage_to_percent(float voltage);
static void battery_low_alert(void);
static void battery_flash_timer_cb(lv_timer_t *timer);

void battery_init(void) {
    ESP_LOGI(TAG, "Initializing battery monitoring");

    // Configure ADC for battery voltage reading
    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &adc_handle));

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,  // 0-3.3V range
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_config));

    // Initialize ADC calibration
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration initialized");
    } else {
        ESP_LOGW(TAG, "ADC calibration failed, using raw values");
        adc_cali_handle = NULL;
    }

    ESP_LOGI(TAG, "Battery monitoring initialized (GPIO %d)", BATTERY_ADC_GPIO);
}

static float battery_read_voltage(void) {
    // Take multiple readings and average for better precision
    const int num_samples = 10;
    int adc_sum = 0;

    for (int i = 0; i < num_samples; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &adc_raw);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            return 0.0f;
        }
        adc_sum += adc_raw;
        vTaskDelay(pdMS_TO_TICKS(2));  // Small delay between samples
    }

    int adc_avg = adc_sum / num_samples;

    int voltage_mv = 0;
    if (adc_cali_handle != NULL) {
        // Use calibrated value
        adc_cali_raw_to_voltage(adc_cali_handle, adc_avg, &voltage_mv);
    } else {
        // Fallback: approximate linear conversion (3.3V / 4096 for 12-bit ADC)
        voltage_mv = (adc_avg * 3300) / 4096;
    }

    // Account for voltage divider
    float battery_volts = (voltage_mv / 1000.0f) * BATTERY_VOLTAGE_DIVIDER;

    // Diagnostic logging for calibration (3 decimal places)
    ESP_LOGI(TAG, "Battery ADC: raw=%d, voltage_mv=%d, calculated=%.3fV (divider=%.2fx)",
             adc_avg, voltage_mv, battery_volts, BATTERY_VOLTAGE_DIVIDER);

    return battery_volts;
}

static int battery_voltage_to_percent(float voltage) {
    // LiPo discharge curve adjusted for under-load voltage:
    // 4.0V = 100%, 3.9V = 90%, 3.8V = 80%, 3.7V = 65%, 3.6V = 40%
    // 3.5V = 20%, 3.4V = 10%, 3.2V = 0%

    if (voltage >= 4.0f) return 100;
    if (voltage >= 3.9f) return 90 + (int)((voltage - 3.9f) * 100.0f); // 90-100%
    if (voltage >= 3.8f) return 80 + (int)((voltage - 3.8f) * 100.0f); // 80-90%
    if (voltage >= 3.7f) return 65 + (int)((voltage - 3.7f) * 150.0f); // 65-80%
    if (voltage >= 3.6f) return 40 + (int)((voltage - 3.6f) * 250.0f); // 40-65%
    if (voltage >= 3.5f) return 20 + (int)((voltage - 3.5f) * 200.0f); // 20-40%
    if (voltage >= 3.4f) return 10 + (int)((voltage - 3.4f) * 100.0f); // 10-20%
    if (voltage >= 3.2f) return (int)((voltage - 3.2f) * 50.0f);       // 0-10%
    return 0;
}

static void battery_low_alert(void) {
    // Play 3 beeps (250ms apart) to warn of low battery
    const uint8_t volume = 70;
    for (int i = 0; i < 3; i++) {
        buzzer_tone(800, volume);
        vTaskDelay(pdMS_TO_TICKS(100));
        buzzer_stop();
        if (i < 2) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    ESP_LOGW(TAG, "Low battery alert played (10%% warning)");
}

static void battery_flash_timer_cb(lv_timer_t *timer) {
    // Flash battery icon red when battery is at 10%
    static bool flash_state = false;

    if (battery_canvas != NULL && lvgl_port_lock(0)) {
        uint32_t color = flash_state ? COLOR_RED : COLOR_TEXT_WHITE;
        draw_battery_icon(battery_canvas, color, 1);  // 1 bar for low battery
        flash_state = !flash_state;
        lvgl_port_unlock();
    }
}

void update_battery_display(void) {
    if (battery_canvas == NULL) {
        return;  // UI not initialized yet
    }

    if (!lvgl_port_lock(0)) {
        return;
    }

    // Determine number of bars based on percentage (1-4 bars)
    int bars;
    if (battery_percent >= 75) {
        bars = 4;
    } else if (battery_percent >= 50) {
        bars = 3;
    } else if (battery_percent >= 25) {
        bars = 2;
    } else {
        bars = 1;
    }

    // Determine color based on percentage
    uint32_t color;
    if (battery_percent >= 80) {
        color = COLOR_GREEN;  // Green for 80%+
    } else if (battery_percent >= 20) {
        color = COLOR_ORANGE;  // Amber/Orange for 20-80%
    } else {
        color = COLOR_RED;  // Red for 0-20% (will flash if < 10%)
    }

    // Redraw battery icon with current bars and color
    // Only redraw with color if not flashing (< 10%)
    if (battery_percent >= 10) {
        draw_battery_icon(battery_canvas, color, bars);
    }

    lvgl_port_unlock();
}

void battery_monitor_task(void *arg) {
    ESP_LOGI(TAG, "Battery monitor task started");

    while (1) {
        // Read battery voltage
        battery_voltage = battery_read_voltage();

        // Calculate battery percentage
        battery_percent = battery_voltage_to_percent(battery_voltage);

        ESP_LOGI(TAG, "Battery: %.3fV, %d%%", battery_voltage, battery_percent);

        // Update display
        update_battery_display();

        // Check for critical low battery (3.2V or below) - shut down to prevent damage
        if (battery_voltage <= BATTERY_LOW_VOLTAGE) {
            ESP_LOGE(TAG, "CRITICAL: Battery voltage %.3fV <= %.3fV - SHUTTING DOWN",
                     battery_voltage, BATTERY_LOW_VOLTAGE);

            // Show critical battery message on display
            if (lvgl_port_lock(0)) {
                // Create full-screen message
                lv_obj_t *shutdown_screen = lv_obj_create(NULL);
                lv_obj_set_style_bg_color(shutdown_screen, lv_color_black(), 0);

                lv_obj_t *label = lv_label_create(shutdown_screen);
                lv_label_set_text(label, "CRITICAL LOW BATTERY\n\nShutting down...\n\nPlease charge device");
                lv_obj_set_style_text_color(label, lv_color_hex(COLOR_RED), 0);
                lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

                lv_scr_load(shutdown_screen);
                lvgl_port_unlock();
            }

            // Play shutdown tone
            buzzer_tone(400, 80);
            vTaskDelay(pdMS_TO_TICKS(500));
            buzzer_stop();

            // Wait a moment for user to see message
            vTaskDelay(pdMS_TO_TICKS(3000));

            // Deep sleep until external wake (USB plug-in)
            ESP_LOGI(TAG, "Entering deep sleep...");
            esp_deep_sleep_start();
        }

        // Check for 10% battery warning
        if (battery_percent <= BATTERY_CRITICAL_PERCENT && !battery_low_warning_shown) {
            battery_low_warning_shown = true;

            // Play warning beeps
            battery_low_alert();

            // Start flashing timer
            if (battery_flash_timer == NULL) {
                battery_flash_timer = lv_timer_create(battery_flash_timer_cb, 500, NULL);  // Flash every 500ms
            }
        } else if (battery_percent > BATTERY_CRITICAL_PERCENT) {
            // Reset warning flag when battery recovers
            battery_low_warning_shown = false;

            // Stop flashing timer
            if (battery_flash_timer != NULL) {
                lv_timer_del(battery_flash_timer);
                battery_flash_timer = NULL;
            }
        }

        // Update every 30 seconds
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
