/*
 * background_tasks.c
 *
 * The canonical background-task creation table, the UI watchdog and the WiFi task.
 */

#include "background_tasks.h"
#include "shared_state.h"
#include "main.h"
#include "hardware/display.h"
#include "hardware/battery.h"
#include "hardware/led.h"
#include "hardware/buzzer.h"
#include "hardware/screenshot.h"
#include "features/time_system.h"
#include "features/weather_system.h"
#include "ui/home_screen.h"
#include "ui/menu_screen.h"
#include "ui/wifi_screens.h"
#include "wifi_manager.h"
#include "dexcom_api.h"
#include "libre_api.h"
#include "nightscout_api.h"
#include "cgm_types.h"
#include "sd_logger.h"
#include "features/heartbeat.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/task.h"

static const char *TAG = "BACKGROUND_TASKS";

#define UI_INACTIVITY_RETURN_MS 30000   // Idle on a sub-screen -> back to home
#define UI_DIM_TIMEOUT_MS      120000   // Idle anywhere -> backlight floor
#define UI_DIM_LEVEL_DAY            10  // Backlight % while dimmed, day
#define UI_DIM_LEVEL_NIGHT           1  // Backlight % while dimmed, night window
#define WATCHDOG_TICK_MS           100  // Button debounce + wake latency
#define WATCHDOG_TICKS_PER_SEC      (1000 / WATCHDOG_TICK_MS)
#define LVGL_POOL_LOG_SEC           60  // Leak-hunt sample of the LVGL pool
#define BOOT_BUTTON_GPIO      GPIO_NUM_0
#define BOOT_BTN_DEBOUNCE_TICKS      2  // 200ms low — a USB glitch settles in ~10ms
#define SPLASH_MIN_MS             1500  // Keep the boot log readable on a fast boot
#define SPLASH_MAX_MS             8000  // Hard ceiling when the network hangs

// Set by wifi_task_core0 as soon as the network outcome is known. The splash gate
// waits on this instead of a fixed delay. It deliberately excludes the first
// glucose fetch — that waits for home_screen_active, so gating on it would deadlock.
static volatile bool boot_net_resolved = false;

// ==================== Canonical background-task table ====================
// The single creation point for every background task. Boot, WiFi reconnect,
// post-TLS recreate and the screenshot restart all go through it, so no call site
// can drift on stack, priority or core: an unpinned glucose_update at priority 5
// preempts the LVGL render task whenever the scheduler puts it on core 1.

typedef struct {
    TaskHandle_t  *handle;
    TaskFunction_t fn;
    const char    *name;
    uint32_t       stack;
    UBaseType_t    prio;
    BaseType_t     core;
} cygm_task_def_t;

enum {
    CYGM_TASK_GLUCOSE = 0,
    CYGM_TASK_WEATHER,
    CYGM_TASK_TIME,
    CYGM_TASK_BATTERY,
    CYGM_TASK_COUNT
};

static const cygm_task_def_t k_task_defs[CYGM_TASK_COUNT] = {
    [CYGM_TASK_GLUCOSE] = { &glucose_task_handle, glucose_update_task,  "glucose_update", GLUCOSE_STACK_SIZE, 5, 0 },
    [CYGM_TASK_WEATHER] = { &weather_task_handle, weather_update_task,  "weather_update", WEATHER_STACK_SIZE, 3, 0 },
    [CYGM_TASK_TIME]    = { &time_task_handle,    time_update_task,     "time_update",    TIME_STACK_SIZE,    3, tskNO_AFFINITY },
    [CYGM_TASK_BATTERY] = { &battery_task_handle, battery_monitor_task, "battery_mon",    BATTERY_STACK_SIZE, 2, 0 },
};

// ensure_tasks_running() is reachable from the WiFi task, the update-check worker
// and the UI watchdog at once, and a plain check-then-create lets two of them see
// a NULL handle and start the task twice — the second create overwrites the handle
// and the orphan runs unkillable until reboot. A spinlock-guarded claim flag
// serialises the check; the create itself must stay outside the critical section.
static portMUX_TYPE s_task_ensure_spin = portMUX_INITIALIZER_UNLOCKED;
static bool s_task_creating[CYGM_TASK_COUNT];

