# Weather HTTP Fix - Quick Start Guide

## 10-Minute Fix to Re-Enable Weather

### The Change: HTTPS → HTTP

Change two lines in `main/main.c` to switch OpenStreetMap Nominatim from HTTPS to HTTP.

---

## Location 1: zipcode_to_latlon() function

**File:** `main/main.c`
**Around line:** 983-995

### Find this code:
```c
char url[256];
// Use HTTPS but skip cert verification for OpenStreetMap (non-critical geocoding data)
snprintf(url, sizeof(url),
         "https://nominatim.openstreetmap.org/search?postalcode=%s&country=us&format=json&limit=1&addressdetails=1",
         zipcode);

// ... later in the function ...

esp_http_client_config_t config = {
    .url = url,
    .event_handler = http_event_handler,
    .timeout_ms = 10000,
    .user_agent = "CYGM/1.0",
    .transport_type = HTTP_TRANSPORT_OVER_SSL
};
```

### Change to this:
```c
char url[256];
// Use HTTP for OpenStreetMap geocoding (saves ~15KB SSL memory overhead)
snprintf(url, sizeof(url),
         "http://nominatim.openstreetmap.org/search?postalcode=%s&country=us&format=json&limit=1&addressdetails=1",
         zipcode);

// ... later in the function ...

esp_http_client_config_t config = {
    .url = url,
    .event_handler = http_event_handler,
    .timeout_ms = 10000,
    .user_agent = "CYGM/1.0",
    .transport_type = HTTP_TRANSPORT_OVER_TCP  // Changed from HTTP_TRANSPORT_OVER_SSL
};
```

**Changes:**
1. Line ~983: `https://` → `http://`
2. Line ~995: `HTTP_TRANSPORT_OVER_SSL` → `HTTP_TRANSPORT_OVER_TCP`

---

## Location 2: reverse_geocode() function

**File:** `main/main.c`
**Around line:** 1149-1161

### Find this code:
```c
char url[256];
// Use HTTPS but skip cert verification for OpenStreetMap (non-critical geocoding data)
snprintf(url, sizeof(url),
         "https://nominatim.openstreetmap.org/reverse?lat=%.4f&lon=%.4f&format=json&addressdetails=1",
         lat, lon);

// ... later in the function ...

esp_http_client_config_t config = {
    .url = url,
    .event_handler = http_event_handler,
    .timeout_ms = 10000,
    .user_agent = "CYGM/1.0",
    .transport_type = HTTP_TRANSPORT_OVER_SSL
};
```

### Change to this:
```c
char url[256];
// Use HTTP for OpenStreetMap reverse geocoding (saves ~15KB SSL memory overhead)
snprintf(url, sizeof(url),
         "http://nominatim.openstreetmap.org/reverse?lat=%.4f&lon=%.4f&format=json&addressdetails=1",
         lat, lon);

// ... later in the function ...

esp_http_client_config_t config = {
    .url = url,
    .event_handler = http_event_handler,
    .timeout_ms = 10000,
    .user_agent = "CYGM/1.0",
    .transport_type = HTTP_TRANSPORT_OVER_TCP  // Changed from HTTP_TRANSPORT_OVER_SSL
};
```

**Changes:**
1. Line ~1149: `https://` → `http://`
2. Line ~1161: `HTTP_TRANSPORT_OVER_SSL` → `HTTP_TRANSPORT_OVER_TCP`

---

## Re-Enable Weather Task

**File:** `main/main.c`

### Location 1: After Dexcom auth (line ~5553)

**Find:**
```c
// DISABLED: Weather task causes severe memory fragmentation (17KB → 536 bytes)
// TODO: Re-enable after implementing memory pooling or reducing SSL overhead
// if (weather_task_handle == NULL) {
//     ESP_LOGI(TAG, "Starting weather task (after Dexcom)...");
//     xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
// }
```

**Change to:**
```c
// Re-enabled with HTTP transport (saves ~15KB SSL overhead per request)
if (weather_task_handle == NULL) {
    ESP_LOGI(TAG, "Starting weather task (after Dexcom)...");
    xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
}
```

### Location 2: No Dexcom credentials (line ~5586)

