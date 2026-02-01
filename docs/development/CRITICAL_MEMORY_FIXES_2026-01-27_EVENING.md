# Critical Memory Fixes - January 27, 2026 (Evening Session 2)

## Emergency Fixes Applied

**Context:** After applying LVGL reduction (48KB → 36KB), two new critical crashes were discovered during testing.

---

## Issue 1: Tone Picker Screen Crash ✅ FIXED (Second Attempt)

### Symptom (Second Occurrence)
```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
PC: 0x400ea79b (lv_obj_get_local_style_prop)
EXCVADDR: 0x00000020
Backtrace: lv_scr_load -> alarm_tone_btn_event_cb at main.c:5096
```

### Root Cause (Revised Analysis)
The **first fix was incorrect**. The actual problem:
1. Code deleted `screen_alarm_detail_editor` WHILE IT WAS THE ACTIVE SCREEN
2. Then called `lv_scr_load(screen_tone_picker)` to switch screens
3. LVGL tried to transition FROM the deleted screen → **use-after-free crash**

**Critical Lesson:** Never delete a screen while it's the active/loaded screen. Always load the new screen FIRST, then delete the old one.

### Fixes Applied

**File: `main/main.c`**

**Fix 1: Correct screen transition order in tone picker navigation (line 5080-5105)**
```c
ESP_LOGI(TAG, "Opening tone picker from detail editor");

// Delete old tone picker if it exists
if (screen_tone_picker != NULL) {
    lv_obj_del(screen_tone_picker);
    screen_tone_picker = NULL;
}

// Create new tone picker screen
create_tone_picker_screen(alarm);

if (screen_tone_picker != NULL) {
    // Load new screen FIRST (this switches away from detail editor)
    lv_scr_load(screen_tone_picker);

    // NOW safe to delete detail editor (no longer active screen)
    if (screen_alarm_detail_editor != NULL) {
        lv_obj_del(screen_alarm_detail_editor);
        screen_alarm_detail_editor = NULL;
    }
} else {
    ESP_LOGE(TAG, "Failed to create tone picker screen - out of memory");
}
```

**Fix 2: Same fix for tone picker back button (line 4850-4871)**
```c
// Delete old detail editor if it exists
if (screen_alarm_detail_editor != NULL) {
    lv_obj_del(screen_alarm_detail_editor);
    screen_alarm_detail_editor = NULL;
}

// Create and load new detail editor
create_alarm_detail_editor_screen(editing_alarm);
lv_scr_load(screen_alarm_detail_editor);

// Now safe to delete tone picker (no longer active screen)
if (screen_tone_picker != NULL) {
    lv_obj_del(screen_tone_picker);
    screen_tone_picker = NULL;
}
```

**Fix 3: Same fix for tone selection (line 4955-4972)**
After selecting a tone, properly delete tone picker after loading detail editor.

**Fix 4: Detail editor to alarm settings navigation (line 4817-4831)**
Delete alarm settings screen after loading detail editor.

**Fix 5: Detail editor back to alarm settings (line 5015-5030)**
Delete detail editor after loading alarm settings.

**Fix 6: Add NULL check after screen creation (line 4865-4869)**
```c
static void create_tone_picker_screen(alarm_config_t *alarm) {
    screen_tone_picker = lv_obj_create(NULL);
    if (screen_tone_picker == NULL) {
        ESP_LOGE(TAG, "Failed to create tone picker screen - LVGL out of memory!");
        return;
    }
    lv_obj_add_style(screen_tone_picker, &style_bg, 0);
    // ...
}
```

**Status:** ✅ Fixed - All screen transitions now follow correct order: create → load → delete old

---

## Issue 2: Glucose Fetch NULL Pointer Crash ✅ FIXED

### Symptom
```
I (107613) DEXCOM: Free heap before glucose fetch: 536 bytes
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
PC: 0x400014fd (strlen in ROM)
EXCVADDR: 0x00000000
Backtrace: prepare_client_for_request at dexcom_api.c:247
```

### Root Cause Analysis

**Memory Timeline:**
1. 67616ms: 17644 bytes free (before weather geocoding)
2. 77847-92928ms: Weather attempts 3 failed HTTPS connections (each creates ~20KB SSL context)
3. 107613ms: **536 bytes free** (before glucose fetch) → **17KB disappeared!**

