# LVGL Thread-Safety Patches
**Date:** 2026-02-18  
**Priority:** Critical → High → Medium  
**Estimated Time:** 6-9 hours total

---

## 🔴 CRITICAL PRIORITY PATCHES (1-2 hours)

### Patch 1: Fix WiFi status lock timeouts

**File:** `main/tasks/background_tasks.c`  
**Lines:** 168-172, 210-216, 238-244  
**Risk:** WiFi status display inconsistent (silent lock failures)

```c
// ============== BEFORE ==============
// Line 168-172 (no credentials case)
if (label_wifi_status != NULL && lvgl_port_lock(0)) {
    lv_label_set_recolor(label_wifi_status, true);
    lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
    lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
    lvgl_port_unlock();
}

// ============== AFTER ==============
// FIXED: Increased timeout to 100ms with retry logic
if (label_wifi_status != NULL) {
    int retries = 0;
    bool updated = false;
    while (retries < 3 && !updated) {
        if (lvgl_port_lock(100)) {  // 100ms timeout instead of 0
            if (label_wifi_status != NULL) {  // Re-check under lock (TOCTOU fix)
                lv_label_set_recolor(label_wifi_status, true);
                lv_label_set_text(label_wifi_status, LV_SYMBOL_WIFI " #FF0000 " LV_SYMBOL_WARNING "#");
                lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(COLOR_TEXT_DIM), 0);
                updated = true;
            }
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "WiFi status update retry %d/3", ++retries);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (!updated) {
        ESP_LOGE(TAG, "Failed to update WiFi status icon after 3 retries");
    }
}
```

**Apply same pattern to:**
- Line 210-216 (connection success)
- Line 238-244 (connection failed)

---

### Patch 2: Fix screen transition lock timeouts

**File:** `main/tasks/background_tasks.c`  
**Lines:** 62-65, 103-106  
**Risk:** User stuck on splash screen if LVGL busy

```c
// ============== BEFORE ==============
// Line 62-65 (home screen transition)
if (lvgl_port_lock(0)) {
    lv_scr_load_anim(screen_home, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
    lvgl_port_unlock();
}

// ============== AFTER ==============
// FIXED: Longer timeout + fallback + error handling
int max_retries = 5;
int retry_delay_ms = 100;
bool transitioned = false;

for (int i = 0; i < max_retries; i++) {
    if (lvgl_port_lock(500)) {  // 500ms for animation
        lv_scr_load_anim(screen_home, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
        lvgl_port_unlock();
        transitioned = true;
        ESP_LOGI(TAG, "Home screen transition successful");
        break;
    }
    ESP_LOGW(TAG, "Screen transition retry %d/%d (LVGL busy)", i+1, max_retries);
    vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    retry_delay_ms *= 2;  // Exponential backoff: 100, 200, 400, 800ms
}

if (!transitioned) {
    ESP_LOGE(TAG, "CRITICAL: Failed screen transition after %d retries! Trying instant load...", max_retries);
    // Fallback: Try without animation
    if (lvgl_port_lock(100)) {
        lv_scr_load(screen_home);  // Instant load as emergency fallback
        lvgl_port_unlock();
        ESP_LOGW(TAG, "Emergency fallback: Instant screen load succeeded");
    } else {
        ESP_LOGE(TAG, "CRITICAL: Even fallback screen load failed! UI may be frozen!");
    }
}
```

**Apply same pattern to:**
- Line 103-106 (inactivity watchdog screen transition)

---

### Patch 3: Fix boot log TOCTOU race

**File:** `main/tasks/background_tasks.c`  
**Lines:** 56-60  
**Risk:** Crash if boot_log_label becomes NULL between check and use

```c
// ============== BEFORE ==============
// Line 56-60
if (boot_log_label != NULL && lvgl_port_lock(0)) {
    lv_label_set_text(boot_log_label, boot_log_buffer);
    lvgl_port_unlock();
}

// ============== AFTER ==============
// FIXED: Lock first, then check object validity (prevents TOCTOU)
if (lvgl_port_lock(100)) {  // Lock before checking object
    if (boot_log_label != NULL) {  // Safe check under lock
        lv_label_set_text(boot_log_label, boot_log_buffer);
    } else {
        ESP_LOGW(TAG, "Boot log label not created yet, skipping update");
    }
    lvgl_port_unlock();
} else {
    ESP_LOGW(TAG, "Failed to lock LVGL for boot log update");
}
```

