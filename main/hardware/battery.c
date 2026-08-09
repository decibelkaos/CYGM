/*
 * battery.c
 *
 * ADC battery monitoring, charge-state detection and the battery indicator.
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
#include "sd_logger.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

static const char *TAG = "BATTERY";

// Helper function prototypes
static float battery_read_voltage(void);
static float battery_quick_read_voltage(void);
static int battery_voltage_to_percent(float voltage);
static void battery_low_alert(void);
static void battery_flash_timer_cb(lv_timer_t *timer);
static void charging_anim_timer_cb(lv_timer_t *timer);

void battery_init(void) {
    ESP_LOGI(TAG, "Initializing battery monitoring");

    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &adc_handle));

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,  // 0-3.3V range
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_config));

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
        vTaskDelay(1);  // Yield between samples (1 tick regardless of tick rate)
    }

    int adc_avg = adc_sum / num_samples;

    int voltage_mv = 0;
    if (adc_cali_handle != NULL) {
        adc_cali_raw_to_voltage(adc_cali_handle, adc_avg, &voltage_mv);
    } else {
        // Fallback: approximate linear conversion (3.3V / 4096 for 12-bit ADC)
        voltage_mv = (adc_avg * 3300) / 4096;
    }

    // Account for voltage divider
    float battery_volts = (voltage_mv / 1000.0f) * BATTERY_VOLTAGE_DIVIDER;

    ESP_LOGI(TAG, "Battery ADC: raw=%d, voltage_mv=%d, calculated=%.3fV (divider=%.2fx)",
             adc_avg, voltage_mv, battery_volts, BATTERY_VOLTAGE_DIVIDER);

    return battery_volts;
}

// Lightweight ADC read — fewer samples, no logging (~8ms). Used for fast change
// detection between full processing cycles.
static float battery_quick_read_voltage(void) {
    const int num_samples = 4;
    int adc_sum = 0;
    for (int i = 0; i < num_samples; i++) {
        int adc_raw = 0;
        if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &adc_raw) != ESP_OK) return 0.0f;
        adc_sum += adc_raw;
        vTaskDelay(1);  // Yield between samples (1 tick regardless of tick rate)
    }
    int adc_avg = adc_sum / num_samples;
    int voltage_mv = 0;
    if (adc_cali_handle != NULL) {
        adc_cali_raw_to_voltage(adc_cali_handle, adc_avg, &voltage_mv);
    } else {
        voltage_mv = (adc_avg * 3300) / 4096;
    }
    return (voltage_mv / 1000.0f) * BATTERY_VOLTAGE_DIVIDER;
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

// Charging-compensated curve: maps terminal voltage measured DURING charging back
// to approximate real SOC. The charger pushes ~850mA through the cell's internal
// resistance (~100-350mOhm, varying with SOC), which inflates the reading, so this
// is the OCV curve shifted right by I x Rint. Terminal breakpoints:
//   3.50=0%, 3.68=10%, 3.77=20%, 3.84=30%, 3.86=40%, 3.88=50%,
//   3.91=65%, 3.97=80%, 4.03=90%, 4.09=100%
static int charging_voltage_to_percent(float voltage) {
    if (voltage >= 4.09f) return 100;
    if (voltage >= 4.03f) return 90 + (int)((voltage - 4.03f) * 167.0f);
    if (voltage >= 3.97f) return 80 + (int)((voltage - 3.97f) * 167.0f);
    if (voltage >= 3.91f) return 65 + (int)((voltage - 3.91f) * 250.0f);
    if (voltage >= 3.88f) return 50 + (int)((voltage - 3.88f) * 500.0f);
    if (voltage >= 3.86f) return 40 + (int)((voltage - 3.86f) * 500.0f);
    if (voltage >= 3.84f) return 30 + (int)((voltage - 3.84f) * 500.0f);
    if (voltage >= 3.77f) return 20 + (int)((voltage - 3.77f) * 143.0f);
    if (voltage >= 3.68f) return 10 + (int)((voltage - 3.68f) * 111.0f);
    if (voltage >= 3.50f) return (int)((voltage - 3.50f) * 56.0f);
    return 0;
}

// Smooth green→red gradient: 100%=green, 15%=red, 0-15%=solid red
uint32_t battery_percent_to_color(int percent) {
    if (percent <= 15) return COLOR_RED;
    // Map 15-100% onto HSV hue 0 (red) to 120 (green) at full saturation.
    int hue = (percent - 15) * 120 / 85;  // 0 at 15%, 120 at 100%
    lv_color_t c = lv_color_hsv_to_rgb(hue, 100, 100);
    return (lv_color_to32(c) & 0x00FFFFFF);  // Strip alpha
}

// Darker variant for battery fill gradient
uint32_t battery_percent_to_dark_color(int percent) {
    if (percent <= 15) return 0x991B1B;  // dark red
    int hue = (percent - 15) * 120 / 85;
    lv_color_t c = lv_color_hsv_to_rgb(hue, 100, 50);  // Half brightness
    return (lv_color_to32(c) & 0x00FFFFFF);
}

static void battery_low_alert(void) {
    // Three beeps 250ms apart. 90 on the perceptual buzzer curve is as loud as
    // the old linear 70.
    const uint8_t volume = 90;
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

// Forward reference — defined with the other charging statics below.
static bool bat_confirmed_charging = false;

bool battery_is_charging(void) {
    return bat_confirmed_charging;
}

static void battery_flash_timer_cb(lv_timer_t *timer) {
    // Flash battery icon red when battery is at 10%
    static bool flash_state = false;

    if (battery_canvas != NULL && lvgl_port_lock(1)) {
        uint32_t color = flash_state ? COLOR_RED : COLOR_TEXT_WHITE;
        draw_battery_icon(battery_canvas, color, battery_percent);
        flash_state = !flash_state;
        lvgl_port_unlock();
    }
}

// Charging animation base level — sweep starts here, goes to 100%
static int charging_anim_base = 0;

static void charging_anim_timer_cb(lv_timer_t *timer) {
    // Animate battery fill: sweeps from current level to 100%, then repeats
    static int anim_fill = 0;
    static int anim_pause = 0;

    if (battery_canvas == NULL || !lvgl_port_lock(1)) return;

    if (anim_pause > 0) {
        anim_pause--;
        lvgl_port_unlock();
        return;
    }

    int range = 100 - charging_anim_base;
    if (range < 10) range = 10;  // Always animate at least 10%

    anim_fill += 3;  // 3% per tick at 100ms = ~3 seconds for full sweep
    if (anim_fill > range) {
        anim_fill = 0;
        anim_pause = 8;  // 800ms pause at full before repeating
    }

    int display_fill = charging_anim_base + anim_fill;
    if (display_fill > 100) display_fill = 100;

    draw_battery_icon(battery_canvas, COLOR_GREEN, display_fill);
    lvgl_port_unlock();
}

void update_battery_display(void) {
    if (battery_canvas == NULL) {
        return;  // UI not initialized yet
    }

    // During charging, the animation timer handles icon drawing
    if (bat_confirmed_charging) {
        // Show pre-charge percentage in green (ADC is inflated during charging)
        if (battery_percent_label != NULL && lvgl_port_lock(1)) {
            lv_label_set_text_fmt(battery_percent_label, "%d%%", battery_percent);
            lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(COLOR_GREEN), 0);
            lvgl_port_unlock();
        }
        return;
    }

    if (!lvgl_port_lock(1)) {
        return;
    }

    // Smooth color gradient: green at 100% → red at 15%
    uint32_t color = battery_percent_to_color(battery_percent);

    // Redraw battery icon with smooth fill (skip if flashing at <10%)
    if (battery_percent >= 10) {
        draw_battery_icon(battery_canvas, color, battery_percent);
    }

    // Update percentage label text and color (label may be hidden - tap to show)
    if (battery_percent_label != NULL) {
        lv_label_set_text_fmt(battery_percent_label, "%d%%", battery_percent);
        lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(color), 0);
    }

    lvgl_port_unlock();
}

// --- Battery averaging and charging detection ---
#define BATTERY_AVG_SAMPLES     8   // Rolling average window
#define BATTERY_MAX_STEP        2   // Max % change per cycle (prevents visual jumps)

// Charging detection is trend-based: the charger shares the ADC pin and elevates
// terminal voltage, so no single reading can separate charging from a full cell.
#define CHARGE_BOOT_FAST_COUNT  5      // 5 fast readings at boot for reliable trend analysis
#define CHARGE_BOOT_INTERVAL_MS 10000  // 10s between boot readings (50s observation window)
#define CHARGE_MIN_VOLTAGE      3.50f  // Below this, definitely not charging
#define CHARGE_STABLE_VOLTAGE   3.90f  // Must be above this for trend detection
#define CHARGE_HIGH_VOLTAGE     4.05f  // Above this = definitely charging (OCV rarely exceeds 4.0V)
#define CHARGE_RISE_THRESHOLD   0.025f // 25mV net positive rise required for trend detection
#define CHARGE_JUMP_PERCENT     15     // Raw% jump > 15 in one cycle = plug-in detected
#define UNPLUG_DROP_VOLTAGE     0.06f  // Voltage drop > 0.06V in one cycle = charger removed
#define CHARGE_CUMULATIVE_DROP  0.03f  // Dropped 0.03V below charge-start = false positive
#define CHARGE_DECLINE_COUNT    2      // 2 consecutive voltage declines while "charging" = stop

static int  bat_history[BATTERY_AVG_SAMPLES];
static int  bat_history_count = 0;
static int  bat_history_idx = 0;
static int  bat_displayed_percent = -1;  // -1 = not yet initialized
static float bat_prev_voltage = 0.0f;    // Previous cycle voltage (for jump detection)
static int  bat_prev_raw_percent = -1;   // Previous cycle raw percent
// bat_confirmed_charging declared above update_battery_display (forward reference)
static int  bat_pre_charge_percent = -1; // Last trustworthy % before charger was plugged in
static int   bat_boot_readings = 0;         // Counter for fast boot phase
static float bat_charge_start_voltage = 0;  // Voltage when charging detected
static float bat_voltage_history[5] = {0};  // Last 5 voltages for trend detection (40s window at boot)
static int   bat_voltage_hist_count = 0;    // How many trend entries collected
static int   bat_decline_count = 0;         // Consecutive voltage declines while "charging"

static int battery_compute_average(void) {
    if (bat_history_count == 0) return 0;
    int sum = 0;
    int n = (bat_history_count < BATTERY_AVG_SAMPLES) ? bat_history_count : BATTERY_AVG_SAMPLES;
    for (int i = 0; i < n; i++) sum += bat_history[i];
    return sum / n;
}

static void battery_start_charging(bool from_jump) {
    if (bat_confirmed_charging) return;
    bat_confirmed_charging = true;
    bat_charge_start_voltage = battery_voltage;  // Track for false-positive correction

    if (from_jump) {
        // Mid-session: bat_displayed_percent is the real pre-charge level
        bat_pre_charge_percent = bat_displayed_percent;
    } else {
        // Boot/trend: bat_displayed_percent is inflated — use charging curve
        // on the first reading (before TP4056 stabilization inflates further)
        bat_pre_charge_percent = charging_voltage_to_percent(bat_voltage_history[0]);
    }

    battery_percent = bat_pre_charge_percent;
    charging_anim_base = (bat_pre_charge_percent > 0) ? bat_pre_charge_percent : 0;

    bat_decline_count = 0;

    ESP_LOGI(TAG, "Charging detected (%.3fV, corrected=%d%%, anim_base=%d%%, method=%s)",
             battery_voltage, bat_pre_charge_percent, charging_anim_base,
             from_jump ? "jump" : "trend");
    sd_log(TAG, "Charging: STARTED (%.3fV, corrected=%d%%)", battery_voltage, bat_pre_charge_percent);

    // Start fill animation (LVGL lock required for thread-safe timer creation)
    if (lvgl_port_lock(1)) {
        if (battery_charging_fade_timer == NULL) {
            battery_charging_fade_timer = lv_timer_create(charging_anim_timer_cb, 100, NULL);
        }
        lvgl_port_unlock();
    }

    update_battery_display();
}

static void battery_stop_charging(void) {
    if (!bat_confirmed_charging) return;
    bat_confirmed_charging = false;
    bat_decline_count = 0;

    ESP_LOGI(TAG, "Charging stopped (%.3fV)", battery_voltage);
    sd_log(TAG, "Charging: STOPPED (%.3fV)", battery_voltage);

    // Stop animation timer and redraw icon (LVGL lock covers both operations
    // atomically — prevents orphaned timers from race with LVGL task)
    bool locked = false;
    for (int retry = 0; retry < 50 && !locked; retry++) {
        locked = lvgl_port_lock(1);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (locked) {
        if (battery_charging_fade_timer != NULL) {
            lv_timer_del(battery_charging_fade_timer);
            battery_charging_fade_timer = NULL;
        }
        if (battery_canvas != NULL) {
            uint32_t color = battery_percent_to_color(battery_percent);
            draw_battery_icon(battery_canvas, color, battery_percent);
        }
        lvgl_port_unlock();
    }

    // Reset averaging history — readings during charging were inflated
    bat_history_count = 0;
    bat_history_idx = 0;
    bat_displayed_percent = -1;  // Next reading will initialize fresh
    bat_charge_start_voltage = 0;
    bat_voltage_hist_count = 0;
}

void battery_monitor_task(void *arg) {
    ESP_LOGI(TAG, "Battery monitor task started");

    while (1) {
        // Only check if on home screen (device awake)
        if (!home_screen_active) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        battery_voltage = battery_read_voltage();
        int raw_percent = battery_voltage_to_percent(battery_voltage);

        // Voltage history for trend detection: [0]=oldest, [4]=newest.
        if (bat_voltage_hist_count < 5) {
            bat_voltage_history[bat_voltage_hist_count] = battery_voltage;
            bat_voltage_hist_count++;
        } else {
            for (int i = 0; i < 4; i++) bat_voltage_history[i] = bat_voltage_history[i + 1];
            bat_voltage_history[4] = battery_voltage;
        }

        // --- Charging detection ---
        if (!bat_confirmed_charging) {
            bool detected = false;

            // Method 1: Sudden raw% jump (charger plugged in mid-session)
            bool from_jump = false;
            if (bat_prev_raw_percent >= 0 &&
                (raw_percent - bat_prev_raw_percent > CHARGE_JUMP_PERCENT)) {
                ESP_LOGI(TAG, "Charger detected (jump): raw=%d%% prev=%d%%",
                         raw_percent, bat_prev_raw_percent);
                detected = true;
                from_jump = true;
            }

            // Method 2: monotonic rising trend across 5 readings above 3.90V with a
            // total rise over 25mV. Without the monotonic test the 5-10mV of ADC
            // noise on the flat 3.85-3.95V plateau reads as a rise and false-positives.
            if (!detected && bat_voltage_hist_count >= 5 &&
                battery_voltage >= CHARGE_STABLE_VOLTAGE) {
                float rise = bat_voltage_history[4] - bat_voltage_history[0];
                bool monotonic = true;
                for (int i = 1; i < 5; i++) {
                    if (bat_voltage_history[i] < bat_voltage_history[i - 1] - 0.003f) {
                        monotonic = false;
                        break;
                    }
                }
                if (monotonic && rise > CHARGE_RISE_THRESHOLD) {
                    int trend_corrected = charging_voltage_to_percent(battery_voltage);
                    if (trend_corrected >= 25) {
                        ESP_LOGI(TAG, "Charger detected (trend): V=%.3f->%.3f->%.3f->%.3f->%.3f (+%.1fmV, corrected=%d%%)",
                                 bat_voltage_history[0], bat_voltage_history[1], bat_voltage_history[2],
                                 bat_voltage_history[3], bat_voltage_history[4], rise * 1000.0f, trend_corrected);
                        detected = true;
                    }
                } else {
                    ESP_LOGD(TAG, "Trend check: V=%.3f->%.3f (rise=%.1fmV, mono=%d)",
                             bat_voltage_history[0], bat_voltage_history[4], rise * 1000.0f, monotonic);
                }
            }

            // Method 3: OCV almost never exceeds 4.05V during normal discharge, so a
            // terminal voltage this high means the charger is connected.
            if (!detected && battery_voltage >= CHARGE_HIGH_VOLTAGE) {
                ESP_LOGI(TAG, "Charger detected (high voltage): V=%.3f >= %.3fV",
                         battery_voltage, CHARGE_HIGH_VOLTAGE);
                detected = true;
            }

            if (detected) {
                battery_start_charging(from_jump);
            }
        } else {
            // Currently charging — check for unplug
            bool should_stop = false;

            // Check 1: Large voltage drop in one cycle (charger physically removed)
            if (bat_prev_voltage > 0 &&
                (bat_prev_voltage - battery_voltage > UNPLUG_DROP_VOLTAGE)) {
                ESP_LOGI(TAG, "Charger unplugged (drop): V=%.3f->%.3f",
                         bat_prev_voltage, battery_voltage);
                should_stop = true;
            }

            // Check 2: Voltage below minimum (clearly not being charged)
            if (battery_voltage < CHARGE_MIN_VOLTAGE) {
                ESP_LOGI(TAG, "Charging stopped (low voltage): V=%.3f", battery_voltage);
                should_stop = true;
            }

            // Check 3: Cumulative drop from charge-start (false positive correction)
            if (bat_charge_start_voltage > 0 &&
                (bat_charge_start_voltage - battery_voltage > CHARGE_CUMULATIVE_DROP)) {
                ESP_LOGI(TAG, "Charging stopped (cumulative drop): start=%.3f now=%.3f",
                         bat_charge_start_voltage, battery_voltage);
                should_stop = true;
            }

            // Check 4: voltage declining for CHARGE_DECLINE_COUNT consecutive readings
            // means the device is discharging — a false positive.
            if (!should_stop && bat_prev_voltage > 0 &&
                battery_voltage < bat_prev_voltage - 0.005f) {
                bat_decline_count++;
                if (bat_decline_count >= CHARGE_DECLINE_COUNT) {
                    ESP_LOGI(TAG, "Charging stopped (declining trend): %d consecutive drops, V=%.3f",
                             bat_decline_count, battery_voltage);
                    should_stop = true;
                }
            } else if (!should_stop) {
                bat_decline_count = 0;
            }

            if (should_stop) {
                battery_stop_charging();
                bat_prev_voltage = battery_voltage;
                bat_prev_raw_percent = raw_percent;
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }
        }

        bat_prev_voltage = battery_voltage;
        bat_prev_raw_percent = raw_percent;

        // --- Normal battery processing (only when NOT charging) ---
        if (!bat_confirmed_charging) {
            bat_history[bat_history_idx] = raw_percent;
            bat_history_idx = (bat_history_idx + 1) % BATTERY_AVG_SAMPLES;
            if (bat_history_count < BATTERY_AVG_SAMPLES) bat_history_count++;

            int avg_percent = battery_compute_average();

            // Clamp step change to prevent visible jumps (max ±BATTERY_MAX_STEP per cycle)
            if (bat_displayed_percent < 0) {
                bat_displayed_percent = avg_percent;  // First reading — initialize directly
            } else {
                int delta = avg_percent - bat_displayed_percent;
                if (delta > BATTERY_MAX_STEP) delta = BATTERY_MAX_STEP;
                if (delta < -BATTERY_MAX_STEP) delta = -BATTERY_MAX_STEP;
                bat_displayed_percent += delta;
            }

            if (bat_displayed_percent > 100) bat_displayed_percent = 100;
            if (bat_displayed_percent < 0) bat_displayed_percent = 0;

            // Set the global that the display uses
            battery_percent = bat_displayed_percent;

            ESP_LOGI(TAG, "Battery: %.3fV, raw=%d%%, avg=%d%%, display=%d%%",
                     battery_voltage, raw_percent, avg_percent, bat_displayed_percent);

            update_battery_display();

            // Check for critical low battery (3.2V or below) — shut down to prevent damage
            if (battery_voltage <= BATTERY_LOW_VOLTAGE) {
                ESP_LOGE(TAG, "CRITICAL: Battery voltage %.3fV <= %.3fV - SHUTTING DOWN",
                         battery_voltage, BATTERY_LOW_VOLTAGE);
                sd_log(TAG, "CRITICAL: %.3fV - SHUTDOWN", battery_voltage);
                sd_logger_flush();

                if (lvgl_port_lock(1)) {
                    lv_obj_t *shutdown_screen = lv_obj_create(NULL);
                    lv_obj_set_style_bg_color(shutdown_screen, lv_color_black(), 0);
                    lv_obj_clear_flag(shutdown_screen, LV_OBJ_FLAG_SCROLLABLE);

                    lv_obj_t *label = lv_label_create(shutdown_screen);
                    lv_label_set_text(label, "CRITICAL LOW BATTERY\n\nShutting down...\n\nPlease charge device");
                    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_RED), 0);
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
                    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

                    lv_scr_load(shutdown_screen);
                    lvgl_port_unlock();
                }

                buzzer_tone(400, 95);  // Perceptual curve: 95 == the old 80
                vTaskDelay(pdMS_TO_TICKS(500));
                buzzer_stop();
                vTaskDelay(pdMS_TO_TICKS(3000));

                ESP_LOGI(TAG, "Entering deep sleep...");
                esp_deep_sleep_start();
            }

            // Check for 10% battery warning
            if (battery_percent <= BATTERY_CRITICAL_PERCENT && !battery_low_warning_shown) {
                battery_low_warning_shown = true;
                sd_log(TAG, "LOW: %d%% (%.3fV) - alert played", battery_percent, battery_voltage);
                battery_low_alert();
                if (battery_flash_timer == NULL) {
                    battery_flash_timer = lv_timer_create(battery_flash_timer_cb, 500, NULL);
                }
            }
        } else {
            // Charging: use the compensated curve, which accounts for the IR rise.
            int corrected_percent = charging_voltage_to_percent(battery_voltage);
            if (corrected_percent > 100) corrected_percent = 100;
            if (corrected_percent < 0) corrected_percent = 0;

            // Step-clamp to ±2% per reading (smooths ADC noise in the flat region)
            int delta = corrected_percent - battery_percent;
            if (delta > BATTERY_MAX_STEP) delta = BATTERY_MAX_STEP;
            if (delta < -BATTERY_MAX_STEP) delta = -BATTERY_MAX_STEP;
            battery_percent += delta;
            if (battery_percent > 100) battery_percent = 100;
            if (battery_percent < 0) battery_percent = 0;

            update_battery_display();

            // Raise animation base as charge progresses (sweep area shrinks)
            if (battery_percent > charging_anim_base) {
                charging_anim_base = battery_percent;
                if (charging_anim_base > 95) charging_anim_base = 95;
            }

            ESP_LOGI(TAG, "Charging: %.3fV, curve=%d%%, display=%d%%, anim=%d%%",
                     battery_voltage, corrected_percent, battery_percent, charging_anim_base);
        }

        // Periodic SD flush, skipped only during the brief glucose fetch. Mounting
        // uses sub-1KB allocations, so only the active fetch competes for heap.
        if (sd_logger_available() && !glucose_fetch_in_progress) {
            sd_logger_flush();
        }

        // Fast boot readings, then normal interval with change detection
        bat_boot_readings++;
        if (bat_boot_readings <= CHARGE_BOOT_FAST_COUNT) {
            // Boot phase: fast readings at 2s intervals
            vTaskDelay(pdMS_TO_TICKS(CHARGE_BOOT_INTERVAL_MS));
        } else {
            // Normal phase: sleep in 5s chunks, quick-check ADC for plug/unplug
            // Full processing runs every 60s OR immediately on voltage change
            float last_check_v = battery_voltage;
            int elapsed_ms = 0;
            while (elapsed_ms < 60000) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                elapsed_ms += 5000;

                if (!home_screen_active) break;

                float quick_v = battery_quick_read_voltage();
                float delta_v = quick_v - last_check_v;

                // +0.08V = likely charger plugged in (TP4056 IR rise)
                // -0.10V = likely charger removed (voltage drop back to OCV)
                if (delta_v > 0.08f || delta_v < -0.10f) {
                    ESP_LOGI(TAG, "Voltage change: %.3fV -> %.3fV (delta=%.3fV) — fast cycle",
                             last_check_v, quick_v, delta_v);
                    break;
                }
            }
        }
    }
}