static void cygm_task_ensure(const cygm_task_def_t *def) {
    const int idx = (int)(def - k_task_defs);

    bool claimed;
    portENTER_CRITICAL(&s_task_ensure_spin);
    claimed = (*def->handle == NULL) && !s_task_creating[idx];
    if (claimed) s_task_creating[idx] = true;
    portEXIT_CRITICAL(&s_task_ensure_spin);

    if (!claimed) return;

    BaseType_t rc = xTaskCreatePinnedToCore(def->fn, def->name, def->stack, NULL,
                                            def->prio, def->handle, def->core);

    portENTER_CRITICAL(&s_task_ensure_spin);
    s_task_creating[idx] = false;
    portEXIT_CRITICAL(&s_task_ensure_spin);

    if (rc != pdPASS) {
        *def->handle = NULL;  // Keep the handle clean so the next call retries
        ESP_LOGE(TAG, "Failed to create %s (rc=%d, heap=%lu largest=%u)", def->name, (int)rc,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    } else {
        ESP_LOGI(TAG, "Started %s (stack=%lu prio=%d core=%d)", def->name,
                 (unsigned long)def->stack, (int)def->prio, (int)def->core);
    }
}

void ensure_tasks_running(void) {
    for (int i = 0; i < CYGM_TASK_COUNT; i++) {
        cygm_task_ensure(&k_task_defs[i]);
    }
}

// ==================== Idle dim + tap-to-wake ====================

static bool             display_dimmed = false;
static lv_obj_t        *dim_catcher    = NULL;
static volatile bool    dim_wake_requested = false;

// PRESSED as well as CLICKED: a press that drags off the catcher still means
// the user touched the screen, and it must wake rather than be swallowed.
static void dim_catcher_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED) {
        dim_wake_requested = true;  // Watchdog tick does the LVGL teardown
    }
}

static void dim_wake(bool restore_brightness);

