# Memory Time-Sharing Solution - January 28, 2026 (Evening)

## Summary

✅ **Implemented memory time-sharing** - Dexcom and Weather share the same ~30KB memory pool
🎯 **Solution to OOM crash** - `mbedtls_ssl_setup returned -0x7F00` (out of memory)
🔄 **Close → Weather → Reopen** - Sequential operations prevent memory competition

---

## Problem: SSL Memory Allocation Failure

### The Error
```
E (27415) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (27417) esp-tls: create_ssl_handle failed
```

**Error Code:** `-0x7F00` = `MBEDTLS_ERR_SSL_ALLOC_FAILED` (-32512)
**Meaning:** mbedTLS couldn't allocate memory for SSL handshake

### Memory State
```
Free heap before weather:  17,608 bytes
SSL needs:                 ~20,000 bytes
Deficit:                   ~2,400 bytes ❌ INSUFFICIENT
```

### Root Cause Analysis

**Memory Allocation:**
```
Component                    Memory Usage
──────────────────────────────────────────
Dexcom persistent HTTPS:     ~30 KB (always open)
LVGL graphics buffers:       36 KB
WiFi stack:                  ~40 KB
Other (tasks, SNTP, NVS):    ~80 KB
──────────────────────────────────────────
Total allocated:             ~186 KB
Heap size:                   ~240 KB
Free heap:                   ~54 KB (theoretical)
Actual free at weather:      17.6 KB (fragmentation!)
```

**Why so low?**
- Memory fragmentation reduces effective free heap
- Dexcom keeps persistent SSL connection open (~30KB)
- Weather needs another SSL connection (~20KB)
- **Can't allocate 20KB from 17KB!**

---

## The Insight: Time-Sharing

**User's Brilliant Idea:**
> "Is there a way to maybe load dexcom data and display it, and then release that memory for weather, and it is timed to share the memory?"

**Exactly right!** Operations don't overlap:
- Dexcom fetches every 90 seconds
- Weather fetches every 5+ minutes
- **They never need SSL simultaneously!**

### Before (Memory Conflict)
```
Timeline:
├─ Dexcom SSL:      30 KB (always allocated)
├─ Free heap:       17 KB
└─ Weather needs:   20 KB ❌ CRASH!
```

### After (Time-Sharing)
```
Timeline:
0:00 - Dexcom fetch        (30 KB SSL used)
0:05 - Close Dexcom        (30 KB freed → 47 KB free!)
0:06 - Weather geocoding   (20 KB SSL used temporarily)
0:07 - Weather fetch       (20 KB SSL used temporarily)
0:08 - Reopen Dexcom       (30 KB SSL re-established)
0:10 - Next Dexcom fetch   (reuses connection)
```

**Memory Budget:**
```
Free heap with Dexcom closed: 17 KB + 30 KB = 47 KB
Weather SSL needs:            20 KB
Margin:                       27 KB ✅ PLENTY!
```

---

## Implementation

### New Functions in `dexcom_api.h` / `dexcom_api.c`

#### 1. Close Persistent Client
```c
void dexcom_close_persistent_client(void);
```

**What it does:**
- Calls `esp_http_client_cleanup(persistent_client)`
- Sets `persistent_client = NULL`
- Frees ~30KB of heap memory
- Logs free heap after closing

**Use case:** Before weather operations to free memory

---

#### 2. Reopen Persistent Client
```c
esp_err_t dexcom_reopen_persistent_client(void);
```

**What it does:**
- Re-initializes `persistent_client` with same config
- Re-establishes SSL connection to Dexcom Share
- Returns `ESP_OK` on success
- Logs free heap after reopening

**Use case:** After weather operations complete

**Note:** If already open, does nothing (idempotent)

---

### Modified `weather_update_task()` in `main.c`

**Line ~1672-1765:** Weather update task logic

#### Change 1: Close Dexcom Before Weather Operations
```c
// MEMORY OPTIMIZATION: Close Dexcom persistent client to free ~30KB for weather SSL
ESP_LOGI(TAG, "Closing Dexcom client to free memory for weather operations...");
dexcom_close_persistent_client();
vTaskDelay(pdMS_TO_TICKS(1000));  // Let memory stabilize

// Only geocode if we don't have coordinates yet
if (user_latitude == 0.0f && user_longitude == 0.0f) {
    ESP_LOGI(TAG, "Geocoding zipcode: %s (time is synced)", user_zipcode);
    // ... geocoding logic ...
}

// Fetch weather if we have coordinates
if (user_latitude != 0.0f && user_longitude != 0.0f) {
    // ... weather fetch logic ...
}
```

