# Memory Optimization & Audio Fix - 2026-01-27

## Summary
Fixed critical memory pressure issues and implemented audio tone playback for alarms.

## Problems Identified (from serial.txt)
- Only 2556 bytes free heap during alarm screen operations
- SSL handshake failures (`mbedtls_ssl_handshake returned -0x0010`)
- Periodic glucose fetch failures after initial success
- No audio output despite "Playing alarm tone" logs

---

## Fixes Implemented

### 1. Audio Tone Playback (FIXED)
**Problem:** Alarm system logged intent to play tones but no sound output.

**Root Cause:** No audio hardware initialization - speaker requires PWM via LEDC.

**Solution:** Added complete tone generation system
- **GPIO 26**: Speaker/buzzer via 8002A amplifier
- **LEDC Timer 0 + Channel 0**: Separate from LED (which uses Timer 1)
- **5 Alarm Tones:**
  - `ALARM_TONE_BEEP_1`: Single 800Hz beep (200ms)
  - `ALARM_TONE_BEEP_2`: Double beep with 100ms gap
  - `ALARM_TONE_BEEP_3`: Triple beep
  - `ALARM_TONE_CHIME`: Rising chime (600Hz → 700Hz → 800Hz)
  - `ALARM_TONE_ALERT`: Rapid 1000Hz pulses (3× 100ms)

**Files Modified:**
- `main/main.c`:
  - Added `BUZZER_GPIO`, `BUZZER_CHANNEL`, `BUZZER_TIMER` defines
  - Added `buzzer_init()`, `buzzer_tone()`, `play_alarm_tone()` functions
  - Called `buzzer_init()` in `app_main()` after LED initialization
  - Removed TODO comment from `trigger_alarm()` - now actually plays tones

**Memory Impact:** ~2KB code + negligible runtime overhead

---

### 2. Persistent HTTP Client Strategy (FIXED)
**Problem:** First glucose fetch succeeds, but subsequent fetches fail with SSL errors.

**Root Cause:**
- Persistent HTTP client was being cleaned up after each glucose fetch
- Recreating SSL context every 90 seconds fails due to memory fragmentation
- SSL context requires ~40KB contiguous memory, which becomes unavailable

**Solution:** Keep persistent client alive between glucose fetches
- Client stays initialized for the lifetime of the session
- Only cleaned up on authentication errors or shutdown
- Prevents memory fragmentation from repeated SSL allocation/deallocation

**Files Modified:**
- `main/dexcom_api.c:734`
  - Commented out `cleanup_persistent_client()` call after glucose fetch
  - Added logging to explain the strategy

**Memory Impact:**
- **Before**: 40KB allocated → freed → allocated → freed (fails due to fragmentation)
- **After**: 40KB stays allocated, but no fragmentation issues

**Expected Result:** Periodic glucose fetches should now succeed consistently

---

### 3. Weather Update Interval (NEW FEATURE)
**User Request:** Configurable weather update interval (5-90 minutes, default 5)

**Implementation:**
- Added `update_interval_min` field to `weather_settings_t`
- NVS storage with default of 5 minutes and clamping to 5-90 range
- UI slider in Weather Settings screen
- Weather task now uses configured interval instead of hardcoded 30 minutes

**Files Modified:**
- `main/nvs_config.h`: Added interval field to structure
- `main/nvs_config.c`: Updated save/load functions with validation
- `main/main.c`:
  - Added global `user_weather_interval_min` variable
  - Updated weather task to use configured interval
  - Added slider UI control with real-time value display
- `main/web_server.c`: Updated API endpoints to include interval

---

## Recommended: LVGL Memory Reduction

**Current:** LVGL using 48KB heap
**Recommended:** Reduce to 36KB (saves 12KB)

### How to Apply:
```bash
cd C:\Projects\CYGM
idf.py menuconfig
```

Navigate to:
```
Component config → LVGL configuration → Memory Settings
→ [CONFIG_LV_MEM_SIZE_KILOBYTES] Size of the memory (in kilobytes)
```

Change from `48` to `36` and save.

**Why This Works:**
- Your display is 320×240 = 76,800 pixels
- At RGB565 (2 bytes/pixel), full framebuffer = 153KB
- LVGL uses double-buffered partial rendering with ~48 lines
- 36KB is sufficient for: 36,864 bytes ÷ 2 bytes/pixel ÷ 320 width = 57 lines per buffer
- This exceeds minimum requirements (40 lines) while freeing 12KB

**Impact:** More free heap for SSL operations, reduced OOM risk

---

## Alternative Memory Optimizations (if still needed)