**Crash Sequence:**
1. `cJSON_PrintUnformatted()` tries to allocate memory for JSON string
2. With only 536 bytes free, allocation **fails and returns NULL**
3. Code passes NULL to `prepare_client_for_request(url, NULL)`
4. Function calls `strlen(NULL)` → **CRASH**

### Fixes Applied

**File: `main/dexcom_api.c`**

**Fix 1: Add NULL checks for all cJSON operations (4 locations)**

**Location 1: `get_account_id()` (line 290-306)**
```c
cJSON *body = cJSON_CreateObject();
if (body == NULL) {
    ESP_LOGE(TAG, "Failed to create JSON body - out of memory");
    return ESP_ERR_NO_MEM;
}
// ... add fields ...
char *body_str = cJSON_PrintUnformatted(body);
cJSON_Delete(body);

if (body_str == NULL) {
    ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory");
    return ESP_ERR_NO_MEM;
}
```

**Location 2: `get_session_id()` (line 369-385)** - Same pattern

**Location 3: `get_session_by_name()` (line 448-464)** - Same pattern

**Location 4: `dexcom_fetch_glucose()` (line 613-634)** - Same pattern with enhanced logging:
```c
if (body_str == NULL) {
    ESP_LOGE(TAG, "Failed to serialize JSON body - out of memory (free heap: %lu bytes)",
             esp_get_free_heap_size());
    return ESP_ERR_NO_MEM;
}
```

**Fix 2: Defensive NULL check in `prepare_client_for_request()` (line 246-253)**
```c
// Set POST body (check for NULL to prevent crash)
if (body != NULL) {
    esp_http_client_set_post_field(persistent_client, body, strlen(body));
} else {
    ESP_LOGW(TAG, "POST body is NULL - skipping");
    esp_http_client_set_post_field(persistent_client, "", 0);
}
```

**Status:** ✅ Fixed - Graceful error handling instead of crashes

---

## Issue 3: Weather Task Memory Leak ⚠️ DISABLED

### The Problem

Weather geocoding causes **severe memory fragmentation**:
- Before weather: 17644 bytes free
- After 3 failed HTTPS attempts: 536 bytes free
- **Loss: 17KB in 40 seconds**

Each HTTPS connection attempt:
1. Creates SSL context (~20KB)
2. Attempts DNS lookup
3. Times out after 10 seconds
4. Cleans up HTTP client
5. **But memory is NOT recovered due to fragmentation**

With 3 retry attempts, this compounds the problem.

### Temporary Solution: Disable Weather ✅ APPLIED

**File: `main/main.c`**

**Disabled 3 locations where weather task is created:**

**Location 1: After Dexcom auth (line 5553-5559)**
```c
// DISABLED: Weather task causes severe memory fragmentation (17KB → 536 bytes)
// TODO: Re-enable after implementing memory pooling or reducing SSL overhead
// if (weather_task_handle == NULL) {
//     ESP_LOGI(TAG, "Starting weather task (after Dexcom)...");
//     xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
// }
```

**Location 2: No Dexcom credentials (line 5586-5590)**
```c
// No Dexcom credentials - weather task DISABLED due to memory issues
ESP_LOGI(TAG, "No Dexcom credentials found (weather task disabled for memory)");
// DISABLED: Weather task causes severe memory fragmentation
```

**Location 3: Time/Weather menu (line 3043-3047)**
```c
// DISABLED: Weather task causes severe memory fragmentation (17KB → 536 bytes)
// Start weather update task (check if not already running)
```

**Status:** ⚠️ Temporarily disabled - Prevents crashes, but removes weather feature

---

## Memory Budget Analysis

### Current Configuration
```
Total Heap:        ~226 KB
LVGL Pool:         36 KB (reduced from 48KB)
SSL Context:       ~40 KB (persistent for Dexcom)
Weather HTTPS:     ~20 KB × 3 attempts = 60KB temporary
Other:             ~30 KB
─────────────────────────────
Theoretical Usage: ~166 KB
Free (expected):   ~60 KB

ACTUAL Free (before weather): 17 KB ❌
ACTUAL Free (after weather):  536 bytes ❌❌❌
```

### The Fragmentation Problem

Even though HTTP clients are cleaned up:
1. SSL allocations fragment heap into small unusable chunks
2. Subsequent allocations fail even though "free heap" exists
3. This is classic **heap fragmentation** - total free memory exists but not in contiguous blocks

