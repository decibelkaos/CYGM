# Weather HTTPS Required - January 28, 2026 (Evening)

## Summary

❌ **HTTP-only approach failed** - Nominatim requires HTTPS (enforces 301 redirect)
⚠️ **Memory is critically tight** - Only 17KB free heap before weather operations
🔄 **Switched back to HTTPS** - No alternative for modern geocoding APIs

---

## Problem Discovery

### HTTP Implementation Attempt
Following the morning session, we implemented HTTP-only weather using:
1. HTTP Nominatim for geocoding
2. HTTP Open-Meteo for weather data

**Goal:** Avoid SSL overhead (~50KB) to prevent memory crashes

### Reality Check: HTTP Status 301

Serial output revealed the issue:
```
I (24861) CYGM: Geocoding URL: http://nominatim.openstreetmap.org/search?...
W (40932) esp-tls: Failed to open new connection in specified timeout
E (40933) transport_base: Failed to open a new connection
E (40939) HTTP_CLIENT: Connection failed, sock < 0
I (40940) CYGM: Geocoding HTTP result: ESP_ERR_HTTP_CONNECT (status: 301)
```

**Status 301 = "Moved Permanently"**
- Nominatim redirects all HTTP requests to HTTPS
- ESP32 HTTP client configured for TCP-only transport can't follow HTTPS redirects
- Connection times out after 15 seconds

---

## Root Cause Analysis

### Why HTTP Doesn't Work Anymore

1. **Security policies**: Modern web APIs enforce HTTPS-only access
2. **Nominatim**: OpenStreetMap's geocoding service redirects HTTP → HTTPS (301)
3. **Open-Meteo**: Likely also requires HTTPS (not tested yet, but expected)
4. **Industry trend**: Plain HTTP is being phased out for security reasons

### Why This Matters for ESP32

```
SSL Memory Requirements:
- SSL handshake: ~20KB temporary allocation
- SSL context: ~15KB persistent during connection
- Certificate validation: ~5KB
───────────────────────────────────────────
Total: ~20-40KB peak usage per HTTPS request
```

**Current Memory State:**
```
Free heap before Dexcom: 26,460 bytes
Free heap before weather: 17,624 bytes
SSL requirement:          ~20,000 bytes
───────────────────────────────────────────
Available for SSL:        INSUFFICIENT (would need 20KB+)
```

---

## Code Changes Made

### 1. Reverted to HTTPS for Nominatim Geocoding

**File:** `main/main.c` (zipcode_to_latlon, ~line 981)

```c
// BEFORE (HTTP - doesn't work):
snprintf(url, sizeof(url),
         "http://nominatim.openstreetmap.org/search?...",
         zipcode);
esp_http_client_config_t config = {
    .transport_type = HTTP_TRANSPORT_OVER_TCP
};

// AFTER (HTTPS - required by Nominatim):
snprintf(url, sizeof(url),
         "https://nominatim.openstreetmap.org/search?...",
         zipcode);
esp_http_client_config_t config = {
    .transport_type = HTTP_TRANSPORT_OVER_SSL,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .buffer_size = 2048,        // Reduced from default 4096
    .buffer_size_tx = 1024       // Reduced from default 2048
};
```

**Memory optimizations:**
- Reduced HTTP buffer sizes (4KB → 2KB, 2KB → 1KB)
- Saves ~3KB but may limit response size

### 2. Updated Reverse Geocoding (HTTPS)

**File:** `main/main.c` (reverse_geocode, ~line 1162)

Same changes as above - switched from HTTP to HTTPS with reduced buffers.

### 3. Added Detailed Memory Logging

**File:** `main/main.c` (weather_update_task)

Added heap logging before/after each operation:
```c
ESP_LOGI(TAG, "Free heap before geocoding attempt %d: %lu bytes", attempt, esp_get_free_heap_size());
geocode_result = zipcode_to_latlon(...);
ESP_LOGI(TAG, "Free heap after geocoding attempt %d: %lu bytes", attempt, esp_get_free_heap_size());
```

This will show:
- How much memory SSL allocations consume
- Whether we're hitting OOM (out of memory)
- Memory recovery after SSL operations complete

---

## Memory Budget Analysis

### Current Memory Usage

```
Total ESP32 RAM:               ~520 KB
Available for heap:            ~240 KB
Current free before weather:   17.6 KB
Required for HTTPS:            ~20 KB
───────────────────────────────────────
Memory deficit:                ~2-3 KB (CRITICAL!)
```

### Where Memory Is Allocated

```
Component                   Memory Usage
────────────────────────────────────────
LVGL graphics buffers      36 KB (reduced from 48KB)
WiFi stack                 ~40 KB
Dexcom persistent HTTP     ~30 KB (SSL connection kept alive)
FreeRTOS tasks             ~30 KB (stacks)
HTTP response buffers      8 KB
Other (SNTP, NVS, etc.)    ~50 KB
───────────────────────────────────────
Total allocated:           ~194 KB
Free heap:                 ~46 KB (theoretical)
Actual free at weather:    17.6 KB (fragmentation!)
```

**Key Issue:** Memory fragmentation reduces effective free heap from 46KB to 17KB!

---

## Why Memory Is So Low (17KB)

### Timeline from Serial Output

```
Boot:                          Free heap: ~240 KB
After LVGL init:               Free heap: ~200 KB
After WiFi connect:            Free heap: ~150 KB
After Dexcom auth + SSL:       Free heap: 26 KB (line 329)
Before weather (15s later):    Free heap: 17.6 KB (line 341)
```

**Memory drop: 26KB → 17KB in 15 seconds!**

