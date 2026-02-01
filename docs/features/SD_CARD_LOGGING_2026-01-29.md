# SD Card Logging System

## Summary

✅ **SD card logger implemented** - All HTTP requests, responses, errors, and glucose data logged to SD card for debugging
✅ **JSON parse failure tracking** - Captures actual response when parsing fails
✅ **Buffer overflow detection** - Logs when HTTP response exceeds buffer size
✅ **Overnight test ready** - Comprehensive logging for multi-hour debugging

---

## Problem

User reported frequent "---" / "No sensor data" displays even though Dexcom Follow app was receiving data fine. Need to debug what's causing JSON parsing failures and missed glucose updates overnight.

---

## Solution

Implemented comprehensive SD card logging system that records:
1. **HTTP requests** - Method, URL, POST body
2. **HTTP responses** - Status code, full response body
3. **Glucose data** - Value, trend, timestamp, status
4. **Errors** - JSON parse failures, buffer overflows, network errors
5. **System events** - Successful/failed updates, heap usage

---

## Hardware

### SD Card Slot (JC2432W328)

| Pin | GPIO | Function |
|-----|------|----------|
| CS  | 5    | Chip Select |
| MOSI | 23  | Data to card |
| MISO | 19  | Data from card |
| SCK  | 18  | Clock |

**Interface**: SPI3 (VSPI) in SPI mode (not SDMMC mode)
**Voltage**: 3.3V logic level
**Card**: MicroSD/TF card

---

## Implementation

### Files Created

**main/sd_logger.h** - SD card logger public API
```c
esp_err_t sd_logger_init(void);
void sd_log(const char *tag, const char *format, ...);
void sd_log_http_request(const char *method, const char *url, const char *body);
void sd_log_http_response(int status_code, const char *response);
void sd_log_glucose(int value, const char *trend, time_t timestamp, const char *status);
void sd_log_error(const char *tag, const char *error, const char *context);
```

**main/sd_logger.c** - SD card logger implementation
- Uses ESP-IDF VFS FAT filesystem
- SPI mode for SD card (SDSPI)
- Creates timestamped log files: `/sdcard/cygm_log_YYYYMMDD_HHMMSS.txt`
- Automatic buffer flushing after each write

---

## Integration Points

### main/main.c (app_main)

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
sd_log(TAG, "Glucose update FAILED: %s, status: %s, heap: %lu bytes",
       esp_err_to_name(ret), status_str, esp_get_free_heap_size());
```

### main/dexcom_api.c

**Line ~9**: Include SD logger
```c
#include "sd_logger.h"
```

**Line ~690**: Log HTTP requests
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
    // ...
}
```

**Line ~734**: Log successful HTTP responses
```c
// Log successful HTTP response
sd_log_http_response(status, http_response);
```

**Line ~738**: Log JSON parse failures
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

**Line ~816**: Log successful glucose readings
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

**Line ~91**: Log HTTP buffer overflow
```c
} else {
    // Buffer overflow - log error
    ESP_LOGE(TAG, "HTTP response buffer overflow! Response too large (%d + %d >= %d)",
             http_response_len, evt->data_len, HTTP_BUFFER_SIZE);
    sd_log_error(TAG, "HTTP response buffer overflow",
                 "Response larger than 2048 bytes - JSON will be truncated");
}
```

---

## Log File Format

### Example Log Content

```
=== CYGM Debug Log Started ===
Timestamp: 2026-01-29 08:30:45
=================================

[2026-01-29 08:30:45] CYGM: === CYGM Debug Session Started ===
[2026-01-29 08:30:45] CYGM: Device: JC2432W328 (ESP32-D0WD)
[2026-01-29 08:30:45] CYGM: Free heap: 35240 bytes

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

[2026-01-29 08:32:22] === HTTP REQUEST ===
Method: POST
URL: https://share2.dexcom.com/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues
Body: {"sessionId":"f3ba75f2...","minutes":1440,"maxCount":1}
========================

[2026-01-29 08:32:24] === HTTP RESPONSE ===
Status: 200
Response:
=========================

[2026-01-29 08:32:24] *** ERROR ***
Tag: DEXCOM
Error: JSON parse failed
Context:
**************

[2026-01-29 08:32:24] CYGM: Glucose update FAILED: ESP_FAIL, status: NO_DATA, heap: 35092 bytes
```

