# CYGM Touch & Alarm Regression Analysis
**Date:** 2026-02-18 20:35 PST  
**Symptoms:** Touch unresponsive, alarms not firing  
**Branch:** feature/online-geocoding (uncommitted changes)

---

## 🔴 Critical Issues Identified

### 1. **Removed vTaskDelay() Calls in alarm_screens.c** ⚠️ **HIGH RISK**
**Location:** `main/ui/alarm_screens.c` lines ~136-145

**What Changed:**
```c
// REMOVED these critical yield points:
-         vTaskDelay(pdMS_TO_TICKS(1));  // Brief yield to prevent watchdog timeout

create_alarm_card(..., "High Alarm", ...);
card_y += card_height + card_spacing;
- vTaskDelay(pdMS_TO_TICKS(1));  // REMOVED

create_alarm_card(..., "High Warning", ...);
card_y += card_height + card_spacing;
- vTaskDelay(pdMS_TO_TICKS(1));  // REMOVED

create_alarm_card(..., "Low Warning", ...);
```

**Why This Breaks Touch:**
- LVGL UI creation is CPU-intensive
- Creating 4 alarm cards back-to-back without yielding starves the touch task
- Touch task runs on Core 1, same as LVGL
- Without `vTaskDelay()`, the touch task never gets CPU time
- Result: **Screen appears frozen, touch completely unresponsive**

**Impact:** 🔥 **CRITICAL** - Alarm settings screen unusable

---

### 2. **Toggle Switch → Button Replacement** ⚠️ **MEDIUM RISK**
**Location:** `main/ui/alarm_screens.c` lines ~190-220

**What Changed:**
```c
// OLD (working):
lv_obj_t *toggle = lv_switch_create(card);
lv_obj_add_event_cb(toggle, alarm_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);

// NEW (potentially broken):
lv_obj_t *toggle = lv_btn_create(card);  // Not a switch!
lv_obj_add_event_cb(toggle, alarm_toggle_event_cb, LV_EVENT_CLICKED, alarm);
```