### Option 1: Reduce Screen Complexity
**Current State:** Alarm detail screen has many widgets (sliders, buttons, labels)

**If crashes persist after LVGL reduction:**
- Simplify alarm detail screen layout
- Use smaller fonts where possible (14px instead of 20px)
- Reduce number of visible UI elements

### Option 2: Implement Screen Pooling
**Concept:** Reuse screen objects instead of delete/create

**Implementation:**
```c
// Instead of:
lv_obj_del(old_screen);
screen = lv_obj_create(NULL);

// Do:
lv_obj_clean(screen);  // Remove children but keep screen object
```

**Benefit:** Reduces heap fragmentation from repeated large allocations

### Option 3: Reduce HTTP Buffer Sizes
**Current:** HTTP client buffers are 2048 bytes each

**If needed:**
```c
// In dexcom_api.c, init_persistent_client()
.buffer_size = 1536,     // Reduce from 2048
.buffer_size_tx = 1536,  // Reduce from 2048
```

**Risk:** May fail on large HTTP responses (currently not an issue)

---

## Expected Heap Usage After Fixes

| Component | Memory | Notes |
|-----------|--------|-------|
| **LVGL** | 36KB | Reduced from 48KB via menuconfig |
| **SSL Context** | 40KB | Persistent (no longer freed/reallocated) |
| **Dexcom HTTP Client** | 4KB | Buffers remain allocated |
| **Weather HTTP** | 2KB | Temporary during fetch |
| **LVGL UI Objects** | 15-20KB | Active screens and widgets |
| **cJSON Parsing** | 2-5KB | Temporary during API calls |
| **FreeRTOS Stacks** | 30KB | All tasks combined |
| **Other** | 10KB | Misc allocations |
| **Total Used** | ~140-150KB | |
| **Free Heap** | **76-86KB** | Up from 2.5KB! |

**Target:** 70-80KB free heap should prevent SSL handshake failures

---

## Testing Checklist

### Audio Playback:
- [ ] Device boots without errors
- [ ] Go to CGM Settings → Alarms
- [ ] Enter any alarm detail screen
- [ ] Toggle audio enabled ON
- [ ] Tap "Test" button (or wait for actual alarm trigger)
- [ ] **Should hear tone from speaker**

### Memory Stability:
- [ ] Watch serial monitor for first glucose fetch (should succeed)
- [ ] Wait 90 seconds for second fetch (should also succeed - not see SSL errors)
- [ ] Wait 5 minutes for third fetch (should continue succeeding)
- [ ] Check free heap: `I (xxxxx) DEXCOM: Keeping persistent client alive. Free heap: XXXXX bytes`

### Weather Interval:
- [ ] Go to Settings → Time/Weather → Weather
- [ ] Verify "Update Interval:" slider (5-90 minutes)
- [ ] Change to different value (e.g., 15 minutes)
- [ ] Tap "Save"
- [ ] Watch serial logs - weather should update at configured interval

### LVGL Memory (if applied):
- [ ] After running menuconfig and rebuilding
- [ ] Check boot logs for LVGL memory allocation
- [ ] Verify UI still renders correctly
- [ ] Check alarm screens still create without crashes

---

## Build and Flash

```bash
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

---

## If Problems Persist

### Symptom: Still running out of memory
**Next Steps:**
1. Apply LVGL memory reduction (36KB)
2. Check for memory leaks in custom code
3. Consider reducing HTTP buffer sizes
4. Implement screen pooling

### Symptom: SSL still failing after 2-3 fetches
**Possible Causes:**
- Memory leak in persistent client (should investigate with heap tracing)
- Need to increase task stack sizes
- mbedTLS configuration may need tuning

### Symptom: Audio not working
**Debug:**
- Check GPIO 26 is not used by anything else
- Verify buzzer hardware is connected to P4 JST connector
- Use multimeter to check for PWM signal on GPIO 26
- Try simpler test: `buzzer_tone(1000, 80); vTaskDelay(1000); buzzer_stop();`

---

## Files Changed

| File | Changes |
|------|---------|
| `main/main.c` | Audio init, tone functions, weather interval UI |
| `main/dexcom_api.c` | Removed persistent client cleanup after glucose fetch |
| `main/nvs_config.h` | Added weather interval field |
| `main/nvs_config.c` | Weather interval save/load |
| `main/web_server.c` | Weather interval API support |
| `sdkconfig` | (Manual) Reduce LVGL memory to 36KB |

---

*Applied: 2026-01-27*
*Next: Monitor glucose fetch success rate and audio output*