---

## Usage

### 1. Insert SD Card
- Power off device
- Insert microSD/TF card into slot
- Power on device

### 2. Check Boot Log
After WiFi connects, display will show either:
- ✅ "SD logging enabled" - Logger working
- ⚠️ "SD card not found" - No SD card or card error

### 3. Let Device Run Overnight
The logger will automatically capture:
- Every glucose fetch attempt (every 90 seconds)
- All HTTP requests and responses
- Any errors or failures
- Memory usage over time

### 4. Retrieve Log File
- Power off device
- Remove SD card
- Insert into computer
- Log file: `/cygm_log_YYYYMMDD_HHMMSS.txt`

---

## Debugging JSON Parse Failures

When JSON parsing fails, the log will show:
1. **HTTP Request** - What was sent to Dexcom
2. **HTTP Response** - Actual response body that failed to parse
3. **Error Entry** - Context of the failure

### Common Causes of Parse Failures:

1. **Empty response** - Dexcom returns empty string
   ```
   Response:
   ```

2. **Truncated response** - Buffer overflow (response > 2048 bytes)
   ```
   *** ERROR ***
   Tag: DEXCOM
   Error: HTTP response buffer overflow
   Context: Response larger than 2048 bytes - JSON will be truncated
   ```

3. **Invalid JSON** - Malformed response from server
   ```
   Response: {"error":"Invalid session"}
   ```

4. **Rate limiting** - Dexcom blocking requests
   ```
   Response: Too many requests
   ```

---

## Memory Usage

**SD Logger Overhead**:
- SPI bus initialization: ~2 KB (shared with LCD)
- FAT filesystem mount: ~10 KB
- Log file handle: ~1 KB
- **Total**: ~13 KB additional heap usage

**Memory Time-Sharing**:
- SD logger automatically suspends during weather HTTPS operations
- Unmounts filesystem and closes file to free ~13KB
- Combined with Dexcom client close (~30KB) = **~43KB free for weather SSL**
- Automatically resumes after weather operations complete
- Log file includes `[SUSPEND]` and `[RESUME]` markers

**Heap tracking**: Each log entry includes current free heap, allowing you to track memory leaks or fragmentation over time.

---

## File Size Estimates

**Average log entry**: ~250 bytes
**Glucose update cycle** (90 seconds): ~600 bytes
- HTTP request log: ~200 bytes
- HTTP response log: ~300 bytes
- Glucose data log: ~100 bytes

**24-hour run**:
- 960 glucose cycles × 600 bytes = ~576 KB
- Plus errors, system events: ~600 KB total
- **Most SD cards**: 512 MB - 32 GB (plenty of space!)

---

## Troubleshooting

### SD Card Not Detected

**Check**:
1. SD card properly inserted
2. SD card formatted as FAT32
3. SD card not write-protected
4. GPIO pins not conflicting with other peripherals

**Serial log shows**:
```
E (xxxx) SD_LOGGER: Failed to mount filesystem. Card not formatted or not inserted.
```

**Solution**:
- Format SD card as FAT32 on computer
- Try different SD card
- Check card contacts

### Log File Empty

**Possible causes**:
1. SD logger initialized but failed silently
2. No glucose fetches attempted
3. SD card write-protected

**Check**:
- Boot log shows "SD logging enabled"
- Device shows glucose data on screen
- SD card not locked

---

## Future Enhancements

1. **Log rotation** - Auto-create new file after 24 hours or 1 MB
2. **Web download** - Retrieve logs via web interface
3. **Real-time streaming** - View logs over WiFi
4. **Crash dumps** - Automatic logging of panics/resets
5. **Performance metrics** - Track SSL handshake times, request latencies

---

## Related Files

- **Hardware**: `docs/hardware/HARDWARE_REFERENCE.md` (SD card pinout)
- **Dexcom API**: `main/dexcom_api.c` (HTTP request/response handling)
- **Main**: `main/main.c` (Glucose update task)

---

*Implemented: 2026-01-29*
*Status: Ready for testing*
*Purpose: Debug overnight glucose fetch failures*