**Why This Breaks Alarms:**
- Alarm enable/disable state now depends on button clicks (LV_EVENT_CLICKED)
- Button event handling is different from switch event handling
- Event might not fire reliably if touch input is starved (see issue #1)
- ON/OFF state is now manually tracked instead of using LVGL's built-in switch state
- **Debounce logic added** (`ALARM_TOGGLE_DEBOUNCE_MS 300`) might block legitimate state changes

**Impact:** ⚠️ **HIGH** - Alarms may not enable when toggled

---

### 3. **Removed LVGL Mutex Locking** ⚠️ **MEDIUM RISK**
**Location:** `main/ui/alarm_screens.c` lines ~941-970

**What Changed:**
```c
// OLD (thread-safe):
static void alarm_settings_back_event_cb(...) {
    if (lvgl_port_lock(0)) {
        lv_scr_load(screen_menu);
        lvgl_port_unlock();
    }
}

// NEW (race conditions possible):
static void alarm_settings_back_event_cb(...) {
    lv_scr_load(screen_menu);  // No mutex!
}
```

**Why This Breaks:**
- LVGL operations must be mutex-protected when called from event handlers
- Multiple event callbacks removed `lvgl_port_lock(0)` / `lvgl_port_unlock()`
- Can cause UI corruption or crashes if touch events arrive during screen transitions
- **Race condition:** Background tasks might access LVGL objects during screen load

**Impact:** ⚠️ **MEDIUM** - UI corruption, potential crashes

---

### 4. **Inactivity Timer Added** ⚠️ **LOW-MEDIUM RISK**
**Location:** `main/ui/alarm_screens.c` lines ~43-50, 1225-1265

**What Changed:**
```c
// NEW timer running every 1000ms:
if (alarm_inactivity_timer == NULL) {
    alarm_inactivity_timer = lv_timer_create(alarm_inactivity_timer_cb, 1000, NULL);
}

// Callback checks inactivity and auto-returns to home after 30s
static void alarm_inactivity_timer_cb(lv_timer_t *timer) {
    uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
    if (inactive_ms < ALARM_UI_INACTIVITY_RETURN_MS) return;
    // ... auto-return to home screen
}
```

**Why This Might Break Alarms:**
- Timer callback runs on LVGL task (Core 1)
- If user is actively editing alarm, timer might trigger screen switch mid-edit
- **Verbose logging** (`ALARM_VERBOSE_LOG 1`) adds CPU overhead every 1000ms
- Timer checks `lv_disp_get_inactive_time()` - if touch is broken, this might auto-exit alarm UI

**Impact:** ⚠️ **LOW** - Auto-exit might interfere with alarm editing, extra CPU load

---

### 5. **Background Task Pausing Logic** ⚠️ **LOW RISK**
**Location:** `main/ui/alarm_screens.c` lines ~53-56

**What Changed:**
```c
// NEW: Enforces resource policy when entering alarm UI
home_screen_active = false;
pause_background_tasks = true;
```

**Why This Might Break Alarms:**
- Pausing background tasks prevents glucose/weather/battery updates
- If alarm conditions depend on fresh glucose data, alarms won't evaluate while in alarm UI
- **Battery monitoring paused** - low battery alarm won't fire while editing alarms

**Impact:** ⚠️ **LOW** - Alarms won't check conditions while in settings UI

---

## 📊 Root Cause Summary

### Primary Culprit: **Removed vTaskDelay() → Touch Starvation**
The removed `vTaskDelay()` calls in `create_alarm_settings_screen()` are **the most likely cause** of touch unresponsiveness. Creating 4 complex alarm cards back-to-back without yielding CPU time to the touch task completely starves touch input processing.

### Secondary Issues:
1. **Toggle switch → button** replacement changes event handling logic
2. **Missing mutex locks** create race conditions in screen transitions
3. **Inactivity timer** adds extra CPU overhead and logging
4. **Background task pausing** prevents alarm condition checks

---

## ✅ Rollback-Safe Patch Strategy

### **IMMEDIATE REVERT (High Priority)**

#### 1. **Restore vTaskDelay() Calls** 🔥 **CRITICAL**
**File:** `main/ui/alarm_screens.c`  
**Lines:** ~136-145

**Revert to:**
```c
create_alarm_card(screen_alarm_settings, "High Alarm", &current_alarm_settings.high_alarm,
                  card_y, card_height);
card_y += card_height + card_spacing;
vTaskDelay(pdMS_TO_TICKS(1));  // ← RESTORE THIS

create_alarm_card(screen_alarm_settings, "High Warning", &current_alarm_settings.high_warning,
                  card_y, card_height);
card_y += card_height + card_spacing;
vTaskDelay(pdMS_TO_TICKS(1));  // ← RESTORE THIS

create_alarm_card(screen_alarm_settings, "Low Warning", &current_alarm_settings.low_warning,
                  card_y, card_height);
card_y += card_height + card_spacing;
vTaskDelay(pdMS_TO_TICKS(1));  // ← RESTORE THIS
```

**Reason:** Prevents touch starvation during UI creation. Each 1ms yield allows touch task to run.

---

#### 2. **Restore lv_switch for Alarm Toggle** ⚠️ **HIGH PRIORITY**
**File:** `main/ui/alarm_screens.c`  
**Lines:** ~147-172

**Revert to:**
```c
// Enable/Disable toggle (left)
lv_obj_t *toggle = lv_switch_create(card);  // ← USE SWITCH, NOT BUTTON
if (toggle == NULL) {
    ESP_LOGE(TAG, "Failed to create toggle for %s!", title);
    return;
}
lv_obj_set_width(toggle, 40);
lv_obj_set_height(toggle, 20);
lv_obj_align(toggle, LV_ALIGN_LEFT_MID, 0, 0);

// Clear style transitions to prevent animation hangs
lv_obj_set_style_transition(toggle, NULL, LV_PART_MAIN | LV_STATE_ANY);
lv_obj_set_style_transition(toggle, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
lv_obj_set_style_transition(toggle, NULL, LV_PART_KNOB | LV_STATE_ANY);

if (alarm->enabled) {
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
}
lv_obj_add_event_cb(toggle, alarm_toggle_event_cb, LV_EVENT_VALUE_CHANGED, alarm);  // ← VALUE_CHANGED, not CLICKED
```

**Event Handler:**
```c
static void alarm_toggle_event_cb(lv_event_t *e) {
    alarm_config_t *alarm = (alarm_config_t *)lv_event_get_user_data(e);
    if (alarm == NULL) {
        ESP_LOGE(TAG, "alarm_toggle_event_cb: alarm is NULL!");
        return;
    }

    lv_obj_t *toggle = lv_event_get_target(e);
    alarm->enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);  // ← Use switch state
    ESP_LOGI(TAG, "Alarm enabled: %d", alarm->enabled);
}
```

**Reason:** lv_switch has built-in state management and reliable event handling.

---

#### 3. **Restore LVGL Mutex Locks** ⚠️ **MEDIUM PRIORITY**
**File:** `main/ui/alarm_screens.c`  
**Lines:** Multiple locations (alarm_settings_back_event_cb, alarm_detail_editor_back_event_cb, etc.)

**Revert to:**
```c
static void alarm_settings_back_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back to menu - saving alarm settings");
        nvs_save_alarm_settings(&current_alarm_settings);

        if (lvgl_port_lock(0)) {  // ← RESTORE MUTEX LOCK
            lv_scr_load(screen_menu);
            lvgl_port_unlock();     // ← RESTORE MUTEX UNLOCK
        }
    }
}
```

**Locations to fix:**
- `alarm_settings_back_event_cb`
- `alarm_detail_editor_back_event_cb`
- Any other screen transition callbacks

**Reason:** Prevents race conditions and UI corruption during screen transitions.

---

### **OPTIONAL REVERT (Low-Medium Priority)**

#### 4. **Disable Alarm Inactivity Timer** (Optional)
**File:** `main/ui/alarm_screens.c`  
**Lines:** ~43-50

**Comment out:**
```c
// OPTIONAL: Disable auto-return timer to simplify debugging
/*
if (alarm_inactivity_timer == NULL) {
    alarm_inactivity_timer = lv_timer_create(alarm_inactivity_timer_cb, 1000, NULL);
}
*/
```

**Reason:** Reduces CPU overhead and logging spam. User can manually exit alarm UI.

---

#### 5. **Disable Verbose Alarm Logging** (Optional)
**File:** `main/ui/alarm_screens.c`  
**Line:** ~48

**Change:**
```c
#define ALARM_VERBOSE_LOG 0  // ← Disable to reduce CPU/serial overhead
```

**Reason:** Less CPU time spent on logging = more time for touch handling.

---

### **KEEP (Working as Intended)**

#### ✅ ON/OFF Button Preference Display
The visual change from switch to button for **display purposes** is fine, **IF** the underlying state management uses `lv_switch` internally. The button visual can stay, but the control logic should use switch events.

**Compromise solution:**
- Use `lv_switch` for state management
- Apply custom styling to make it look like ON/OFF button
- Keep `LV_EVENT_VALUE_CHANGED` event handling

---

## 🛠️ Recommended Patch Sequence

### Phase 1: **Emergency Touch Fix** (Deploy ASAP)
1. ✅ Restore `vTaskDelay()` calls in `create_alarm_settings_screen()`
2. ✅ Test: Touch should respond in alarm settings screen

### Phase 2: **Alarm Toggle Fix** (Deploy within hours)
1. ✅ Revert toggle button → switch with `LV_EVENT_VALUE_CHANGED`
2. ✅ Restore LVGL mutex locks in event callbacks
3. ✅ Test: Alarm enable/disable should work reliably

### Phase 3: **Cleanup & Optimization** (Deploy within days)
1. ✅ Disable alarm inactivity timer (or set to 60s minimum)
2. ✅ Disable verbose logging (`ALARM_VERBOSE_LOG 0`)
3. ✅ Remove unnecessary `alarm_trace_event()` calls
4. ✅ Test: UI should be responsive with reduced CPU overhead

---

## 🧪 Testing Checklist

After applying patches:
- [ ] Touch responds within alarm settings screen
- [ ] Alarm toggle switches work (check NVS storage)
- [ ] Alarms fire when glucose crosses thresholds
- [ ] Screen transitions don't crash or corrupt
- [ ] Battery/glucose/weather updates continue when not in alarm UI
- [ ] No watchdog timeouts during UI creation
- [ ] Serial logs show no race conditions

---

## 📁 Files to Patch

1. **`main/ui/alarm_screens.c`** (primary fix target)
2. **`main/tasks/background_tasks.c`** (verify no related issues)
3. **`main/main.c`** (verify weather animation changes don't block touch)

---

## ⚠️ DO NOT REVERT

These changes are safe and should remain:
- ✅ Weather icon animation code (main.c)
- ✅ Background task WiFi connection checks
- ✅ Battery charging indicator fade animation
- ✅ Tone picker pagination improvements
- ✅ Serial screenshot functionality
- ✅ Alarm tone replacements (Fur Elise, Dixie Horn, Saints)

---

## 🎯 Success Criteria

**Before Patch:**
- ❌ Touch unresponsive in alarm settings
- ❌ Alarms don't enable when toggled
- ❌ Potential UI crashes during screen transitions

**After Patch:**
- ✅ Touch responds instantly in all screens
- ✅ Alarm toggles work reliably
- ✅ Alarms fire when conditions met
- ✅ Smooth screen transitions with no corruption
- ✅ No watchdog timeouts
- ✅ Background tasks resume when exiting alarm UI

---

**End of Analysis**