// Drop the backlight to the floor and put a transparent full-screen catcher on
// the top layer so the first touch only wakes the display instead of pressing
// whatever is under the finger. Never entered while an alarm owns the screen.
static void dim_enter(void) {
    if (display_dimmed || visual_alarm_active) return;
    if (!lvgl_port_lock(1)) return;  // Retry on the next tick

    dim_catcher = lv_obj_create(lv_layer_top());
    if (dim_catcher != NULL) {
        lv_obj_remove_style_all(dim_catcher);
        lv_obj_set_size(dim_catcher, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_pos(dim_catcher, 0, 0);
        lv_obj_set_style_bg_opa(dim_catcher, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(dim_catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dim_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dim_catcher, dim_catcher_event_cb, LV_EVENT_ALL, NULL);
    }
    lvgl_port_unlock();

    if (dim_catcher == NULL) {
        ESP_LOGW(TAG, "Idle dim skipped — could not create wake catcher");
        return;
    }

    display_dimmed = true;
    dim_wake_requested = false;
    cst820_take_gesture();  // Discard anything latched before the dim

    uint8_t level = cygm_night_active() ? UI_DIM_LEVEL_NIGHT : UI_DIM_LEVEL_DAY;
    display_set_brightness(level);
    ESP_LOGI(TAG, "Idle dim: backlight %d%%", level);

    // An alarm can force 100% between the entry check and the write above, and
    // nothing re-asserts it later. Hand the display back rather than leaving a
    // takeover sitting at the dim floor.
    if (visual_alarm_active) {
        ESP_LOGW(TAG, "Alarm started during idle dim — restoring full brightness");
        display_set_brightness(100);
        dim_wake(false);
    }
}

// Leave the dim state. restore_brightness is false when an alarm took over —
// the alarm forces 100% and restores the saved level itself, so the wake path
// must not fight it.
static void dim_wake(bool restore_brightness) {
    if (!display_dimmed) return;

    if (dim_catcher != NULL) {
        if (!lvgl_port_lock(1)) return;  // Stay dimmed, retry on the next tick
        lv_obj_del(dim_catcher);
        dim_catcher = NULL;
        lv_disp_trig_activity(NULL);  // A button wake has no touch to reset it
        lvgl_port_unlock();
    }

    display_dimmed = false;
    dim_wake_requested = false;

    if (restore_brightness) {
        uint8_t level = cygm_current_scheduled_brightness();
        display_set_brightness(level);
        ESP_LOGI(TAG, "Wake from dim: backlight %d%%", level);
    }
}

// ==================== Boot button (GPIO0) runtime input ====================
// Strapping pin, active low. Input-only with the internal pull-up, matching the
// deep-sleep wake check in app_main. Nothing here touches the EXT0 wake source,
// which is armed at sleep time by the power-off paths.

static void boot_button_init(void) {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "Boot button armed on GPIO %d (snooze / wake)", (int)BOOT_BUTTON_GPIO);
}

static void boot_button_action(void) {
    if (visual_alarm_active) {
        // Same public-global sequence as the takeover's SNOOZE tap in main.c.
        int snooze_min = alarm_ext_settings.snooze_default_min;
        if (snooze_min < 5 || snooze_min > 120) snooze_min = 30;

        // SAFETY: never snooze an alarm that already cleared — it would
        // suppress the next real one for the whole window.
        if (current_alarm_state != ALARM_STATE_NONE) {
            time_t now;
            time(&now);
            alarm_snooze_until = now + (snooze_min * 60);
            alarm_acknowledged = true;
            ESP_LOGI(TAG, "Boot button: alarm snoozed %d min (glucose=%d)", snooze_min, current_glucose);
            sd_log(TAG, "BUTTON: alarm snoozed %dmin, glucose=%d", snooze_min, current_glucose);
        } else {
            ESP_LOGI(TAG, "Boot button: alert dismissed (no standing alarm)");
            sd_log(TAG, "BUTTON: alert dismissed");
        }

        stop_audio_alarm();
        stop_visual_alarm();

        // Same return-to-home the takeover's SNOOZE tap performs. If the lock is
        // busy, leave home_screen_active false so the watchdog does the return
        // instead of the flags claiming a screen that is not displayed.
        if (screen_home != NULL && lvgl_port_lock(1)) {
            if (lv_scr_act() != screen_home) lv_scr_load(screen_home);
            home_screen_active = (lv_scr_act() == screen_home);
            lvgl_port_unlock();
        }
        pause_background_tasks = false;
        return;
    }

    if (display_dimmed) {
        ESP_LOGI(TAG, "Boot button: wake from dim");
        dim_wake(true);
        return;
    }

    ESP_LOGD(TAG, "Boot button press ignored — nothing to act on");
}

static void boot_button_poll(void) {
    static uint8_t low_ticks = 0;
    static bool    fired     = false;

    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        if (low_ticks < BOOT_BTN_DEBOUNCE_TICKS) low_ticks++;
        if (low_ticks >= BOOT_BTN_DEBOUNCE_TICKS && !fired) {
            fired = true;  // One action per press; re-arms on release
            boot_button_action();
        }
    } else {
        low_ticks = 0;
        fired = false;
    }
}

// ==================== Abandoned sub-screen reclaim ====================

static lv_obj_t *watch_screen    = NULL;
static uint32_t  watch_child_cnt = 0;

// Sub-screens the watchdog may free. Every one is already deleted-and-NULLed by
// some navigation path, so its owner recreates it on demand.
static lv_obj_t **const k_disposable_screens[] = {
    &screen_menu, &screen_wifi_list, &screen_wifi_password, &screen_zipcode_entry,
    &screen_cgm_menu, &screen_dexcom_auth, &screen_dexcom_login,
    &screen_dexcom_username_entry, &screen_dexcom_password_entry, &screen_dexcom_status,
    &screen_libre_auth, &screen_libre_login, &screen_libre_email_entry,
    &screen_libre_password_entry, &screen_libre_status,
    &screen_nightscout_auth, &screen_nightscout_login, &screen_nightscout_url_entry,
    &screen_nightscout_token_entry, &screen_nightscout_status,
    &screen_alarm_settings, &screen_tone_picker, &screen_alarm_preview,
    &screen_alarm_detail_editor, &screen_time_weather_settings,
};

// Remember which screen the user walked away from and its child count on arrival.
// Modal overlays here are children of lv_scr_act() but are freed through a
// module-static pointer, so freeing the screen under a live overlay would leave
// that pointer dangling. An unchanged child count is the evidence none is up.
static void watchdog_sample_screen(void) {
    if (!lvgl_port_lock(1)) return;
    lv_obj_t *act = lv_scr_act();
    if (act != NULL && act != watch_screen) {
        watch_screen = act;
        watch_child_cnt = lv_obj_get_child_cnt(act);
    }
    lvgl_port_unlock();
}