#### Change 2: Reopen Dexcom After Weather Complete
```c
// MEMORY OPTIMIZATION: Re-open Dexcom persistent client after weather operations complete
ESP_LOGI(TAG, "Weather operations complete, re-opening Dexcom client...");
dexcom_reopen_persistent_client();
```

---

## Expected Serial Output

### Successful Weather Update
```
I (xxxxx) CYGM: Free heap before weather geocoding: 17608 bytes
I (xxxxx) CYGM: Closing Dexcom client to free memory for weather operations...
I (xxxxx) DEXCOM: Closing persistent HTTP client to free memory (~30KB)
I (xxxxx) DEXCOM: Free heap after closing Dexcom client: 47342 bytes

I (xxxxx) CYGM: Geocoding zipcode: 98092
I (xxxxx) CYGM: Free heap before geocoding attempt 1: 47342 bytes
I (xxxxx) CYGM: Geocoding URL: https://nominatim.openstreetmap.org/...
I (xxxxx) CYGM: Geocoding HTTP result: ESP_OK (status: 200)
I (xxxxx) CYGM: Zipcode 98092 -> Lat: 47.3094, Lon: -122.1746
I (xxxxx) CYGM: Free heap after geocoding: 45120 bytes

I (xxxxx) CYGM: Reverse geocoding...
I (xxxxx) CYGM: Free heap after reverse geocoding: 44890 bytes

I (xxxxx) CYGM: Fetching weather for 47.3094, -122.1746
I (xxxxx) CYGM: Free heap before weather fetch: 44890 bytes
I (xxxxx) CYGM: Weather updated: 45 F (Hi: 48 F, Lo: 42 F), Code: 1
I (xxxxx) CYGM: Free heap after weather fetch: 44512 bytes

I (xxxxx) CYGM: Weather operations complete, re-opening Dexcom client...
I (xxxxx) DEXCOM: Re-opening persistent HTTP client for Dexcom
I (xxxxx) DEXCOM: Persistent HTTP client re-opened successfully
I (xxxxx) DEXCOM: Free heap after reopening Dexcom client: 17488 bytes
```

### Key Observations to Watch For

✅ **Good Signs:**
- Free heap jumps from ~17KB to ~47KB after closing Dexcom
- All HTTPS requests succeed (status 200)
- Free heap drops to ~44KB during weather (SSL overhead)
- Free heap returns to ~17KB after reopening Dexcom
- No crashes or reboots

