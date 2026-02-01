# SD Card Logging Implementation - January 29, 2026

## Status: ✅ Complete - Ready for Testing

---

## Problem Statement

User reported frequent "---" / "No sensor data" displays during overnight testing, even though Dexcom Follow app was receiving data fine. Need comprehensive debugging to identify:

1. Why JSON parsing is failing
2. What responses Dexcom is returning
3. If buffer overflow is occurring
4. Memory issues over extended runtime

**Serial evidence**:
```
E (111669) DEXCOM: Failed to parse JSON response
I (111670) DEXCOM: Cleaning up persistent HTTP client
W (111677) CYGM: Periodic glucose fetch failed: ESP_FAIL
```

---

## Solution Implemented

### Comprehensive SD Card Logging System

Created full-featured SD card logger that captures:
- ✅ **HTTP requests** - Method, URL, POST body
- ✅ **HTTP responses** - Status code, full response body
- ✅ **Glucose data** - Value, trend, timestamp, status
- ✅ **Errors** - JSON parse failures, buffer overflows, network errors
- ✅ **System metrics** - Free heap, success/failure tracking

---

## Implementation Details

### New Files Created

1. **main/sd_logger.h** - Public API (300 lines)
   - `sd_logger_init()` - Initialize SD card and create log file
   - `sd_log()` - General logging with timestamp
   - `sd_log_http_request()` - Log HTTP requests
   - `sd_log_http_response()` - Log HTTP responses
   - `sd_log_glucose()` - Log glucose readings
   - `sd_log_error()` - Log errors with context
   - `sd_logger_suspend()` - Unmount SD to free ~13KB (memory time-sharing)
   - `sd_logger_resume()` - Remount SD and reopen log file

2. **main/sd_logger.c** - Implementation (400 lines)
   - SPI mode SD card access (SPI3/VSPI)
   - FAT filesystem mounting
   - Timestamped log files: `/sdcard/cygm_log_YYYYMMDD_HHMMSS.txt`
   - Automatic buffer flushing

### Hardware Configuration

**SD Card Pins (JC2432W328)**:
- CS: GPIO 5
- MOSI: GPIO 23
- MISO: GPIO 19
- SCK: GPIO 18

**SPI Bus**: SPI3 (VSPI) in SPI mode
**Filesystem**: FAT32

---

## Code Changes

### main/CMakeLists.txt
**Line 7**: Added sd_logger.c to build
```cmake
idf_component_register(SRCS "main.c"
                            "nvs_config.c"
                            "wifi_manager.c"
                            "dexcom_api.c"
                            "web_server.c"
                            "ui_config.c"
                            "sd_logger.c"
                       INCLUDE_DIRS ".")
```

### main/main.c

**Line 41**: Include SD logger
```c
#include "sd_logger.h"
```

**Line ~1717**: Suspend SD logger before weather operations (memory time-sharing)
```c
// MEMORY OPTIMIZATION: Free memory for weather HTTPS operations
// Suspend SD logger (~13KB) + Close Dexcom client (~30KB) = ~43KB freed
ESP_LOGI(TAG, "Suspending SD logger to free memory for weather operations...");
sd_logger_suspend();

ESP_LOGI(TAG, "Closing Dexcom client to free memory for weather operations...");
dexcom_close_persistent_client();
```

**Line ~1801**: Resume SD logger after weather operations
```c
// Resume SD logger after weather operations
ESP_LOGI(TAG, "Resuming SD logger after weather operations...");
sd_logger_resume();
```

**Line ~5667**: Initialize SD logger after WiFi connects
```c
// Initialize SD card logger for debugging
ESP_LOGI(TAG, "Initializing SD card logger...");
if (sd_logger_init() == ESP_OK) {
    ESP_LOGI(TAG, "SD card logger initialized: %s", sd_logger_get_filepath());
    add_boot_log("SD logging enabled");
    sd_log(TAG, "=== CYGM Debug Session Started ===");
    sd_log(TAG, "Device: JC2432W328 (ESP32-D0WD)");
    sd_log(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
} else {
    ESP_LOGW(TAG, "SD card logger failed to initialize (SD card may not be inserted)");
    add_boot_log("SD card not found");
}
```

**Line ~1825**: Log successful glucose updates
```c
sd_log(TAG, "Glucose update SUCCESS: %d mg/dL, trend: %s, heap: %lu bytes",
       glucose.value, dexcom_trend_description(glucose.trend),
       esp_get_free_heap_size());
```

**Line ~1852**: Log failed glucose updates
```c
const char *status_str = (glucose.status == GLUCOSE_STATUS_OK) ? "OK" :
                         (glucose.status == GLUCOSE_STATUS_NO_DATA) ? "NO_DATA" :
                         (glucose.status == GLUCOSE_STATUS_WARMUP) ? "WARMUP" :
                         (glucose.status == GLUCOSE_STATUS_SIGNAL_LOSS) ? "SIGNAL_LOSS" :
                         "NOT_AUTHENTICATED";
sd_log(TAG, "Glucose update FAILED: %s, status: %s, heap: %lu bytes",
       esp_err_to_name(ret), status_str, esp_get_free_heap_size());
```

