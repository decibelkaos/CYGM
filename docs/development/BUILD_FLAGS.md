# CYGM Build Configuration Flags

This document describes compile-time feature flags that can be enabled/disabled before building the firmware.

## Location
All build flags are defined in: `main/main.c` (top of file, after includes)

## Available Flags

### 1. ENABLE_WEB_SERVER
**Current Value**: `0` (DISABLED)

**Purpose**: Enables web-based configuration interface for device settings.

**Memory Impact**:
- Enabled: Uses ~30KB heap for HTTP server and web content
- Disabled: Saves 30KB for critical SSL operations (CGM/Weather)

**When to Enable**:
- When you need to configure WiFi, time, weather, or CGM settings via web browser
- During development for easier testing
- When memory is not constrained

**How to Enable**:
```c
#define ENABLE_WEB_SERVER 1  // Change from 0 to 1
```

---

### 2. ENABLE_SD_LOGGING
**Current Value**: `0` (DISABLED)

**Purpose**: Enables SD card logging for glucose data, HTTP requests/responses, and system events.

**Memory Impact**:
- Enabled: Uses ~15KB heap constantly for FAT filesystem
- Disabled: Saves 15KB for critical SSL operations (CGM/Weather)

**When to Enable**:
- During debugging to capture detailed logs
- When troubleshooting overnight issues
- When you need to analyze HTTP traffic or glucose fetch patterns

**When to Keep Disabled**:
- Production use (prioritize CGM reliability)
- When running on marginal memory (~18-20KB free)
- When SSL allocation errors occur

**How to Enable**:
```c
#define ENABLE_SD_LOGGING 1  // Change from 0 to 1
```

**Future Upgrade Path** (Option 2 - Smart On-Demand Logging):
Currently, SD card stays mounted all the time (15KB overhead). Future implementation will:
1. Mount SD card only when writing logs
2. Unmount immediately after write (frees 15KB)
3. Check heap before mounting (skip logging if < 30KB threshold)
4. Batch writes every 5 minutes
5. Gracefully degrade: CGM/Weather always prioritized over logging

See comments in `main/main.c` (near `ENABLE_SD_LOGGING` flag) for upgrade instructions.

---

### 3. CLEAR_WIFI_CREDENTIALS
**Current Value**: `0` (DISABLED)

**Purpose**: Erases saved WiFi credentials from NVS on boot (for testing/reset).

**When to Enable**:
- When you need to re-enter WiFi credentials
- During development/testing
- When switching to a different network

**How to Enable**:
```c
#define CLEAR_WIFI_CREDENTIALS 1  // Change from 0 to 1
```

**IMPORTANT**: Change back to `0` after first boot, otherwise credentials will be cleared every boot!

---

## Build Process

After changing any flag:

1. Save `main/main.c`
2. Rebuild firmware:
   ```bash
   idf.py build
   ```
3. Flash to device:
   ```bash
   idf.py flash monitor -p COM40
   ```

---

## Memory Guidelines

**Total ESP32 Heap**: ~240KB at boot
**After initialization**: ~87KB available
**After WiFi + Dexcom auth**: ~28KB free
**Minimum for stable operation**: ~18KB free

**Memory Budget**:
- Dexcom persistent SSL client: ~30KB (when active)
- Weather geocoding SSL: ~30KB (one-time, then cached)
- Weather HTTP fetch: ~5KB (no SSL after geocoding)
- Web server: ~30KB (if enabled)
- SD card FAT filesystem: ~15KB (if enabled)

**Recommendations**:
- Keep web server disabled in production (saves 30KB)
- Keep SD logging disabled unless debugging (saves 15KB)
- Enable features one at a time to test memory impact
- Monitor heap with logs: Look for "Free heap" messages

---

## SD Card Logging Details

### What Gets Logged (when enabled):
1. **Glucose readings**: Value, trend, timestamp, status
2. **HTTP requests**: Method, URL, request body
3. **HTTP responses**: Status code, response body (truncated if > 2KB)
4. **Errors**: Authentication failures, network issues, JSON parse errors
5. **Session events**: Boot time, auth success, task starts

### Log File Location:
- Mount point: `/sdcard/`
- Filename format: `MMDDYY.log` (8.3 FAT32 format)
- Example: `013026.log` for January 30, 2026

### Log File Format:
```
[YYYY-MM-DD HH:MM:SS] TAG: Message

[2026-01-30 14:52:00] DEXCOM: Glucose: 127 mg/dL, Trend: Rising slowly
[2026-01-30 14:52:05] CYGM: Weather updated: 48 F (Hi: 50 F, Lo: 44 F)
```

### Troubleshooting SD Card:
- Ensure SD card is inserted before boot
- Use FAT32 formatted card (not exFAT or NTFS)
- Maximum 32GB card size recommended
- If init fails, device continues without logging (graceful degradation)

---

## Version History

- **v1.0** (2026-01-30): Initial flags (Web server, SD logging, WiFi clear)
- Option 2 (Smart SD logging): Planned for future release

---

## Support

For issues or questions:
- Check serial monitor output for errors
- Review memory usage logs
- Disable non-critical features if SSL errors occur