---

### Patch 4: Add brightness state mutex

**File:** `main/shared_state.h`  
**Add after line with brightness declarations**

```c
// ============== ADD TO HEADER ==============
// In shared_state.h, add after brightness declarations:
extern SemaphoreHandle_t brightness_mutex;  // Protects screen_brightness_percent
```

**File:** `main/main.c`  
**Add in initialization section (around line with other mutex inits)**

```c
// ============== ADD TO MAIN.C ==============
// In app_main(), create brightness mutex:
brightness_mutex = xSemaphoreCreateMutex();
if (brightness_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create brightness mutex!");
    abort();
}
```

**File:** `main/ui/home_screen.c`  
**Lines:** 130-134  
**Risk:** Race condition on global brightness + blocking hardware access in UI thread

```c
// ============== BEFORE ==============
// Line 130-134 (brightness_slider_event_cb)
static void brightness_slider_event_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    screen_brightness_percent = (uint8_t)lv_slider_get_value(slider);

    if (brightness_value_label != NULL) {
        lv_label_set_text_fmt(brightness_value_label, "%d%%", screen_brightness_percent);
    }

    display_set_brightness(screen_brightness_percent);  // BLOCKING HARDWARE ACCESS!
}

// ============== AFTER ==============
// FIXED: Defer hardware update to background task
static void brightness_slider_event_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t new_brightness = (uint8_t)lv_slider_get_value(slider);

    // Update label immediately (safe in event callback)
    if (brightness_value_label != NULL) {
        lv_label_set_text_fmt(brightness_value_label, "%d%%", new_brightness);
    }

    // Defer hardware update to avoid blocking LVGL
    // Use TaskNotify instead of creating new task (more efficient)
    static TaskHandle_t brightness_task_handle = NULL;
    if (brightness_task_handle == NULL) {
        xTaskCreate(brightness_apply_task, "brightness", 2048, NULL, 1, &brightness_task_handle);
    }
    xTaskNotify(brightness_task_handle, new_brightness, eSetValueWithOverwrite);
}

// NEW FUNCTION: Add to home_screen.c
static void brightness_apply_task(void *pvParameters) {
    uint32_t brightness;
    
    while (1) {
        // Wait for notification from slider event
        if (xTaskNotifyWait(0, 0, &brightness, portMAX_DELAY) == pdTRUE) {
            // Protect global variable with mutex
            if (xSemaphoreTake(brightness_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                screen_brightness_percent = (uint8_t)brightness;
                display_set_brightness(screen_brightness_percent);  // Safe outside LVGL context
                xSemaphoreGive(brightness_mutex);
                ESP_LOGI(TAG, "Brightness applied: %d%%", (int)brightness);
            } else {
                ESP_LOGW(TAG, "Failed to acquire brightness mutex");
            }
        }
    }
}
```

---

### Patch 5: Fix Dexcom login task screen transition

**File:** `main/ui/cgm_screens.c`  
**Lines:** 48-71  
**Risk:** User stuck on login screen if LVGL busy during authentication

```c
// ============== BEFORE ==============
// Line 66-71 (inside dexcom_login_task)
if (lvgl_port_lock(100)) {
    lv_scr_load(screen_home);
    home_screen_active = true;
    pause_background_tasks = false;
    lvgl_port_unlock();
} else {
    ESP_LOGW(TAG, "Failed to lock LVGL to return to home screen after login");
}

// ============== AFTER ==============
// FIXED: Retry logic with exponential backoff
if (dexcom_authenticate(username, password) == ESP_OK) {
    ESP_LOGI(TAG, "Dexcom login successful, transitioning to home screen");
    
    // Retry screen transition with backoff
    int max_retries = 5;
    int retry_delay_ms = 100;
    bool transitioned = false;
    
    for (int i = 0; i < max_retries; i++) {
        if (lvgl_port_lock(retry_delay_ms)) {
            lv_scr_load(screen_home);
            home_screen_active = true;
            pause_background_tasks = false;
            lvgl_port_unlock();
            transitioned = true;
            ESP_LOGI(TAG, "Screen transition successful after %d attempt(s)", i+1);
            break;
        }
        ESP_LOGW(TAG, "Screen transition retry %d/%d (LVGL busy)", i+1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        retry_delay_ms *= 2;  // Exponential backoff
    }
    
    if (!transitioned) {
        ESP_LOGE(TAG, "CRITICAL: Failed to transition to home screen after %d retries!", max_retries);
        ESP_LOGE(TAG, "User is stuck on login screen despite successful authentication");
        // TODO: Show error message on login screen or force UI refresh
    }
    
    // ... rest of success logic ...
}
```

