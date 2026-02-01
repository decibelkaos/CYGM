# SSL Memory Allocation Fix - CRITICAL

## Date: January 27, 2026
## Status: ✅ RESOLVED

---

## Problem Summary

### Symptoms
- Weather API (HTTPS) worked perfectly ✅
- Dexcom API (HTTPS) failed immediately with error: `mbedtls_ssl_setup returned -0x7F00` ❌
- Error code `-0x7F00` = `MBEDTLS_ERR_SSL_ALLOC_FAILED` (memory allocation failure)
- Credentials were not saved to NVS (because authentication failed before reaching save point)

### Root Cause Analysis

**ESP32 was running out of heap memory for SSL connections:**

1. **Static SSL Buffer Allocation** (BEFORE FIX):
   - Each HTTPS connection allocated **20KB of contiguous memory**:
     - `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` (16KB input buffer)
     - `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` (4KB output buffer)
   - Weather task started first → grabbed first 20KB ✅
   - Dexcom task tried to connect → no 20KB available ❌

2. **Static Peer Certificate Storage**:
   - `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y` kept certificates in memory (~2KB each)
   - Added to total memory pressure

3. **No Dynamic Memory Management**:
   - Buffers allocated at connection start, held until connection closed
   - Memory fragmentation over time
   - No automatic cleanup of unused SSL data

### Why Weather Worked But Dexcom Failed

**Sequence of Events:**
1. Device boots → ~220KB free heap
2. Weather task starts → allocates 20KB → succeeds ✅
3. Weather data fetches → SSL connection uses 20KB
4. Dexcom login attempt → tries to allocate another 20KB → **OUT OF MEMORY** ❌

---

## Solution

### Changes Made to `sdkconfig`

**1. Reduced SSL Buffer Sizes** (Saved 12KB per connection):
```
# BEFORE:
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384   # 16KB
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096   # 4KB
Total per connection: 20KB

# AFTER:
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096    # 4KB (minimum for TLS)
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096   # 4KB
Total per connection: 8KB ← 60% reduction!
```

**2. Enabled Dynamic SSL Buffers**:
```
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
```

**What This Does:**
- Buffers start small and grow only as needed
- Unused buffers are freed immediately after handshake
- Peer certificates released after verification
- Config data freed when not in use

**3. Disabled Peer Certificate Retention** (Saved ~2KB per connection):
```
# BEFORE:
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y

# AFTER:
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set
```

### Total Memory Savings

| Configuration | Memory Per Connection | Notes |
|---------------|----------------------|-------|
| **BEFORE** | 20KB + 2KB cert = **22KB** | Static allocation |
| **AFTER** | 4-8KB dynamic | Freed immediately after use |
| **Savings** | **~14KB per connection** | 63% reduction |

---

## Verification

### Test Results (2026-01-27 15:49:19)

```
✅ Weather API: SUCCESS
   - Geocoding: 47.2928, -122.1707
   - Weather: 47°F (Hi: 49°F, Lo: 33°F), Cloudy
   - SSL connection established

✅ Dexcom API: SUCCESS
   - Account ID: 0052f8e5...
   - Session ID: 6ae47096...
   - Authentication: PASSED
   - Credentials saved to NVS
   - Glucose: 88 mg/dL, Trend: Steady
   - SSL connection established

✅ Both APIs working simultaneously!
```

### Log Evidence

**Before Fix:**
```
E (93337) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (93338) esp-tls: create_ssl_handle failed
E (93338) esp-tls: Failed to open new connection
```

**After Fix:**
```
I (205866) DEXCOM: Result: ESP_OK (status 200)
I (205869) DEXCOM: Got account ID: 0052f8e5...
I (208097) DEXCOM: Got session ID: 6ae47096...
I (208100) NVS_CONFIG: Dexcom credentials saved to NVS
I (208101) DEXCOM: Dexcom authentication successful on US region!
```

---

## Technical References

### ESP-IDF Documentation
- [MbedTLS Memory Management](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mbedtls.html)
- [Minimizing RAM Usage](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/ram-usage.html)
- [ESP-FAQ: Mbed TLS](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/mbedtls.html)

### Community Resources
- [ESP32 Forum: RAM for mbedTLS](https://esp32.com/viewtopic.php?t=4329)
- [GitHub: SSL Memory Allocation Issues](https://github.com/espressif/arduino-esp32/issues/7884)
- [ESP32 Forum: HTTPS Memory Usage](https://www.esp32.com/viewtopic.php?t=32894)

### Key Findings from Research
1. **HTTPS requires 40-50KB of free heap** during TLS handshake
2. **4KB is the minimum buffer size** for TLS 1.2 protocol
3. **Dynamic buffers are essential** for devices with multiple HTTPS connections
4. **Peer certificate retention** can consume significant memory over time

---

## Important Notes for Future Development

### ⚠️ DO NOT Change These Settings
The following `sdkconfig` settings are **critical** for SSL to work:

```bash
# SSL Buffer Sizes (4KB minimum for TLS)
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096

# Dynamic Buffer Management (REQUIRED for multiple connections)
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y

# Peer Certificate (DO NOT ENABLE)
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set

# Skip Certificate Verification (already enabled for Dexcom)
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

### If Adding More HTTPS Connections

**Current working connections:**
1. Weather API (open-meteo.com, nominatim.openstreetmap.org)
2. Dexcom API (share2.dexcom.com, shareous1.dexcom.com)

**If adding a 3rd HTTPS API:**
- Monitor free heap with `heap_caps_get_free_size(MALLOC_CAP_8BIT)`
- Ensure at least **60KB free heap** before initiating connection
- Consider staggering connections (don't open all simultaneously)
- May need to reduce buffer sizes further or enable PSRAM

### Memory Monitoring Commands

```c
// Check free heap before SSL connection
ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Largest free block: %d bytes",
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

---

## Credentials Now Save to NVS

**Why credentials weren't saving before:**
The code only saves credentials **after successful authentication**:

```c
// In dexcom_api.c:474-483
success:
    // Save credentials to NVS
    nvs_set_dexcom_credentials(username, password);  // ← Only executes on success

    is_authenticated = true;
    ESP_LOGI(TAG, "Dexcom authentication successful!");
    return ESP_OK;
```

Since authentication was failing due to SSL memory errors, it never reached the `nvs_set_dexcom_credentials()` call. Now that authentication succeeds, credentials are saved automatically.

---

## Summary

**Problem:** ESP32 ran out of heap memory for SSL connections due to large static buffer allocation.

**Solution:** Reduced SSL buffer sizes from 20KB to 8KB per connection and enabled dynamic memory management.

**Result:** Both Weather and Dexcom APIs work simultaneously with credentials properly saved to NVS.

**Key Lesson:** Multiple HTTPS connections on ESP32 require dynamic SSL buffers and careful memory management.

---

✅ **Issue Resolved: January 27, 2026**