// Load home and free the abandoned sub-screen in one lock — lv_scr_load with no
// animation swaps synchronously, so the old screen can be deleted immediately
// (same pattern as back_btn_event_cb).
static void return_to_home(void) {
    if (screen_home == NULL) return;

    // Resume network work first: a missed LVGL lock must never leave glucose
    // polling paused. The screen swap itself simply retries on the next tick.
    pause_background_tasks = false;

    if (!lvgl_port_lock(1)) return;

    lv_obj_t *abandoned = lv_scr_act();
    lv_scr_load(screen_home);

    if (abandoned != NULL && abandoned != screen_home && abandoned != screen_splash &&
        abandoned == watch_screen && lv_obj_get_child_cnt(abandoned) == watch_child_cnt) {
        const size_t n_disposable = sizeof(k_disposable_screens) / sizeof(k_disposable_screens[0]);
        for (size_t i = 0; i < n_disposable; i++) {
            if (*k_disposable_screens[i] == abandoned) {
                // Some entries are overlays parented to the doomed screen rather
                // than screens of their own, so they die with it — their module
                // statics must be cleared in the same locked section or a later
                // delete runs on freed memory.
                for (size_t j = 0; j < n_disposable; j++) {
                    if (j == i) continue;
                    lv_obj_t *other = *k_disposable_screens[j];
                    if (other != NULL && lv_obj_get_screen(other) == abandoned) {
                        *k_disposable_screens[j] = NULL;
                    }
                }
                lv_obj_del(abandoned);
                *k_disposable_screens[i] = NULL;
                ESP_LOGI(TAG, "Freed abandoned screen (heap=%lu)",
                         (unsigned long)esp_get_free_heap_size());
                break;
            }
        }
    }

    watch_screen = NULL;
    watch_child_cnt = 0;
    lvgl_port_unlock();

    home_screen_active = true;
}