---

## 🟡 HIGH PRIORITY PATCHES (2-3 hours)

### Patch 6: Remove unnecessary locks from timer callbacks

**File:** `main/ui/home_screen.c`  
**Lines:** 30-43  
**Risk:** Performance overhead (lock/unlock every second for no reason)

```c
// ============== BEFORE ==============
// Line 30-43
static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_fetch_timer_bar == NULL) {
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;
    int64_t elapsed = now - last_glucose_fetch_time;
    const int64_t FETCH_INTERVAL_MS = 90000;

    int percentage = (int)((elapsed * 100) / FETCH_INTERVAL_MS);
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    if (lvgl_port_lock(0)) {  // UNNECESSARY - timers run in LVGL context
        lv_bar_set_value(glucose_fetch_timer_bar, percentage, LV_ANIM_OFF);
        lvgl_port_unlock();
    }
}

// ============== AFTER ==============
// FIXED: Remove lock (timers run in LVGL task context)
/**
 * @brief Update glucose fetch progress bar
 * 
 * THREAD SAFETY: LVGL timers run in LVGL task context (Core 1),
 * so no locking is needed. This callback is called by LVGL's
 * internal timer handler.
 */
static void glucose_timer_update_cb(lv_timer_t *timer) {
    if (!home_screen_active || glucose_fetch_timer_bar == NULL) {
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;
    int64_t elapsed = now - last_glucose_fetch_time;
    const int64_t FETCH_INTERVAL_MS = 90000;

    int percentage = (int)((elapsed * 100) / FETCH_INTERVAL_MS);
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    // NO LOCK NEEDED - already in LVGL context
    lv_bar_set_value(glucose_fetch_timer_bar, percentage, LV_ANIM_OFF);
}
```

**Apply same pattern to:**
- All other `lv_timer_t` callbacks that currently use `lvgl_port_lock()`

---

### Patch 7: Fix inactivity watchdog lock

**File:** `main/tasks/background_tasks.c`  
**Lines:** 114-127  
**Risk:** Rare false positives if reading stale display inactive time

```c
// ============== BEFORE ==============
// Line 116-123
uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
if (inactive_ms >= UI_INACTIVITY_RETURN_MS) {
    ESP_LOGI(TAG, "UI inactivity timeout - returning to home");
    if (lvgl_port_lock(0)) {
        lv_scr_load(screen_home);
        lvgl_port_unlock();
    }
    home_screen_active = true;
    pause_background_tasks = false;
}

// ============== AFTER ==============
// FIXED: Lock around inactive time check + screen transition
bool should_return_home = false;
uint32_t inactive_ms = 0;

// Read LVGL state under lock
if (lvgl_port_lock(10)) {
    inactive_ms = lv_disp_get_inactive_time(NULL);
    should_return_home = (inactive_ms >= UI_INACTIVITY_RETURN_MS);
    lvgl_port_unlock();
}

if (should_return_home) {
    ESP_LOGI(TAG, "UI inactivity timeout (%lu ms) - returning to home", (unsigned long)inactive_ms);
    
    // Screen transition with retry
    if (lvgl_port_lock(100)) {
        lv_scr_load(screen_home);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "Failed to lock LVGL for inactivity screen transition");
    }
    
    home_screen_active = true;
    pause_background_tasks = false;
}
```

---

### Patch 8: Add defensive assertions to lock-required functions

**File:** `main/ui/glucose_chart.c`  
**Lines:** 32-35  
**Risk:** Function called without lock would crash silently