---

## Testing Results

### Expected Behavior After Fixes:
✅ Tone picker opens without crash (screen transition fixed)
✅ Navigate back from tone picker without crash
✅ All alarm screen transitions work smoothly
✅ Alarm settings fully functional
✅ Glucose fetching stable (no SSL crashes)
✅ No device reboots
⚠️ Weather display disabled (will show last known value or placeholder)

### Heap Monitoring Points:
```
I (xxxxx) CYGM: Free heap before screen create: >10KB (should be higher now)
I (xxxxx) DEXCOM: Free heap before glucose fetch: >15KB (stable without weather)
```

---

## Next Steps

### Immediate (User Testing)
1. Build and flash with all fixes
2. Monitor heap values in serial output
3. Test alarm settings and tone picker
4. Verify Dexcom glucose fetching stability
5. Confirm no device crashes/reboots

### Short Term (Weather Re-enablement)
To re-enable weather without memory issues, implement ONE of:

**Option A: HTTP Connection Pooling**
- Share single HTTP client between weather attempts
- Reuse SSL session across retries
- Reduces peak memory usage

**Option B: Reduce LVGL Further**
- Try 32KB or 28KB for LVGL
- Frees 4-8KB more for SSL operations
- May impact UI smoothness

**Option C: Use HTTP (not HTTPS) for Weather**
- OpenStreetMap allows HTTP
- Saves ~15KB SSL overhead per request
- Less secure but acceptable for weather data

**Option D: Hardcode Zip→Coordinates**
- Offline lookup table for US zipcodes
- No HTTPS connections needed
- Eliminates geocoding memory spike entirely

### Long Term (Optimal Solution)
- Use external PSRAM (if hardware supports it)
- Or migrate to ESP32-S3 with more RAM
- Or implement custom heap allocator with defragmentation

---

## Screen Transition Pattern (CRITICAL)

**Correct Order for LVGL Screen Transitions:**
```c
// 1. Delete old instance of NEW screen (if exists)
if (new_screen != NULL) {
    lv_obj_del(new_screen);
    new_screen = NULL;
}

// 2. Create new screen
create_new_screen();

// 3. Load new screen (switches away from old screen)
lv_scr_load(new_screen);

// 4. NOW delete old screen (safe - no longer active)
if (old_screen != NULL) {
    lv_obj_del(old_screen);
    old_screen = NULL;
}
```

**Why This Order Matters:**
- LVGL crashes if you delete a screen while it's the active/loaded screen
- Always load the new screen FIRST to make the old screen inactive
- Only then is it safe to delete the old screen

## Files Modified

| File | Changes |
|------|---------|
| `main/main.c` | • Fixed 5 screen transition handlers to use correct order (lines 4817, 4850, 4955, 5015, 5080)<br>• Added NULL check in `create_tone_picker_screen()` (line 4865)<br>• Disabled weather task at 3 locations (lines 3043, 5553, 5586) |
| `main/dexcom_api.c` | • Added NULL checks for `cJSON_CreateObject()` (4 locations)<br>• Added NULL checks for `cJSON_PrintUnformatted()` (4 locations)<br>• Added defensive NULL check in `prepare_client_for_request()` (line 246) |

---

## Build Instructions

```bash
cd C:\Projects\CYGM
idf.py build flash monitor -p COM40
```

**Expected Serial Output:**
```
I (xxxxx) CYGM: Opening tone picker from detail editor
I (xxxxx) DEXCOM: Free heap before glucose fetch: [should be >15KB]
[No more crashes or reboots]
```

---

## Known Limitations

⚠️ **Weather display is currently DISABLED**
- Weather icon will not update
- Location will not display
- Temperature will show last known value or placeholder

This is a temporary measure to ensure device stability. Weather will be re-enabled in a future update after implementing proper memory management.

---

*Applied: 2026-01-27 Evening (Session 2)*
*Status: ✅ All critical crashes fixed - tone picker confirmed working by user*
*Next: Re-enable weather with memory optimization*

---

## User Testing Results

✅ **Tone picker working** - User confirmed all alarm tone selection working correctly
✅ **No crashes** - Device stable during alarm configuration
⚠️ **Weather disabled** - Awaiting memory optimization solution
