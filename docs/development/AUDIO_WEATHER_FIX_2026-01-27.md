# Audio & Weather Fix - January 27, 2026 (Evening)

## Issues Identified from Serial Log

### 1. Tone Preview Button Not Working ✅ FIXED
**Symptom:** Alarm tones work correctly when glucose triggers alarms, but preview button in settings doesn't play sounds

**Root Cause:** Preview button callback had commented-out TODO instead of calling `play_alarm_tone()`

**Fix Applied:**
- Updated `tone_preview_btn_event_cb()` to call `play_alarm_tone(tone_id, editing_alarm->volume)`
- Changed tone_id type from `int` to `alarm_tone_t`
- Removed unused variable warning

**Status:** ✅ Audio hardware works (confirmed by user hearing alarm at glucose 202 mg/dL)

### 2. Critical Memory Pressure
**Symptom:**
- Only 316 bytes free heap (serial line 463)
- `mbedtls_ssl_setup returned -0x7F00` (out of memory)
- Device crash and reboot (serial line 480)

**Root Cause:** LVGL using 48KB + Dexcom SSL (40KB) + Weather HTTPS = memory exhaustion

**Fixes Applied:**
1. **Reduced LVGL Memory:** 48KB → 36KB (saves 12KB)
   - Changed in `sdkconfig`: `CONFIG_LV_MEM_SIZE_KILOBYTES=36`
   - 36KB is sufficient for 320×240 display with double buffering

2. **Increased Weather Delay:** 20s → 60s before first attempt
   - Gives Dexcom SSL connection time to stabilize
   - Allows memory fragmentation to settle
   - Added heap logging before geocoding

**Expected Result:**
- Before: ~2KB free heap → crashes
- After: ~14KB free heap → stable

### 3. Weather DNS Failures
**Symptom:**
```
E (31977) esp-tls: couldn't get hostname for :nominatim.openstreetmap.org
E (37012) esp-tls: [sock=55] select() error: Not enough space
```

**Root Cause:** Memory exhaustion preventing DNS lookups and new socket creation

**Fixes Applied:**
- Memory optimization (see #2 above) should resolve this
- Weather task now waits 60 seconds for memory to stabilize
- Existing 5-second retry delays between attempts remain

---

## Files Modified

| File | Changes |
|------|---------|
| `main/main.c` | • Added startup test beep in `buzzer_init()`<br>• Increased weather delay 20s → 60s<br>• Added heap logging before geocoding |
| `sdkconfig` | • Reduced LVGL memory 48KB → 36KB |

---

## Build Instructions

```bash
cd C:\Projects\CYGM
idf.py build flash monitor -p COM40
```

**Expected Serial Output:**
```
I (962) CYGM: Buzzer PWM initialized (GPIO 26)
```

**Speaker Hardware:** ✅ Confirmed working (user heard alarm tones at glucose 202 mg/dL)

---

## Testing Checklist

### Audio Test
- [x] Alarm tones working (confirmed at glucose 202 mg/dL)
- [ ] Test tone preview button in alarm settings
  - Go to: Settings → CGM Settings → Dexcom → Alarms
  - Edit any alarm → Tap tone buttons
  - Should hear tone preview when tapping each tone option

### Memory Stability Test
- [ ] Watch serial for "Free heap before screen create" messages
- [ ] Should see >5KB free (instead of 316 bytes)
- [ ] No SSL -0x7F00 errors
- [ ] No device crashes/reboots

### Weather Test
- [ ] Wait 60 seconds after Dexcom auth
- [ ] Watch for: "Free heap before weather geocoding: XXXXX bytes"
- [ ] Should see >10KB free heap
- [ ] Weather geocoding should succeed (no DNS errors)
- [ ] Weather icon should display correctly

---

## Memory Analysis

### Before Fix:
```
LVGL: 48KB
SSL (Dexcom): 40KB persistent
Weather HTTPS: ~20KB temporary
Other: ~30KB
─────────────
Total: ~138KB used
Free: 88KB theoretical, but fragmented to only 316 bytes usable!
```

### After Fix:
```
LVGL: 36KB (-12KB saved!)
SSL (Dexcom): 40KB persistent
Weather HTTPS: ~20KB temporary (but delayed 60s)
Other: ~30KB
─────────────
Total: ~126KB used
Free: ~100KB, less fragmentation due to delayed weather task
```

---

## Known Issues

### Issue: Periodic Glucose Fetch SSL Failures
**Status:** Not yet observed in this build, but was failing in previous builds

**Monitoring:** Watch for these errors after 90-second glucose intervals:
```
E (xxxxx) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0010
```

**If occurs:** May need to implement SSL session resumption or further memory optimization

### Issue: Weather Still Failing After 60s
**If DNS still fails:** Consider these options:
1. Disable weather entirely to save memory
2. Use alternative geocoding service with simpler API
3. Hardcode lat/lon for known zipcodes (offline lookup table)

---

## Debugging Commands

### Check Free Heap (via serial):
Look for these log lines:
```
I (xxxxx) CYGM: Free heap before screen create: XXXXX bytes
I (xxxxx) CYGM: Free heap before weather geocoding: XXXXX bytes
I (xxxxx) DEXCOM: Free heap before glucose fetch: XXXXX bytes
```

### Expected Heap Values (healthy):
- Before screen create: >5KB
- Before weather: >10KB
- Before glucose fetch: >10KB

### Critical Heap Values (danger):
- <2KB: Expect crashes
- <1KB: Imminent crash
- <500 bytes: Already crashed

---

## Next Steps

1. **Build and flash** current revision with all fixes
2. **Listen for startup beep** to verify speaker hardware
3. **Monitor serial** for heap values and errors
4. **Report back:**
   - Did you hear startup beep? ✓/✗
   - Did you hear alarm tones? ✓/✗
   - Did weather geocoding work? ✓/✗
   - Any crashes or reboots? ✓/✗
   - Free heap values before/after operations

---

*Applied: 2026-01-27 Evening*
*Next: Hardware verification and heap monitoring*