```c
// ============== BEFORE ==============
// Line 32-35
void create_glucose_chart_overlay(void) {
    if (chart_overlay != NULL) {
        ESP_LOGW(TAG, "Chart overlay already open");
        return;
    }

// ============== AFTER ==============
/**
 * @brief Create glucose chart overlay
 * 
 * THREAD SAFETY: This function MUST be called with lvgl_port_lock() held.
 * It does NOT acquire the lock internally.
 * 
 * @warning Calling without lock will trigger assertion in debug builds
 * 
 * Typical usage:
 *   if (lvgl_port_lock(100)) {
 *       create_glucose_chart_overlay();
 *       lvgl_port_unlock();
 *   }
 */
void create_glucose_chart_overlay(void) {
    // Defensive assertion: verify LVGL lock is held
    #ifndef NDEBUG
    // Try to acquire lock with 0 timeout - should fail if already locked
    bool lock_already_held = (lvgl_port_lock(0) == false);
    if (!lock_already_held) {
        // Lock succeeded - this is BAD! Function requires lock to be pre-held
        lvgl_port_unlock();  // Clean up the lock we just acquired
        ESP_LOGE(TAG, "ASSERTION FAILED: create_glucose_chart_overlay() called without LVGL lock!");
        assert(false);  // Trigger assertion failure
    }
    #endif
    
    if (chart_overlay != NULL) {
        ESP_LOGW(TAG, "Chart overlay already open");
        return;
    }
    
    // ... rest of function ...
}
```

**Apply same pattern to:**
- All screen creation functions: `create_dexcom_auth_screen()`, `create_wifi_list_screen()`, etc.

---

### Patch 9: Add state mutexes for global variables

**File:** `main/shared_state.h`  
**Add mutex declarations**

```c
// ============== ADD TO HEADER ==============
// After existing mutex declarations, add:

// State protection mutexes
extern SemaphoreHandle_t glucose_state_mutex;  // Protects current_glucose, glucose_data_valid, etc.
extern SemaphoreHandle_t wifi_state_mutex;     // Protects wifi_connected, wifi_network_count, etc.
extern SemaphoreHandle_t weather_state_mutex;  // Protects current_temp_f, weather_code, etc.
```

**File:** `main/main.c`  
**Add mutex initialization in `app_main()`**

```c
// ============== ADD TO MAIN.C ==============
// In app_main(), after network_mutex creation:

// Create state mutexes
glucose_state_mutex = xSemaphoreCreateMutex();
wifi_state_mutex = xSemaphoreCreateMutex();
weather_state_mutex = xSemaphoreCreateMutex();

if (glucose_state_mutex == NULL || wifi_state_mutex == NULL || weather_state_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create state mutexes!");
    abort();
}
```

**Example usage in glucose update task:**

```c
// ============== EXAMPLE: Protect glucose state ==============
// In glucose_update_task() after fetching data:

if (dexcom_fetch_glucose(&glucose) == ESP_OK && glucose.valid) {
    // Protect global state with mutex
    if (xSemaphoreTake(glucose_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        current_glucose = glucose.value;
        current_trend = glucose.trend;
        glucose_timestamp = glucose.timestamp;
        glucose_data_valid = true;
        glucose_status = glucose.status;
        last_glucose_fetch_time = esp_timer_get_time() / 1000;
        xSemaphoreGive(glucose_state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire glucose state mutex");
    }
    
    // Update UI with lock
    update_glucose_display();
}
```

---

### Patch 10: Fix screen delete-recreate pattern

**File:** `main/ui/cgm_screens.c`  
**Lines:** 102-110  
**Risk:** Potential recursive deletion if child objects trigger events

```c
// ============== BEFORE ==============
// Line 102-110
static void dexcom_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(0)) {
            if (screen_dexcom_auth != NULL) {
                lv_obj_del(screen_dexcom_auth);  // Immediate deletion
                screen_dexcom_auth = NULL;
            }
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            lvgl_port_unlock();
        }
    }
}

// ============== AFTER ==============
// FIXED: Deferred deletion to avoid recursion
static void dexcom_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (lvgl_port_lock(100)) {  // Increased timeout
            // Hide old screen first
            if (screen_dexcom_auth != NULL) {
                lv_obj_add_flag(screen_dexcom_auth, LV_OBJ_FLAG_HIDDEN);
            }
            
            // Create new screen
            lv_obj_t *old_screen = screen_dexcom_auth;
            screen_dexcom_auth = NULL;  // Clear pointer before deletion
            create_dexcom_auth_screen();
            lv_scr_load(screen_dexcom_auth);
            
            // Delete old screen asynchronously (after screen transition completes)
            if (old_screen != NULL) {
                lv_obj_del_async(old_screen);  // Deferred deletion
            }
            
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "Failed to lock LVGL for screen navigation");
        }
    }
}
```