**Find:**
```c
// No Dexcom credentials - weather task DISABLED due to memory issues
ESP_LOGI(TAG, "No Dexcom credentials found (weather task disabled for memory)");
// DISABLED: Weather task causes severe memory fragmentation
```

**Change to:**
```c
// No Dexcom credentials - start weather task immediately
ESP_LOGI(TAG, "No Dexcom credentials found, starting weather task...");
if (weather_task_handle == NULL) {
    xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
}
```

### Location 3: Time/Weather menu (line ~3043)

**Find:**
```c
// DISABLED: Weather task causes severe memory fragmentation (17KB → 536 bytes)
// Start weather update task (check if not already running)
// if (weather_task_handle == NULL) {
//     ESP_LOGI(TAG, "Starting weather update task");
//     xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
// }
```

**Change to:**
```c
// Start weather update task (check if not already running)
if (weather_task_handle == NULL) {
    ESP_LOGI(TAG, "Starting weather update task");
    xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
}
```

---

## Remove Unused Variable Warning

**File:** `main/main.c`
**Around line:** 128

### Find:
```c
static lv_obj_t *screen_alarm_preview = NULL;
```

### Change to:
```c
// Unused - preview feature removed
// static lv_obj_t *screen_alarm_preview = NULL;
```

---

## Build & Test

```bash
cd C:\Projects\CYGM
idf.py build flash monitor -p COM40
```

---

## Expected Serial Output

```
I (xxxxx) CYGM: Starting weather task (after Dexcom)...
I (xxxxx) CYGM: Weather update task started
I (xxxxx) CYGM: Waiting 60 seconds before first weather attempt (allow memory to stabilize)...
I (xxxxx) CYGM: Free heap before weather geocoding: XXXXX bytes
I (xxxxx) CYGM: Geocoding zipcode: 98092 (time is synced)
I (xxxxx) CYGM: Location found: 47.3094, -122.1745
I (xxxxx) CYGM: Zipcode 98092 -> Lat: 47.3094, Lon: -122.1745, Location: Auburn, WA
I (xxxxx) CYGM: Reverse geocoding successful
```

### What to Watch For:

**Good Signs:**
- Free heap stays above 10KB during weather operations
- No DNS errors
- "Location found" messages appear
- Weather icon displays correctly

**Bad Signs (need more optimization):**
- Free heap drops below 5KB
- DNS failures: "couldn't get hostname"
- Device crashes or reboots

---

## Memory Impact

### Before (HTTPS):
```
Each HTTPS request: ~20KB SSL context
3 attempts: ~60KB peak usage
Memory fragmentation: Severe (17KB → 536 bytes)
Result: Crashes
```

### After (HTTP):
```
Each HTTP request: ~5KB temporary
3 attempts: ~15KB peak usage
Memory fragmentation: Minimal
Result: Should be stable
```

**Expected free heap during weather:** ~135KB (vs 536 bytes before)

---

## If Still Crashes

If you still see crashes or DNS failures after switching to HTTP:

### Option 1: Reduce LVGL
```bash
# Edit sdkconfig
CONFIG_LV_MEM_SIZE_KILOBYTES=32  # Currently 36
```

### Option 2: Switch to OpenWeatherMap
See: `docs/development/WEATHER_SOLUTIONS_RESEARCH_2026-01-28.md` - Solution 2

### Option 3: mbedTLS Optimizations
See: Weather Solutions doc - Solution 3

---

## Rollback Instructions

If weather causes problems, disable it again:

```c
// Comment out weather task creation at all 3 locations
// if (weather_task_handle == NULL) {
//     xTaskCreate(weather_update_task, "weather_update", 4096, NULL, 1, &weather_task_handle);
// }
```

---

## Success Criteria

✅ Weather geocoding completes successfully
✅ Location displays correctly (e.g., "Auburn, WA")
✅ Weather icon updates
✅ Free heap stays above 10KB
✅ No device crashes or reboots
✅ Glucose fetching continues normally

---

*Quick Start Guide Created: 2026-01-28*
*Estimated Time: 10 minutes*
*Expected Result: Weather working without crashes*