void ui_task_core1(void *pvParameters) {
    ESP_LOGI(TAG, "UI Task started on Core 1");

    add_boot_log("Init touch screen");
    ESP_ERROR_CHECK(touch_init_core1());

    add_boot_log("Init LVGL");
    ESP_ERROR_CHECK(lvgl_init());

    init_styles();

    add_boot_log("Create UI screens");
    create_splash_screen();
    create_home_screen();
    // Menu and WiFi screens are created on demand rather than at boot, to save RAM.

    lv_scr_load(screen_splash);
    add_boot_log("Display ready");

    if (boot_log_label != NULL && lvgl_port_lock(1)) {
        lv_label_set_text(boot_log_label, boot_log_buffer);
        lvgl_port_unlock();
    }

    // Readiness-gated splash: leave as soon as the boot steps that can complete
    // before the home screen exists are done, instead of burning a fixed 5s.
    int64_t splash_start_ms = esp_timer_get_time() / 1000;
    while (1) {
        int64_t elapsed_ms = (esp_timer_get_time() / 1000) - splash_start_ms;
        if (elapsed_ms >= SPLASH_MAX_MS) {
            ESP_LOGW(TAG, "Splash ceiling reached (%d ms) — network still unresolved", SPLASH_MAX_MS);
            break;
        }
        if (boot_net_resolved && elapsed_ms >= SPLASH_MIN_MS) {
            ESP_LOGI(TAG, "Splash complete after %lu ms", (unsigned long)elapsed_ms);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    add_boot_log("Loading home...");

    vTaskDelay(pdMS_TO_TICKS(200));

    // Switch to home screen (no animation — lv_anim_start causes freeze on this hardware).
    // Retried: a single missed lock here would leave the device showing the splash
    // for the rest of the run while home_screen_active below claims otherwise.
    lv_mem_monitor_t splash_mon = {0};
    bool splash_freed = false;
    for (int retry = 0; retry < 50; retry++) {
        if (!lvgl_port_lock(1)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        lv_scr_load(screen_home);

        // The splash is never shown again, and its objects plus the boot log's text
        // buffer would otherwise sit in the 36KB LVGL pool forever. NULL the globals
        // that point into it — add_boot_log() checks them before writing.
        if (screen_splash != NULL) {
            lv_obj_del(screen_splash);
            screen_splash = NULL;
            boot_log_label = NULL;
            boot_progress_bar = NULL;
            lv_mem_monitor(&splash_mon);
            splash_freed = true;
        }
        lvgl_port_unlock();
        break;
    }
    if (splash_freed) {
        ESP_LOGI(TAG, "Splash freed — LVGLPOOL: free=%u biggest=%u frag=%u%%",
                 (unsigned)splash_mon.free_size, (unsigned)splash_mon.free_biggest_size,
                 (unsigned)splash_mon.frag_pct);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    // Mark home screen as active - allow background tasks to run
    home_screen_active = true;
    pause_background_tasks = false;
    ESP_LOGI(TAG, "Home screen loaded - background tasks enabled");

    // Show legal disclaimer overlay on boot (unless user has accepted permanently)
    if (!nvs_get_disclaimer_accepted()) {
        if (lvgl_port_lock(1)) {
            show_disclaimer_overlay();
            lvgl_port_unlock();
        }
    }

    // Stop any boot LED activity (WiFi blink or error pulse) when home screen loads
    led_off();

    // Start battery monitoring after home screen is loaded. Only this task —
    // the network tasks are created after auth so their stacks don't fragment
    // the heap during the TLS handshake.
    ESP_LOGI(TAG, "Starting battery monitor task");
    cygm_task_ensure(&k_task_defs[CYGM_TASK_BATTERY]);

    boot_button_init();

    // Global watchdog: inactivity return-to-home, idle dim + tap-to-wake, and the
    // boot-button input. Ticks fast enough to debounce the button, while the
    // inactivity work runs once a second.
    int tick = 0;
    uint32_t lvgl_pool_secs = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_TICK_MS));

        // An alarm owns the display: drop the wake catcher at once so it can
        // never swallow a SNOOZE/DISMISS press, and leave brightness to the
        // alarm's own force-100%/restore.
        if (visual_alarm_active) {
            if (display_dimmed) {
                dim_wake(false);
                // Re-assert the alarm's forced level: dim_enter may have written
                // the dim floor after start_visual_alarm forced 100%. Idempotent
                // whichever writer won, and bounds a dark alarm to one tick.
                display_set_brightness(100);
            }
        } else if (dim_wake_requested) {
            dim_wake(true);
        }

        boot_button_poll();

        if (++tick < WATCHDOG_TICKS_PER_SEC) continue;
        tick = 0;

        uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);

        // A remote driving session leaves long gaps between injected taps, which
        // reads as an abandoned screen. Hold suppresses the navigation the session
        // did not ask for; alarms above are untouched and still take the display.
        bool ui_held = cygm_ui_hold_active();

        if (!home_screen_active) {
            watchdog_sample_screen();
            if (inactive_ms >= UI_INACTIVITY_RETURN_MS && !ui_held) {
                ESP_LOGI(TAG, "UI inactivity timeout (%lu ms) - returning to home", (unsigned long)inactive_ms);
                return_to_home();
            }
        }

        // On the charger the display stays on unless the user opted into
        // dimming (brightness modal). Charger detection is trend-based, so a
        // plug-in confirms within a battery-task cycle — also un-dims here.
        bool charger_holds_wake = !dim_while_charging && battery_is_charging();

        if (display_dimmed) {
            if (charger_holds_wake) {
                ESP_LOGI(TAG, "Charger connected — waking display");
                dim_wake(true);
            } else if (cst820_take_gesture() == CST820_GESTURE_DOUBLE_TAP) {
                ESP_LOGI(TAG, "Double-tap gesture — waking display");
                dim_wake(true);
            }
        } else if (!visual_alarm_active && inactive_ms >= UI_DIM_TIMEOUT_MS &&
                   !ui_held && !charger_holds_wake) {
            dim_enter();
        }

        // LVGL pool sample for the leak hunt. Try-lock only, so a busy port just
        // leaves the counter tripped for the next second and adds no lock pressure.
        if (++lvgl_pool_secs >= LVGL_POOL_LOG_SEC) {
            if (lvgl_port_lock(1)) {
                lv_mem_monitor_t mon;
                lv_mem_monitor(&mon);
                lvgl_port_unlock();
                lvgl_pool_secs = 0;
                ESP_LOGI(TAG, "LVGLPOOL: free=%u biggest=%u frag=%u%%",
                         (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
                         (unsigned)mon.frag_pct);
            }
        }
    }
}

void wifi_task_core0(void *pvParameters) {
    ESP_LOGI(TAG, "WiFi Task started on Core 0");

    add_boot_log("Init WiFi");
    ESP_ERROR_CHECK(wifi_manager_init());

    // Check for saved credentials
    uint8_t saved_count = nvs_get_saved_wifi_count();

    if (saved_count == 0) {
        ESP_LOGI(TAG, "No saved WiFi credentials - setup required");
        add_boot_log("WiFi: Setup needed");
        led_start_wifi_boot_blink();  // Blue blink = setup needed
        boot_net_resolved = true;     // Nothing left to wait for — release the splash

        // Update WiFi status icon - retry up to 500ms during boot
        if (label_wifi_status != NULL) {
            for (int retry = 0; retry < 50; retry++) {
                if (lvgl_port_lock(1)) {
                    // NO lv_label_set_recolor — it can freeze this hardware.
                    // Color the whole label red to flag the WiFi problem.
                    lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " " LV_SYMBOL_WARNING);
                    lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
                    lvgl_port_unlock();
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    } else {
        // Smart WiFi connection: Try last connected network first, then fall back to others
        ESP_LOGI(TAG, "Found %d saved WiFi network(s)", saved_count);

        wifi_credentials_t networks[MAX_SAVED_WIFI_NETWORKS];
        uint8_t network_count = 0;
        nvs_get_saved_wifi_networks(networks, &network_count);

        char last_ssid[MAX_SSID_LEN] = {0};
        bool has_last_wifi = (nvs_load_last_wifi_ssid(last_ssid, sizeof(last_ssid)) == ESP_OK);

        // Build connection order: last connected first, then others
        int connection_order[MAX_SAVED_WIFI_NETWORKS];
        int order_count = 0;

        if (has_last_wifi) {
            for (int i = 0; i < network_count; i++) {
                if (strcmp(networks[i].ssid, last_ssid) == 0) {
                    connection_order[order_count++] = i;
                    ESP_LOGI(TAG, "Last connected WiFi found: %s (will try first)", last_ssid);
                    break;
                }
            }
        }

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
            led_start_wifi_boot_blink();  // Blue blink = connecting

            if (wifi_manager_connect_to(networks[idx].ssid, networks[idx].password) == ESP_OK) {
                // The connected SSID can differ from the attempted one after a fallback.
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
            boot_net_resolved = true;  // Release the splash before SNTP + auth
            ESP_LOGI(TAG, "WiFi connected successfully");
            sd_log(TAG, "WiFi connected");
            add_boot_log("WiFi connected!");
            wifi_connected = true;
            led_start_wifi_boot_blink();  // Start green blink during boot

            // Update WiFi status icon - retry up to 500ms during boot
            // (LVGL port task may be busy with splash→home transition)
            if (label_wifi_status != NULL) {
                for (int retry = 0; retry < 50; retry++) {
                    if (lvgl_port_lock(1)) {
                        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI);
                        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_GREEN), 0);
                        lvgl_port_unlock();
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            // SD card logger will be initialized after time sync (needs correct timestamp for filename)

            initialize_sntp();

            // Start time update task (idempotent, canonical parameters)
            cygm_task_ensure(&k_task_defs[CYGM_TASK_TIME]);

            // Determine active CGM provider
            char cgm_type[MAX_CGM_TYPE_LEN] = "dexcom";
            nvs_load_cgm_type(cgm_type, sizeof(cgm_type));
            cgm_provider_t boot_provider = CGM_PROVIDER_DEXCOM;
            if (strcmp(cgm_type, "libre") == 0) boot_provider = CGM_PROVIDER_LIBRE;
            else if (strcmp(cgm_type, "nightscout") == 0) boot_provider = CGM_PROVIDER_NIGHTSCOUT;

            // Check for saved credentials (dispatch by provider)
            bool has_cgm_creds;
            switch (boot_provider) {
                case CGM_PROVIDER_NIGHTSCOUT: has_cgm_creds = nvs_has_nightscout_credentials(); break;
                case CGM_PROVIDER_LIBRE:      has_cgm_creds = nvs_has_libre_credentials(); break;
                default:                      has_cgm_creds = nvs_has_dexcom_credentials(); break;
            }

            if (has_cgm_creds) {
                ESP_LOGI(TAG, "Found saved %s credentials, will attempt auto-login after time sync", cgm_type);

                if (boot_provider == CGM_PROVIDER_NIGHTSCOUT) {
                    nvs_get_nightscout_credentials(nightscout_url_buf, sizeof(nightscout_url_buf),
                                                    nightscout_token_buf, sizeof(nightscout_token_buf));
                } else if (boot_provider == CGM_PROVIDER_LIBRE) {
                    nvs_get_libre_credentials(libre_email_buf, sizeof(libre_email_buf),
                                               libre_password_buf, sizeof(libre_password_buf));
                } else {
                    char username[64], password[64];
                    if (nvs_get_dexcom_credentials(username, sizeof(username), password, sizeof(password)) == ESP_OK) {
                        strncpy(dexcom_username_buf, username, sizeof(dexcom_username_buf) - 1);
                        strncpy(dexcom_password_buf, password, sizeof(dexcom_password_buf) - 1);
                    }
                }

                // SSL certificate validation needs an accurate clock — wait for SNTP.
                ESP_LOGI(TAG, "Waiting for SNTP time sync before %s auth (required for SSL)...", cgm_type);

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

                    // Start tasks anyway — glucose handles not-yet-authenticated state
                    ensure_tasks_running();
                } else {
                    ESP_LOGI(TAG, "Time synced successfully, proceeding with %s auto-login", cgm_type);

                    int max_retries = 5;
                    int retry_delay_ms = 2000;

                    // Libre: try restoring saved session before full login
                    bool session_restored = false;
                    if (boot_provider == CGM_PROVIDER_LIBRE) {
                        if (libre_restore_session() == ESP_OK) {
                            ESP_LOGI(TAG, "Libre session restored from NVS — skipping login");
                            sd_log(TAG, "Libre session restored (skip login)");
                            session_restored = true;

                            // Wait for home screen before fetching
                            for (int w = 0; w < 100 && !home_screen_active; w++) {
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }

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
                                    if (esp_reset_reason() == ESP_RST_POWERON) {
                                        play_success_sound();
                                    }
                                }

                                check_glucose_alarms(current_glucose);
                                update_glucose_display();
                            } else {
                                glucose_data_valid = false;
                                glucose_status = glucose.status;
                                update_glucose_display();
                            }

                            // Close SSL before creating tasks — task stacks will fragment
                            // the heap, making TLS read buffers fail on the first cycle.
                            // Every provider's first glucose cycle does delete→handshake→fetch.
                            libre_close_persistent_client();
                            ESP_LOGI(TAG, "SSL closed after Libre session restore (heap=%lu)", esp_get_free_heap_size());

                            ensure_tasks_running();
                        } else {
                            ESP_LOGI(TAG, "Libre session restore failed — will do full login");
                        }
                    }

                    for (int attempt = 1; attempt <= max_retries && !session_restored; attempt++) {
                        ESP_LOGI(TAG, "%s auth attempt %d/%d...", cgm_type, attempt, max_retries);

                        esp_err_t ret;
                        if (boot_provider == CGM_PROVIDER_NIGHTSCOUT) {
                            ret = nightscout_authenticate(nightscout_url_buf, nightscout_token_buf);
                        } else if (boot_provider == CGM_PROVIDER_LIBRE) {
                            ret = libre_authenticate(libre_email_buf, libre_password_buf);
                        } else {
                            ret = dexcom_authenticate(dexcom_username_buf, dexcom_password_buf);
                        }

                        if (ret == ESP_OK) {
                            sd_log(TAG, "%s auto-login OK (attempt %d)", cgm_type, attempt);
                            ESP_LOGI(TAG, "%s auto-login successful on attempt %d!", cgm_type, attempt);

                            // Wait for home screen before fetching (UI task loads it on Core 1)
                            for (int w = 0; w < 100 && !home_screen_active; w++) {
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }

                            cgm_glucose_t glucose = {0};
                            esp_err_t fetch_ret;
                            if (boot_provider == CGM_PROVIDER_NIGHTSCOUT) {
                                fetch_ret = nightscout_fetch_glucose(&glucose);
                            } else if (boot_provider == CGM_PROVIDER_LIBRE) {
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
                                    if (esp_reset_reason() == ESP_RST_POWERON) {
                                        ESP_LOGI(TAG, "First glucose reading - playing success sound");
                                        play_success_sound();
                                    }
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
                            // No other network tasks running yet. Safe without mutex.
                            heartbeat_send();

                            // Close SSL before creating tasks — their stacks fragment
                            // the heap and the TLS read buffer then fails on cycle one.
                            if (boot_provider == CGM_PROVIDER_NIGHTSCOUT) {
                                nightscout_close_persistent_client();
                            } else if (boot_provider == CGM_PROVIDER_LIBRE) {
                                libre_close_persistent_client();
                            } else {
                                dexcom_close_persistent_client();
                            }
                            ESP_LOGI(TAG, "SSL closed after initial fetch (heap=%lu)", esp_get_free_heap_size());

                            // Start background tasks AFTER auth: their stacks interleave
                            // with the SSL region, so freeing both on reconnect merges
                            // into one large block. Creating them first costs ~8KB
                            // during the critical handshake.
                            ensure_tasks_running();

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

                    // Ensure tasks start even if auth failed
                    ensure_tasks_running();
                }
            } else {
                // No CGM credentials — start weather only
                ESP_LOGI(TAG, "No %s credentials found", cgm_type);
                cygm_task_ensure(&k_task_defs[CYGM_TASK_WEATHER]);
            }
        } else {
            boot_net_resolved = true;  // Outcome is known — release the splash
            sd_log(TAG, "WiFi connection failed");
            ESP_LOGE(TAG, "WiFi connection failed");
            led_start_error_blink();  // Red blink for error

            // Update WiFi status on home screen - retry up to 500ms
            if (label_wifi_status != NULL) {
                for (int retry = 0; retry < 50; retry++) {
                    if (lvgl_port_lock(1)) {
                        // NO lv_label_set_recolor — color whole label red instead.
                        lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " " LV_SYMBOL_WARNING);
                        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_RED), 0);
                        lvgl_port_unlock();
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
    }

    // Monitor WiFi connection and auto-reconnect.
    // SAFETY: WiFi reconnection is NEVER paused regardless of active screen.
    // Without WiFi there is no glucose data, which is life-critical.
    bool was_connected = wifi_connected;
    bool ever_connected = wifi_connected;  // Only show overlay after a prior successful connection
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
                ever_connected = true;
                sd_log(TAG, "WiFi auto-reconnect successful");
                ESP_LOGI(TAG, "Auto-reconnect successful");
                update_wifi_status_display(true);

                // Dismiss WiFi overlay if it was shown
                if (wifi_overlay_shown) {
                    for (int retry = 0; retry < 50; retry++) {
                        if (lvgl_port_lock(1)) {
                            dismiss_wifi_disconnected_overlay();
                            lvgl_port_unlock();
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    wifi_overlay_shown = false;
                }

                // Re-initialize SNTP in case time drifted
                initialize_sntp();

                // Create tasks if initial WiFi connection failed (tasks are only
                // created inside the connected path during boot)
                ensure_tasks_running();
            } else if (!wifi_overlay_shown && home_screen_active && ever_connected) {
                // Reconnect failed after a prior success — show the overlay. It stays
                // hidden on first boot, when WiFi was never connected.
                for (int retry = 0; retry < 50; retry++) {
                    if (lvgl_port_lock(1)) {
                        show_wifi_disconnected_overlay();
                        lvgl_port_unlock();
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                wifi_overlay_shown = true;
            }
        } else {
            if (!was_connected) {
                // Connection restored externally (e.g. event handler retry succeeded)
                was_connected = true;
                ever_connected = true;
                update_wifi_status_display(true);

                if (wifi_overlay_shown) {
                    for (int retry = 0; retry < 50; retry++) {
                        if (lvgl_port_lock(1)) {
                            dismiss_wifi_disconnected_overlay();
                            lvgl_port_unlock();
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    wifi_overlay_shown = false;
                }
            }

            // Reset overlay flag when connected (so it can show again on next disconnect)
            wifi_overlay_shown = false;
        }
    }
}