❌ **Bad Signs (shouldn't happen now):**
- `mbedtls_ssl_setup returned -0x7F00` (OOM)
- Free heap doesn't increase after closing Dexcom (memory leak)
- Crashes or watchdog resets

---

## Trade-offs

### Advantages ✅
1. **Works with existing APIs** - No need for offline zipcode database
2. **No API keys required** - Still fully open-source
3. **Stable memory usage** - Predictable allocation patterns
4. **Simple implementation** - Two function calls

### Disadvantages ⚠️
1. **Next Dexcom fetch slower** - SSL handshake required after reopen (~1-2 seconds)
2. **Timing dependency** - Weather and glucose fetches can't overlap
3. **Slightly increased complexity** - More moving parts

### Impact on User Experience
- **Weather updates:** Every 5 minutes (configurable)
- **Glucose updates:** Every 90 seconds
- **First glucose fetch after weather:** ~2 seconds slower (one-time SSL handshake)
- **All subsequent fetches:** Normal speed (persistent connection reused)

**Example Timeline:**
```
00:00 - Glucose fetch (fast - persistent connection)
00:30 - Glucose fetch (fast)
01:00 - Glucose fetch (fast)
01:30 - Glucose fetch (fast)
05:00 - Weather fetch (Dexcom closed)
05:30 - Glucose fetch (SLOW - SSL handshake)
06:00 - Glucose fetch (fast - persistent connection restored)
06:30 - Glucose fetch (fast)
```

Only 1 out of every ~30-40 glucose fetches will be slower!

---

## Memory Budget Analysis

### Before (Crashed)
```
Component                     Heap Usage
──────────────────────────────────────────
LVGL buffers:                 36 KB
WiFi:                         40 KB
Dexcom persistent HTTPS:      30 KB ← PROBLEM
Tasks/SNTP/NVS:               80 KB
──────────────────────────────────────────
Free heap:                    17 KB
Weather needs:                20 KB ❌ INSUFFICIENT
```

### After (Time-Shared)
```
During Weather Operations:
──────────────────────────────────────────
LVGL buffers:                 36 KB
WiFi:                         40 KB
Dexcom persistent HTTPS:      0 KB ← CLOSED
Tasks/SNTP/NVS:               80 KB
──────────────────────────────────────────
Free heap:                    47 KB
Weather SSL (temporary):      20 KB ✅ SUFFICIENT
Remaining free:               27 KB
```

**Memory Savings:** 30 KB freed during weather operations

---

## Future Optimizations (If Needed)

### 1. Stagger Weather and Glucose Fetches
Ensure weather never runs during glucose fetch window:
```c
// Don't start weather if glucose fetch is imminent
time_t now;
time(&now);
if ((now - last_glucose_fetch) < 85) {
    ESP_LOGI(TAG, "Delaying weather - glucose fetch coming soon");
    vTaskDelay(pdMS_TO_TICKS(10000));
}
```

### 2. Preemptive Dexcom Reconnect
Reconnect Dexcom immediately after weather, before next glucose fetch:
```c
// After weather
dexcom_reopen_persistent_client();
// Preemptively fetch glucose to establish connection
if (last_glucose_fetch + 90 > now) {
    dexcom_fetch_glucose(&glucose_data);  // Warm up connection
}
```

### 3. Weather Scheduling
Align weather updates with glucose fetch gaps:
```c
// Schedule weather at :02, :07, :12, :17, :22 (between glucose fetches)
// Glucose at :00, :01:30, :03:00, :04:30, :06:00, ...
```

---

## Testing Checklist

### Basic Functionality
- [ ] Weather geocoding succeeds (HTTPS)
- [ ] Weather fetch succeeds
- [ ] Location displays correctly
- [ ] Temperature displays correctly
- [ ] Device doesn't crash or reboot

### Memory Behavior
- [ ] Free heap increases after closing Dexcom (~47KB)
- [ ] SSL operations succeed during weather
- [ ] Free heap decreases after reopening Dexcom (~17KB)
- [ ] Memory stable over 24 hours (no leaks)

### Glucose Monitoring
- [ ] First glucose fetch after weather is slower (SSL handshake)
- [ ] Subsequent glucose fetches are fast (persistent connection)
- [ ] Glucose display updates correctly
- [ ] No "rate limited" errors from connection churn

### Edge Cases
- [ ] Weather runs while glucose task is waiting (no conflict)
- [ ] Multiple weather retries don't leak memory
- [ ] Dexcom reopen failure handled gracefully

---

## Files Modified

1. **`main/dexcom_api.h`**
   - Added `dexcom_close_persistent_client()` declaration
   - Added `dexcom_reopen_persistent_client()` declaration

2. **`main/dexcom_api.c`**
   - Implemented `dexcom_close_persistent_client()`
   - Implemented `dexcom_reopen_persistent_client()`
   - Added heap logging

3. **`main/main.c`**
   - Line ~1683: Close Dexcom before weather operations
   - Line ~1765: Reopen Dexcom after weather complete
   - Added heap logging throughout weather task

4. **`docs/development/MEMORY_TIMESHARING_2026-01-28.md`** (this file)

---

## Build Command

```bash
cd C:\Projects\CYGM
idf.py build flash monitor -p COM40
```

---

## Success Criteria

✅ **Weather working:**
- Location and temperature display correctly
- Weather icon updates
- Sunrise/sunset times correct

✅ **Memory stable:**
- Free heap oscillates between ~17KB (Dexcom open) and ~47KB (Dexcom closed)
- No crashes or OOM errors
- No memory leaks over time

✅ **Glucose monitoring unaffected:**
- Glucose updates every 90 seconds
- Occasional slower fetch (after weather) is acceptable
- Persistent connection maintained 99% of the time

✅ **Open-source friendly:**
- No API keys required
- Anyone can build and use

---

## Comparison to Alternative Solutions

### Alternative 1: Offline Zipcode Database
**Pros:** No geocoding HTTPS, saves memory permanently
**Cons:** Large file (~300KB), maintenance burden, US-only
**Verdict:** Time-sharing is simpler and more flexible

### Alternative 2: Reduce LVGL Buffers Further
**Pros:** More free heap permanently
**Cons:** Display artifacts, slower rendering, quality degradation
**Verdict:** Time-sharing avoids compromising display quality

### Alternative 3: Disable Weather
**Pros:** Eliminates problem entirely
**Cons:** Defeats the purpose, users want weather
**Verdict:** User rejected this option

### Alternative 4: Upgrade to ESP32 with More RAM
**Pros:** No software changes needed
**Cons:** Hardware redesign, more expensive, not backward compatible
**Verdict:** Not feasible for existing devices

---

## Conclusion

**Memory time-sharing is the optimal solution:**
- ✅ Uses existing hardware
- ✅ Works with free, open-source APIs
- ✅ No API keys required
- ✅ Minimal performance impact
- ✅ Simple implementation
- ✅ Stable and predictable

**User's idea was spot-on!** 🎉

---

*Implemented: 2026-01-28*
*Previous Failed Approach: HTTP (Nominatim requires HTTPS)*
*Current Approach: HTTPS with Memory Time-Sharing*
*Status: Ready for testing*