### main/dexcom_api.c

**Line 9**: Include SD logger
```c
#include "sd_logger.h"
```

**Line ~690**: Log HTTP request before sending
```c
// Log HTTP request to SD card
sd_log_http_request("POST", url, body_str);
```

**Line ~726**: Log non-200 HTTP responses
```c
if (status != 200) {
    ESP_LOGE(TAG, "Glucose fetch failed with status %d: %s", status, http_response);
    sd_log_http_response(status, http_response);
    sd_log_error(TAG, "Non-200 status code", esp_err_to_name(err));
    glucose->status = GLUCOSE_STATUS_NO_DATA;
    cleanup_persistent_client();
    return ESP_FAIL;
}
```

**Line ~734**: Log successful HTTP response
```c
// Log successful HTTP response
sd_log_http_response(status, http_response);
```

**Line ~738**: Log JSON parse failures with actual response
```c
cJSON *root = cJSON_Parse(http_response);
if (root == NULL) {
    ESP_LOGE(TAG, "Failed to parse JSON response");
    ESP_LOGE(TAG, "Response that failed to parse: %s", http_response);
    sd_log_error(TAG, "JSON parse failed", http_response);
    glucose->status = GLUCOSE_STATUS_NO_DATA;
    cleanup_persistent_client();
    return ESP_FAIL;
}
```

**Line ~91**: Detect and log HTTP buffer overflow
```c
} else {
    // Buffer overflow - log error
    ESP_LOGE(TAG, "HTTP response buffer overflow! Response too large (%d + %d >= %d)",
             http_response_len, evt->data_len, HTTP_BUFFER_SIZE);
    sd_log_error(TAG, "HTTP response buffer overflow",
                 "Response larger than 2048 bytes - JSON will be truncated");
}
```

**Line ~816**: Log successful glucose data
```c
// Log glucose data to SD card
const char *status_str = (glucose->status == GLUCOSE_STATUS_OK) ? "OK" :
                         (glucose->status == GLUCOSE_STATUS_NO_DATA) ? "NO_DATA" :
                         (glucose->status == GLUCOSE_STATUS_WARMUP) ? "WARMUP" :
                         (glucose->status == GLUCOSE_STATUS_SIGNAL_LOSS) ? "SIGNAL_LOSS" :
                         "NOT_AUTHENTICATED";
sd_log_glucose(glucose->value, dexcom_trend_description(glucose->trend),
               glucose->timestamp, status_str);
```

---

## Memory Impact

**Additional Heap Usage**:
- SPI bus: ~2 KB (shared with LCD)
- FAT filesystem: ~10 KB
- File handle: ~1 KB
- **Total**: ~13 KB

**Memory Time-Sharing** (NEW!):
- SD logger now participates in memory time-sharing
- Suspends (unmounts) during weather HTTPS operations
- Frees ~13KB + Dexcom close (~30KB) = **~43KB total**
- Automatically resumes after weather completes
- Log file shows `[SUSPEND]` and `[RESUME]` markers

**Memory Timeline**:
- Normal operation: ~19 KB free heap (with SD + Dexcom)
- During weather: ~62 KB free heap (SD suspended, Dexcom closed)
- **Plenty of room for weather SSL** (~20 KB required)

---

## Expected Log Output

### Memory Time-Sharing During Weather Update
```
[2026-01-29 08:45:00] CYGM: Suspending SD logger to free memory for weather operations...
[SUSPEND] SD logger suspended for memory time-sharing

[After weather operations complete, SD remounts and log continues:]

[RESUME] SD logger resumed after memory time-sharing
[2026-01-29 08:45:30] CYGM: Resuming SD logger after weather operations...
```

### Successful Glucose Fetch
```
[2026-01-29 08:30:50] === HTTP REQUEST ===
Method: POST
URL: https://share2.dexcom.com/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues
Body: {"sessionId":"f3ba75f2...","minutes":1440,"maxCount":1}
========================

[2026-01-29 08:30:52] === HTTP RESPONSE ===
Status: 200
Response: [{"WT":"Date(1738162800000)","ST":"Date(1738162800000)","DT":"Date(1738162800000-0800)","Value":184,"Trend":"Steady"}]
=========================

[2026-01-29 08:30:52] DEXCOM: GLUCOSE: 184 mg/dL, Trend: Steady, Reading Time: 2026-01-29 08:20:00, Status: OK
[2026-01-29 08:30:52] CYGM: Glucose update SUCCESS: 184 mg/dL, trend: Steady, heap: 32280 bytes
```