**Apply same pattern to:**
- All screen navigation event callbacks that delete-then-recreate screens

---

## 🟢 MEDIUM PRIORITY PATCHES (3-4 hours)

### Patch 11: Standardize timeout constants

**File:** `main/shared_state.h`  
**Add timeout constants**

```c
// ============== ADD TO HEADER ==============
// LVGL lock timeout constants (milliseconds)
#define LVGL_LOCK_TIMEOUT_CRITICAL  500  // Screen transitions, animations
#define LVGL_LOCK_TIMEOUT_NORMAL    100  // UI updates, status changes
#define LVGL_LOCK_TIMEOUT_QUICK      10  // Non-critical updates
#define LVGL_LOCK_TIMEOUT_POLL        0  // Lock availability check (assertions)

// Lock retry constants
#define LVGL_LOCK_MAX_RETRIES        5   // Maximum retry attempts
#define LVGL_LOCK_RETRY_DELAY_MS    50   // Initial retry delay
```

**Usage example:**

```c
// Replace all hardcoded timeouts with constants:
if (lvgl_port_lock(100)) { ... }  // OLD
if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_NORMAL)) { ... }  // NEW

if (lvgl_port_lock(500)) { ... }  // OLD
if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_CRITICAL)) { ... }  // NEW
```

---

### Patch 12: Add error recovery for critical screen transitions

**File:** `main/tasks/background_tasks.c`  
**Add fallback mechanism for failed home screen load**

```c
// ============== ADD HELPER FUNCTION ==============
/**
 * @brief Emergency screen recovery - force load home screen
 * 
 * This function bypasses normal screen transitions and forces
 * an immediate load of the home screen. Use only when normal
 * transitions fail repeatedly.
 */
static void emergency_load_home_screen(void) {
    ESP_LOGE(TAG, "EMERGENCY: Forcing home screen load");
    
    // Disable animations temporarily
    uint32_t old_anim_time = lv_disp_get_default()->driver->anim_time;
    lv_disp_get_default()->driver->anim_time = 0;
    
    // Try immediate load with extended timeout
    if (lvgl_port_lock(1000)) {  // 1 second timeout
        lv_scr_load(screen_home);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "Emergency screen load succeeded");
    } else {
        ESP_LOGE(TAG, "CRITICAL: Even emergency screen load failed! UI may be frozen!");
        // TODO: Trigger watchdog reset or safe mode
    }
    
    // Restore animation time
    lv_disp_get_default()->driver->anim_time = old_anim_time;
}
```

---

### Patch 13: Add runtime lock statistics

**File:** `main/shared_state.h`  
**Add lock statistics structure**

```c
// ============== ADD TO HEADER ==============
// LVGL lock statistics (for debugging)
typedef struct {
    uint32_t lock_attempts;       // Total lock attempts
    uint32_t lock_successes;      // Successful locks
    uint32_t lock_failures;       // Failed locks (timeout)
    uint32_t lock_retries;        // Total retry count
    uint32_t max_retry_count;     // Maximum retries for single lock
    uint64_t total_wait_time_us;  // Total time spent waiting
    uint64_t max_wait_time_us;    // Longest single wait
} lvgl_lock_stats_t;

extern lvgl_lock_stats_t lvgl_lock_stats;
```

**File:** Create new `main/lvgl_lock_wrapper.c`