Likely causes:
1. **Dexcom persistent HTTP client** - Keeps SSL connection open (~10KB)
2. **LVGL rendering** - Temporary allocations for UI updates
3. **Memory fragmentation** - Small allocations scatter free blocks

---

## Attempted Solutions

### Solution 1: Reduce HTTP Buffers ✅ IMPLEMENTED
```c
.buffer_size = 2048,      // Was: 4096 (default)
.buffer_size_tx = 1024    // Was: 2048 (default)
```
**Savings:** ~3KB

### Solution 2: Add Memory Logging ✅ IMPLEMENTED
Track heap before/after each SSL operation to identify memory leaks.

### Solution 3: Keep Open-Meteo as HTTP ⏳ TO BE TESTED
Open-Meteo might still support plain HTTP. If so:
- Nominatim: HTTPS (~20KB × 2 calls = 40KB)
- Open-Meteo: HTTP (~5KB × 1 call = 5KB)
- **Total:** 45KB (vs 60KB if all HTTPS)

### Solution 4: Close Dexcom Connection Before Weather ❌ NOT YET
```c
// Close persistent HTTP client to free ~10KB
dexcom_close_persistent_client();
// Start weather
// Re-open persistent client after weather completes
```
**Risk:** Next glucose fetch will be slower (SSL handshake required)

---

## Next Steps

### Test Plan

1. **Build and flash** with HTTPS changes
2. **Monitor serial output** for:
   - Heap logs before/after each SSL operation
   - Whether geocoding succeeds or crashes
   - Memory recovery after operations complete

3. **Expected outcomes:**

   **Best case:** HTTPS works, memory is just barely sufficient
   ```
   Free heap before: 17.6 KB
   SSL allocation:   -20 KB (temporary)
   Heap during SSL:  ~-2 KB (CRITICAL but might work if temporary)
   Free heap after:  17.6 KB (memory recovered)
   ```

   **Worst case:** OOM crash
   ```
   Free heap: 17.6 KB
   SSL allocation fails: ESP_ERR_NO_MEM
   Or: Watchdog reset due to memory corruption
   ```

### Fallback Options (If Crashes)

**Option A: Close Dexcom persistent client before weather**
- Free ~10KB
- Weather would have ~27KB (sufficient for SSL)
- Glucose fetch slower (SSL handshake every 90s)

**Option B: Increase weather delay to 60+ seconds**
- Give more time for memory to stabilize
- Hope for better fragmentation luck

**Option C: Disable weather entirely** ❌ USER DOESN'T WANT THIS
- Defeats the purpose

**Option D: Use offline zipcode database**
- Create static table: US zipcode → lat/lon
- Store in SPIFFS or hardcode common zipcodes
- No geocoding API calls = no SSL overhead
- **Downside:** Large data file, maintenance burden

**Option E: Reduce LVGL buffer further**
- 36KB → 24KB (save 12KB)
- **Risk:** Display artifacts, slower rendering

---

## Open Questions

1. **Does Open-Meteo support plain HTTP?**
   - If yes: Keep it as HTTP to save 15KB
   - If no: All weather requires HTTPS (60KB total SSL overhead)

2. **Can we fragment SSL allocations?**
   - Break single 20KB allocation into smaller chunks?
   - Unlikely - SSL requires contiguous memory

3. **Is there an HTTP-only geocoding API?**
   - We couldn't find one (all modern APIs use HTTPS)
   - Last resort: Build offline zipcode database

---

## Success Criteria

✅ Nominatim geocoding succeeds (HTTPS)
✅ Weather fetch succeeds (HTTP or HTTPS)
✅ Device doesn't crash or reboot
✅ Memory stabilizes after operations
✅ Weather displays correctly on screen

---

## Files Modified

1. `main/main.c`
   - Line ~983: Changed Nominatim geocoding to HTTPS
   - Line ~1163: Changed reverse geocoding to HTTPS
   - Line ~1690: Added heap logging for geocoding
   - Line ~1712: Added heap logging for reverse geocoding
   - Line ~1734: Added heap logging for weather fetch

2. `docs/development/WEATHER_HTTPS_REQUIRED_2026-01-28.md` (this file)

---

## Build Command

```bash
cd C:\Projects\CYGM
idf.py build flash monitor -p COM40
```

---

## Serial Output to Watch For

### Good Signs ✅
```
I (xxxxx) CYGM: Free heap before geocoding: 17624 bytes
I (xxxxx) CYGM: Geocoding HTTP result: ESP_OK (status: 200)
I (xxxxx) CYGM: Free heap after geocoding: 15000 bytes  (temporary dip OK)
I (xxxxx) CYGM: Zipcode 98092 -> Lat: 47.3094, Lon: -122.1746
I (xxxxx) CYGM: Free heap after reverse geocoding: 17000 bytes
I (xxxxx) CYGM: Weather updated: 45 F
```

### Bad Signs ❌
```
E (xxxxx) HTTP_CLIENT: esp_tls_conn_new_sync failed
E (xxxxx) esp-tls: mbedtls_ssl_handshake returned -0x7680 (OOM during SSL)
E (xxxxx) CYGM: Failed to geocode: ESP_ERR_NO_MEM
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
```

---

*Testing required - memory is critically tight.*
*May need to implement fallback options.*

---

**Previous Documentation:**
- [Open-Meteo HTTP Implementation](OPENMETEO_HTTP_IMPLEMENTATION_2026-01-28.md) - Morning session (HTTP attempt)
- [Critical Memory Fixes](CRITICAL_MEMORY_FIXES_2026-01-27_EVENING.md) - Evening session 2
- [Audio & Weather Fix](AUDIO_WEATHER_FIX_2026-01-27.md) - Evening session 1
