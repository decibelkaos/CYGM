# Savepoint: SSL/TLS Configuration Fix
**Date:** 2026-01-25
**Status:** ✅ Working - All APIs functional

## What Was Fixed

Fixed SSL certificate verification issues preventing geocoding and weather APIs from working on boot.

## Problem

On device reboot, geocoding and weather APIs were failing with SSL errors:
```
E (26186) esp-tls-mbedtls: No server verification option set in esp_tls_cfg_t structure
E (31855) esp-x509-crt-bundle: PK verify failed with error 0x4290
E (31856) esp-x509-crt-bundle: Certificate matched but signature verification failed
```

## Root Cause

- OpenStreetMap Nominatim API and Open-Meteo API certificates were failing signature verification
- ESP32's certificate bundle couldn't validate these specific certificates
- Missing ESP-TLS configuration flags to allow skipping verification

## Solution Implemented

### 1. ESP-TLS Configuration (sdkconfig & sdkconfig.defaults)
Added two critical flags:
```
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

### 2. HTTP Client Configuration (main.c)

**Geocoding APIs (Non-critical data):**
```c
esp_http_client_config_t config = {
    .url = url,
    .event_handler = http_event_handler,
    .timeout_ms = 10000,
    .user_agent = "CYGM/1.0",
    .transport_type = HTTP_TRANSPORT_OVER_SSL  // Skip cert verification
};
```

**Dexcom API (Critical health data):**
```c
esp_http_client_config_t config = {
    .url = url,
    .method = HTTP_METHOD_POST,
    .event_handler = http_event_handler,
    .crt_bundle_attach = esp_crt_bundle_attach,  // Full cert verification
    .timeout_ms = 15000,
};
```

## Security Architecture

| API | SSL | Certificate Verification | Rationale |
|-----|-----|-------------------------|-----------|
| **Dexcom** | ✅ HTTPS | ✅ Full verification | Critical health data |
| **Geocoding** | ✅ HTTPS | ⚠️ Skipped | Non-critical location lookup |
| **Weather** | ✅ HTTPS | ⚠️ Skipped | Non-critical weather data |

- **All connections are encrypted** (HTTPS)
- **Only Dexcom uses full cert verification** (health data requires highest security)
- **Non-critical APIs skip verification** (to avoid ESP32 cert bundle compatibility issues)

## Files Modified

1. `main/main.c`
   - Line 934: `zipcode_to_latlon()` - Added `HTTP_TRANSPORT_OVER_SSL`
   - Line 1101: `reverse_geocode()` - Added `HTTP_TRANSPORT_OVER_SSL`
   - Line 1219: `fetch_weather()` - Changed from `crt_bundle_attach` to `HTTP_TRANSPORT_OVER_SSL`

2. `sdkconfig`
   - Line 628-629: Enabled `CONFIG_ESP_TLS_INSECURE` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`

3. `sdkconfig.defaults`
   - Added ESP-TLS insecure options for persistence

## Verification

Device now successfully:
- ✅ Authenticates with Dexcom API (full SSL verification)
- ✅ Geocodes zipcode to coordinates
- ✅ Reverse geocodes to city name
- ✅ Fetches weather data with sunrise/sunset times
- ✅ Displays location on home screen
- ✅ All operations work on reboot

## Logs (Success)

```
I (23421) CYGM: Geocoding zipcode: 98092 (time is synced)
I (25181) CYGM: Geocoding response: [{"place_id":353663216...}]
I (25226) CYGM: Zipcode 98092 -> Lat: 47.2928, Lon: -122.1707, Location: King, WA
I (28840) CYGM: Reverse geocode result: Auburn, WA
I (28846) CYGM: Reverse geocoding successful
```

## References

- [ESP-IDF GitHub Issue #9587](https://github.com/espressif/esp-idf/issues/9587)
- [ESP32 Forum: Disable Server Validation](https://www.esp32.com/viewtopic.php?t=19200)
- [ESP-TLS Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_tls.html)

## Next Steps

System is fully operational. No further SSL/TLS configuration needed.
