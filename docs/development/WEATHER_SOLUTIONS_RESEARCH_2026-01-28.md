# Weather Solutions Research - January 28, 2026

## Current Problem

Weather geocoding on ESP32 is causing **critical memory fragmentation**:
- Starting heap: 17,644 bytes
- After 3 HTTPS geocoding attempts: 536 bytes (17KB lost!)
- Result: Device crashes during glucose fetch

**Root Cause:** Each HTTPS connection creates ~20KB SSL context. Even after cleanup, memory fragmentation prevents reuse.

---

## Solution 1: Switch to HTTP (Recommended - Quick Win) ⭐

### Use HTTP for OpenStreetMap Nominatim

**Current Implementation:**
```c
// Currently using HTTPS
snprintf(url, sizeof(url),
         "https://nominatim.openstreetmap.org/search?postalcode=%s&country=us&format=json",
         zipcode);
```

**Proposed Change:**
```c
// Use HTTP instead (saves ~15KB SSL overhead per request)
snprintf(url, sizeof(url),
         "http://nominatim.openstreetmap.org/search?postalcode=%s&country=us&format=json",
         zipcode);

// In esp_http_client_config_t:
.transport_type = HTTP_TRANSPORT_OVER_TCP  // Instead of HTTP_TRANSPORT_OVER_SSL
```

**Pros:**
- ✅ Saves 15-20KB memory per request
- ✅ 5 minute implementation
- ✅ No API changes needed
- ✅ Nominatim supports HTTP

**Cons:**
- ⚠️ Less secure (but acceptable for non-sensitive weather data)

**Memory Savings:** ~15KB × 3 attempts = ~45KB reduction in peak usage

**Implementation Effort:** ⭐ 10 minutes

---

## Solution 2: Switch to OpenWeatherMap API with HTTP

### Replace Nominatim with OpenWeatherMap

OpenWeatherMap provides weather data directly from zipcode/city name - **no geocoding needed!**

**API Endpoint:**
```
http://api.openweathermap.org/data/2.5/weather?zip=98092,US&appid=YOUR_API_KEY&units=imperial
```

**Advantages:**
- ✅ Single HTTP request instead of 3 (geocoding + reverse geocoding + weather)
- ✅ Free tier: 1,000 calls/day (plenty for 5-30 minute updates)
- ✅ Returns temperature, conditions, icon codes directly
- ✅ Lightweight: 450-byte request, 2.3KB JSON response
- ✅ HTTP supported (no HTTPS required)

**Implementation Changes:**
1. Replace `zipcode_to_latlon()` with direct OpenWeatherMap call
2. Remove `reverse_geocode()` (not needed)
3. Parse JSON for temperature and condition code
4. Map OpenWeatherMap icon codes to LVGL weather icons

**Free Tier Limits:**
- 1,000 calls/day
- 5-minute updates = 288 calls/day ✅
- 30-minute updates = 48 calls/day ✅

**Code Example:**
```c
// Build URL with zipcode
snprintf(url, sizeof(url),
         "http://api.openweathermap.org/data/2.5/weather?zip=%s,US&appid=%s&units=%s",
         zipcode, api_key, temp_celsius ? "metric" : "imperial");

// Single request returns everything:
// - Temperature
// - Weather condition
// - Icon code
// - City name
```

**Pros:**
- ✅ Massive memory savings (1 HTTP vs 3 HTTPS)
- ✅ More reliable (single API call)
- ✅ Better weather data quality
- ✅ No SSL/HTTPS overhead

**Cons:**
- ⚠️ Requires API key signup (free)
- ⚠️ Need to map icon codes to your weather icons

**Memory Savings:** Eliminates 2 HTTPS geocoding calls entirely

**Implementation Effort:** ⭐⭐ 1-2 hours (API integration + icon mapping)