```c
// ============== NEW FILE ==============
#include "lvgl_lock_wrapper.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "LVGL_LOCK";
lvgl_lock_stats_t lvgl_lock_stats = {0};

/**
 * @brief Enhanced LVGL lock with statistics and retry logic
 * 
 * @param timeout_ms Lock timeout in milliseconds
 * @param max_retries Maximum retry attempts (0 = no retries)
 * @param retry_delay_ms Delay between retries
 * @return true if lock acquired, false on failure
 */
bool lvgl_port_lock_ex(uint32_t timeout_ms, uint8_t max_retries, uint32_t retry_delay_ms) {
    uint64_t start_time = esp_timer_get_time();
    uint8_t retries = 0;
    bool locked = false;
    
    lvgl_lock_stats.lock_attempts++;
    
    while (retries <= max_retries && !locked) {
        if (lvgl_port_lock(timeout_ms)) {
            locked = true;
            lvgl_lock_stats.lock_successes++;
            
            // Track wait time
            uint64_t wait_time = esp_timer_get_time() - start_time;
            lvgl_lock_stats.total_wait_time_us += wait_time;
            if (wait_time > lvgl_lock_stats.max_wait_time_us) {
                lvgl_lock_stats.max_wait_time_us = wait_time;
            }
            
            if (retries > 0) {
                ESP_LOGD(TAG, "Lock acquired after %d retries", retries);
                lvgl_lock_stats.lock_retries += retries;
                if (retries > lvgl_lock_stats.max_retry_count) {
                    lvgl_lock_stats.max_retry_count = retries;
                }
            }
        } else {
            retries++;
            if (retries <= max_retries) {
                vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            }
        }
    }
    
    if (!locked) {
        lvgl_lock_stats.lock_failures++;
        ESP_LOGW(TAG, "Lock failed after %d retries (timeout=%lums)", retries, timeout_ms);
    }
    
    return locked;
}

/**
 * @brief Print lock statistics (for debugging)
 */
void lvgl_lock_print_stats(void) {
    ESP_LOGI(TAG, "=== LVGL Lock Statistics ===");
    ESP_LOGI(TAG, "Total attempts:    %lu", lvgl_lock_stats.lock_attempts);
    ESP_LOGI(TAG, "Successes:         %lu (%.1f%%)", 
             lvgl_lock_stats.lock_successes,
             100.0f * lvgl_lock_stats.lock_successes / lvgl_lock_stats.lock_attempts);
    ESP_LOGI(TAG, "Failures:          %lu", lvgl_lock_stats.lock_failures);
    ESP_LOGI(TAG, "Total retries:     %lu", lvgl_lock_stats.lock_retries);
    ESP_LOGI(TAG, "Max retry count:   %lu", lvgl_lock_stats.max_retry_count);
    ESP_LOGI(TAG, "Avg wait time:     %llu us", 
             lvgl_lock_stats.total_wait_time_us / lvgl_lock_stats.lock_successes);
    ESP_LOGI(TAG, "Max wait time:     %llu us", lvgl_lock_stats.max_wait_time_us);
    ESP_LOGI(TAG, "===========================");
}
```

---

## Testing Checklist

After applying patches:

- [ ] **Rapid screen navigation** - Click through all menus 20+ times
- [ ] **WiFi reconnection** - Disconnect/reconnect WiFi during glucose fetch
- [ ] **Concurrent operations** - Trigger Dexcom + weather fetch simultaneously  
- [ ] **Lock timeout stress** - Monitor ESP_LOG for "Failed to lock LVGL" warnings
- [ ] **Memory leak test** - Run 24 hours, check `esp_get_minimum_free_heap_size()`
- [ ] **Heap fragmentation** - Check `heap_caps_get_info()` after 100+ screen transitions
- [ ] **Lock statistics** - Call `lvgl_lock_print_stats()` periodically

---

## Summary

**Total patches:** 13  
**Critical (must fix):** 5 patches  
**High (should fix):** 5 patches  
**Medium (nice to fix):** 3 patches

**Estimated time:**
- Critical: 1-2 hours  
- High: 2-3 hours  
- Medium: 3-4 hours  
- **Total: 6-9 hours**

**Risk reduction:**
- Before patches: **MEDIUM RISK** (rare crashes, UI freezes)
- After critical patches: **LOW RISK** (production-ready)
- After all patches: **MINIMAL RISK** (defensive, well-documented)

**Recommended order:**
1. Apply all critical patches first (enables core functionality)
2. Apply high priority patches (improves reliability)
3. Test thoroughly before medium priority patches
4. Add lock statistics for ongoing monitoring