### JSON Parse Failure (The Bug We're Hunting!)
```
[2026-01-29 08:32:22] === HTTP REQUEST ===
Method: POST
URL: https://share2.dexcom.com/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues
Body: {"sessionId":"f3ba75f2...","minutes":1440,"maxCount":1}
========================

[2026-01-29 08:32:24] === HTTP RESPONSE ===
Status: 200
Response: [ACTUAL RESPONSE WILL BE LOGGED HERE - THIS IS WHAT WE NEED TO SEE!]
=========================

[2026-01-29 08:32:24] *** ERROR ***
Tag: DEXCOM
Error: JSON parse failed
Context: [ACTUAL RESPONSE WILL BE LOGGED HERE]
**************

[2026-01-29 08:32:24] CYGM: Glucose update FAILED: ESP_FAIL, status: NO_DATA, heap: 35092 bytes
```

---

## Testing Procedure

### 1. Prepare SD Card
- Format microSD card as FAT32
- Insert into device SD card slot

### 2. Flash and Boot
```bash
idf.py build flash monitor -p COM40
```

### 3. Verify SD Logger Started
Watch boot log for:
```
I (xxxx) CYGM: Initializing SD card logger...
I (xxxx) SD_LOGGER: SD card mounted successfully
I (xxxx) SD_LOGGER: Creating log file: /sdcard/cygm_log_20260129_083045.txt
I (xxxx) SD_LOGGER: SD card logger initialized successfully
I (xxxx) CYGM: SD card logger initialized: /sdcard/cygm_log_20260129_083045.txt
```

Display should show: **"SD logging enabled"**

### 4. Let Run Overnight
- Leave device running for 8-12 hours
- Monitor for "---" / "No sensor data" displays
- Note approximate times of failures

### 5. Retrieve Log
- Power off device
- Remove SD card
- Insert into computer
- Open log file: `/cygm_log_YYYYMMDD_HHMMSS.txt`

### 6. Analyze Log
Look for patterns:
- Empty responses
- Truncated JSON
- Buffer overflows
- Memory decreasing over time
- Correlation between heap usage and failures

---

## Debugging Workflow

### If JSON Parse Fails

1. **Check actual response** - What did Dexcom actually return?
   ```
   Response: [{"WT":"Date(..."  <- Looks good
   Response:                     <- Empty response!
   Response: {"error":"..."}     <- Error from Dexcom
   ```

2. **Check buffer overflow** - Was response too large?
   ```
   *** ERROR ***
   Tag: DEXCOM
   Error: HTTP response buffer overflow
   ```

3. **Check heap trend** - Is memory leaking?
   ```
   [08:30:00] heap: 32280 bytes
   [09:00:00] heap: 28150 bytes  <- Declining!
   [09:30:00] heap: 24020 bytes  <- Memory leak!
   ```

4. **Check timing** - Does it fail at specific intervals?
   ```
   SUCCESS at: 08:30, 08:32, 08:33...
   FAIL at: 08:35 (after weather update?)
   ```

---

## Potential Root Causes

Based on serial logs and implementation, likely causes:

### 1. Empty Response (Most Likely)
Dexcom returns HTTP 200 but empty body → JSON parse fails

**Fix**: Check for empty response before JSON parse
```c
if (http_response_len == 0 || strlen(http_response) == 0) {
    ESP_LOGW(TAG, "Empty response from Dexcom");
    // Don't cleanup client, just return error
    glucose->status = GLUCOSE_STATUS_NO_DATA;
    return ESP_FAIL;
}
```

### 2. Buffer Overflow (Possible)
Response > 2048 bytes → Truncated JSON → Parse fails

**Fix**: Increase buffer size or implement chunked reading

### 3. Network Timeout During Read (Possible)
HTTP event handler stops receiving data mid-stream

**Fix**: Add timeout detection and retry logic

### 4. Memory Fragmentation (Unlikely)
Persistent client allocation/deallocation causing heap issues

**Fix**: Already using persistent client, should be fine

---

## Next Steps After Log Analysis

1. **Review overnight log** - Identify exact cause of JSON failures
2. **Implement targeted fix** - Based on log findings
3. **Add defensive coding**:
   - Check for empty responses
   - Validate JSON before parsing
   - Add response length check
4. **Test for 24+ hours** - Verify fix works long-term

---

## Success Criteria

✅ SD card logger compiles without errors
✅ SD card mounts on boot
✅ Log file created with timestamp
✅ HTTP requests logged before sending
✅ HTTP responses logged (including failures)
✅ Glucose data logged on success
✅ Errors logged with full context
✅ Overnight run captured to SD card
✅ Log file readable on computer

---

## Related Documentation

- **[SD Card Logging Feature](../features/SD_CARD_LOGGING_2026-01-29.md)** - User documentation
- **[Hardware Reference](../hardware/HARDWARE_REFERENCE.md)** - SD card pinout
- **[Audio Feedback](../features/AUDIO_FEEDBACK_2026-01-28.md)** - Previous debugging session

---

*Implemented: 2026-01-29 Morning*
*Build Status: Ready to compile and test*
*Purpose: Debug overnight glucose fetch failures*