**Sources:**
- [ESP32 HTTP GET with OpenWeatherMap](https://randomnerdtutorials.com/esp32-http-get-open-weather-map-thingspeak-arduino/)
- [ESP32 Weather Station with OpenWeatherMap](https://www.electromaker.io/project/view/esp32-based-openweather-api-weather-station)
- [ESP32 Weather API Tutorial](https://techtutorialsx.com/2018/03/17/esp32-arduino-getting-weather-data-from-api/)

---

## Solution 3: mbedTLS Memory Optimizations

### ESP-IDF Configuration Changes

If you want to keep HTTPS, optimize mbedTLS memory usage.

**Step 1: Enable Dynamic Buffers (menuconfig)**
```
Component config → mbedTLS → Memory allocation strategy
[X] Enable dynamic TX/RX buffer allocation
```

**Step 2: Reduce TLS Buffer Sizes**
```
Component config → mbedTLS
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048  # Default: 16384 (saves 14KB!)
```

**Step 3: Free SSL Resources Early**
```
Component config → mbedTLS
[X] Free SSL peer certificate after its usage
[X] Free certificate, key and DHM data after its usage
```

**Expected Savings:**
- TLS OUT buffer: 16KB → 2KB = **14KB saved**
- Dynamic allocation: ~5KB saved
- **Total: ~19KB memory reduction**

**Pros:**
- ✅ Keeps HTTPS security
- ✅ Significant memory savings
- ✅ No code changes (just config)

**Cons:**
- ⚠️ May affect other HTTPS operations
- ⚠️ Requires full rebuild and testing

**Implementation Effort:** ⭐⭐ 30 minutes (menuconfig + rebuild)

**Sources:**
- [ESP-IDF RAM Usage Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/ram-usage.html)
- [ESP-TLS Configuration FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/esp-tls.html)
- [ESP32 Memory Optimization](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/performance/reduce-ram-usage.html)
- [mbedTLS Memory Discussion (ESP32 Forum)](https://esp32.com/viewtopic.php?t=4329)
- [ESP32 Memory Case Study (Medium)](https://medium.com/the-esp-journal/esp32-memory-analysis-case-study-eacc75fe5431)

---

## Solution 4: Reduce LVGL Memory Further

### Current: 36KB → Try: 32KB or 28KB

**sdkconfig Change:**
```ini
# Current
CONFIG_LV_MEM_SIZE_KILOBYTES=36

# Proposed
CONFIG_LV_MEM_SIZE_KILOBYTES=32  # Try this first (saves 4KB)
# OR
CONFIG_LV_MEM_SIZE_KILOBYTES=28  # If 32KB still stable (saves 8KB)
```

**Testing Strategy:**
1. Try 32KB first
2. Navigate through all UI screens
3. Check for LVGL out-of-memory errors
4. If stable, try 28KB

**Pros:**
- ✅ Quick test (5 minutes)
- ✅ Immediate memory savings
- ✅ Frees RAM for SSL operations

**Cons:**
- ⚠️ May cause UI crashes if too small
- ⚠️ Limits future UI complexity

**Memory Savings:**
- 32KB: +4KB free
- 28KB: +8KB free

**Implementation Effort:** ⭐ 5 minutes (edit sdkconfig, rebuild)

---

## Solution 5: Offline Zipcode Lookup Table

### Replace Geocoding with Embedded Zipcode Database

Instead of API calls, use a hardcoded lookup table for US zipcodes.

**Implementation:**
```c
// Embedded zipcode database (top 1000 US cities)
typedef struct {
    uint32_t zipcode;
    float lat;
    float lon;
    char city[32];
    char state[3];
} zipcode_entry_t;

static const zipcode_entry_t zipcode_db[] = {
    {98092, 47.3094, -122.1745, "Auburn", "WA"},
    {90210, 34.0901, -118.4065, "Beverly Hills", "CA"},
    // ... 998 more entries
};

// Binary search lookup (fast!)
static const zipcode_entry_t* find_zipcode(uint32_t zip) {
    // O(log n) binary search
}
```

**Pros:**
- ✅ Zero network calls for geocoding
- ✅ Instant lookups
- ✅ No memory fragmentation
- ✅ Works offline

**Cons:**
- ⚠️ Flash storage required (~50KB for 1000 entries)
- ⚠️ Only supports US zipcodes
- ⚠️ Requires manual database maintenance

**Memory Savings:** Eliminates both geocoding API calls

**Implementation Effort:** ⭐⭐⭐ 2-4 hours (build database + code)

---

## Solution 6: PSRAM (Hardware Upgrade)

### Add External PSRAM for SSL Buffers

**Note:** Your current hardware (JC2432W328 / ESP32-D0WD) may not have PSRAM. This requires hardware with PSRAM support.

**menuconfig Configuration:**
```
Component config → mbedTLS → Memory allocation strategy
[X] External SPIRAM (PSRAM)
```

**Pros:**
- ✅ Adds 4-8MB external RAM
- ✅ Solves memory issues completely
- ✅ Allows more UI features

**Cons:**
- ❌ Requires new hardware
- ⚠️ mbedTLS + PSRAM has known issues (see sources)
- ⚠️ May not work reliably

**Known Issues:**
Multiple forum posts report SSL handshake failures when forcing mbedTLS to use external PSRAM.

**Implementation Effort:** ⭐⭐⭐⭐ Hardware change required

**Sources:**
- [ESP32 PSRAM mbedTLS Issues (Forum)](https://www.esp32.com/viewtopic.php?t=12473)
- [Arduino PSRAM Issues](https://github.com/espressif/arduino-esp32/issues/1915)
- [LVGL with PSRAM Discussion](https://forum.lvgl.io/t/v9-2-2-how-to-lv-use-stdlib-malloc-by-psram-ps-malloc-on-esp32/18938)

---

## Solution 7: Alternative Geocoding Services

### Photon or Pelias Instead of Nominatim

**Photon API:**
```
http://photon.komoot.io/api?q=98092
```

**Pros:**
- ✅ Lightweight API
- ✅ HTTP support
- ✅ Search-as-you-type optimized

**Cons:**
- ⚠️ Public API may throttle heavy usage
- ⚠️ Same HTTP request overhead as Nominatim

**Pelias API:**
```
http://pelias.example.com/v1/search?text=98092
```

**Pros:**
- ✅ Fast, modern geocoder
- ✅ Built on Elasticsearch

**Cons:**
- ⚠️ Requires self-hosting (no free public API)

**Implementation Effort:** ⭐⭐ 1 hour (API swap)

**Sources:**
- [Photon Geocoding](https://photon.komoot.io/)
- [Nominatim vs Photon Comparison](https://www.geoapify.com/nominatim-vs-photon-geocoder/)
- [Pelias Geocoder](https://www.pelias.io/)

---

## Recommended Implementation Plan

### Phase 1: Quick Win (Tonight/Tomorrow Morning)
**Solution 1: Switch Nominatim to HTTP** ⭐ FASTEST
- Change `https://` → `http://`
- Change `HTTP_TRANSPORT_OVER_SSL` → `HTTP_TRANSPORT_OVER_TCP`
- Test immediately

**Expected Result:** Weather working again with minimal memory usage

### Phase 2: Optimal Solution (This Week)
**Solution 2: Switch to OpenWeatherMap** ⭐⭐ BEST LONG-TERM
- Sign up for free API key
- Replace geocoding calls with single OpenWeatherMap HTTP request
- Map icon codes to LVGL weather icons

**Expected Result:** More reliable weather, better data quality, less memory

### Phase 3: System Optimization (Optional)
**Solution 3 + 4: mbedTLS Config + LVGL Reduction**
- Enable dynamic mbedTLS buffers
- Reduce TLS buffer sizes
- Try LVGL at 32KB

**Expected Result:** Even more headroom for future features

---

## Memory Budget After Fixes

### Current State (Weather Disabled)
```
Total Heap:        226 KB
LVGL:              36 KB
SSL (Dexcom):      40 KB
Free:              ~150 KB ✅ Stable
```

### After Solution 1 (HTTP Nominatim)
```
Weather HTTP:      5 KB × 3 attempts = 15 KB temporary
Expected Free:     ~135 KB ✅ Should work
```

### After Solution 2 (OpenWeatherMap)
```
Weather HTTP:      5 KB × 1 request = 5 KB temporary
Expected Free:     ~145 KB ✅✅ Plenty of headroom
```

### After Solution 3+4 (mbedTLS + LVGL Optimizations)
```
LVGL:              32 KB (-4KB saved)
TLS Buffers:       Reduced by ~14KB
Expected Free:     ~163 KB ✅✅✅ Very comfortable
```

---

## Decision Matrix

| Solution | Effort | Memory Saved | Security | Reliability | Recommended? |
|----------|--------|--------------|----------|-------------|--------------|
| 1. HTTP Nominatim | ⭐ 10 min | ~45 KB | ⚠️ HTTP | ✅ Good | ✅ Quick fix |
| 2. OpenWeatherMap | ⭐⭐ 2 hrs | ~50 KB | ⚠️ HTTP | ✅✅ Best | ✅✅ Long-term |
| 3. mbedTLS Config | ⭐⭐ 30 min | ~19 KB | ✅ HTTPS | ✅ Good | ✅ If need HTTPS |
| 4. Reduce LVGL | ⭐ 5 min | 4-8 KB | N/A | ⚠️ Test | ✅ Easy win |
| 5. Zipcode Table | ⭐⭐⭐ 4 hrs | ~45 KB | N/A | ✅ Offline | ⚠️ US only |
| 6. PSRAM | ⭐⭐⭐⭐ HW | N/A | N/A | ⚠️ Issues | ❌ Not worth it |
| 7. Alt Geocoder | ⭐⭐ 1 hr | Same | ⚠️ HTTP | ⚠️ Throttle | ⚠️ Not better |

---

## Next Steps

**Tomorrow Morning:**
1. Try Solution 1 (HTTP Nominatim) - 10 minutes
2. If works, consider Solution 2 (OpenWeatherMap) for long-term

**This Week:**
1. Implement Solution 2 (OpenWeatherMap) properly
2. Try Solution 4 (reduce LVGL to 32KB)
3. Optional: Solution 3 (mbedTLS optimizations)

**If All Else Fails:**
Solution 5 (Zipcode lookup table) as last resort

---

## Sources

### Memory Optimization:
- [ESP-IDF RAM Usage Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/ram-usage.html)
- [ESP-TLS Configuration](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/esp-tls.html)
- [ESP Memory Optimization](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/performance/reduce-ram-usage.html)
- [ESP32 Memory Case Study](https://medium.com/the-esp-journal/esp32-memory-analysis-case-study-eacc75fe5431)
- [mbedTLS RAM Discussion](https://esp32.com/viewtopic.php?t=4329)
- [mbedTLS Heap Fragmentation (GitHub)](https://github.com/micropython/micropython/issues/8940)

### Weather APIs:
- [ESP32 OpenWeatherMap Tutorial](https://randomnerdtutorials.com/esp32-http-get-open-weather-map-thingspeak-arduino/)
- [ESP32 Weather Station](https://www.electromaker.io/project/view/esp32-based-openweather-api-weather-station)
- [ESP32 Weather API Guide](https://techtutorialsx.com/2018/03/17/esp32-arduino-getting-weather-data-from-api/)
- [ESP32 Weather Projects (GitHub)](https://github.com/lmarzen/esp32-weather-epd)

### Geocoding:
- [Nominatim Documentation](https://nominatim.org/)
- [Photon Geocoding](https://photon.komoot.io/)
- [Pelias Geocoder](https://www.pelias.io/)
- [Nominatim vs Photon](https://www.geoapify.com/nominatim-vs-photon-geocoder/)

### PSRAM:
- [ESP32 PSRAM mbedTLS Issues](https://www.esp32.com/viewtopic.php?t=12473)
- [Arduino PSRAM Issues](https://github.com/espressif/arduino-esp32/issues/1915)
- [LVGL PSRAM Discussion](https://forum.lvgl.io/t/v9-2-2-how-to-lv-use-stdlib-malloc-by-psram-ps-malloc-on-esp32/18938)

---

*Research compiled: 2026-01-28*
*Status: Ready for implementation*
*Recommendation: Start with Solution 1 (HTTP), migrate to Solution 2 (OpenWeatherMap) for production*
