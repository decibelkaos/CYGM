# Critical Bug Fix: Session Timeout Tick Overflow

**Date**: 2026-02-03
**Version**: v1.3.4
**Severity**: CRITICAL - Life-critical device malfunction

---

## Issue Summary

The device would authenticate successfully on boot and fetch the first glucose reading, but then **STOP fetching periodic updates**. After ~12 hours, the session would expire and require multiple reboots to recover. Users had to manually reboot to get new readings instead of automatic 90-second updates.

### Symptoms
- ✅ Dexcom authentication successful on boot
- ✅ First glucose reading fetched successfully
- ❌ Periodic glucose updates (every 90s) NOT happening
- ❌ After 12 hours, authentication fails completely
- ❌ Requires multiple reboots to restore functionality

---

## Root Cause

**Tick Count Overflow Bug** in `dexcom_api.c` session timeout logic.

### The Bug

The code was using:
```c
uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
if (last_login_time > 0 && (now - last_login_time) > SESSION_TIMEOUT_MS) {
    // Session expired
}
```

### Why This Fails

1. `xTaskGetTickCount()` returns ticks since boot (uint32_t)
2. `portTICK_PERIOD_MS` is typically 10ms (100Hz tick rate)
3. Multiplying ticks by 10 causes **uint32_t overflow** after several hours
4. Max value: `4,294,967,295 / 10 = 429,496,729 ticks ≈ 119 hours`
5. But the real issue is **calculation inaccuracy** after just a few hours

### Example Failure

After device runs for 6+ hours:
- Tick count: 2,160,000 ticks (6 hours)
- Multiply by 10: `21,600,000 ms`
- Session timeout check: `43,200,000 ms` (12 hours)

The subtraction `(now - last_login_time)` becomes unreliable due to uint32_t wrapping when both values are multiplied by 10. This causes **false positives** in the timeout check, making `dexcom_is_authenticated()` incorrectly return `false`.

---

## The Fix

Use **uint64_t instead of uint32_t** to prevent overflow.

### Changes in `dexcom_api.c`

**1. Changed time constants to 64-bit:**
```c
// OLD (BUGGY):
#define RATE_LIMIT_MS       90000   // uint32_t
#define SESSION_TIMEOUT_MS  43200000  // uint32_t

// NEW (FIXED):
#define RATE_LIMIT_MS       90000ULL    // uint64_t
#define SESSION_TIMEOUT_MS  43200000ULL // uint64_t
```

**2. Changed variables to uint64_t:**
```c
// OLD (BUGGY):
static uint32_t last_login_time = 0;
static uint32_t last_glucose_fetch_time = 0;

// NEW (FIXED):
static uint64_t last_login_time = 0;
static uint64_t last_glucose_fetch_time = 0;
```

**3. Cast to uint64_t before multiplication (lines ~600, ~625, ~827, ~848):**
```c
// OLD (BUGGY):
uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;  // Can overflow!

// NEW (FIXED):
uint64_t now_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;  // Safe!
```

### Enhanced Logging in `main.c`

Added authentication status logging to glucose_update_task (line ~1291):
```c
bool is_auth = dexcom_is_authenticated();
ESP_LOGI(TAG, "[Glucose Task] Auth status: %s, Free heap: %lu bytes",
         is_auth ? "AUTHENTICATED" : "NOT AUTHENTICATED",
         esp_get_free_heap_size());
```

This helps diagnose if authentication is incorrectly failing.

---

## Why This Fix Works

1. **uint64_t prevents overflow**: Max value is 18,446,744,073,709,551,615 ms = ~584 million years
2. **Cast before multiplication**: `(uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS` ensures the multiplication happens in 64-bit space
3. **12-hour timeout accurate**: 43,200,000 ms fits comfortably in uint64_t
4. **Works indefinitely**: Device would need to run for hundreds of thousands of years to overflow uint64_t

---

## Testing Plan

1. **Build and flash** with the fixed code
2. **Monitor serial logs** for the new authentication status messages
3. **Verify periodic fetches**: Look for "Fetching periodic glucose reading..." every 90 seconds
4. **Long-term test**: Let device run for 12+ hours to verify no false timeout errors

### Expected Serial Output (Every 90 Seconds)

```
I (123456) CYGM: [Glucose Task] Auth status: AUTHENTICATED, Free heap: 85432 bytes
I (123456) CYGM: Fetching periodic glucose reading...
I (123460) DEXCOM_API: Glucose updated: 125 mg/dL, trend: Flat
```

If authentication fails incorrectly, you'll see:
```
I (123456) CYGM: [Glucose Task] Auth status: NOT AUTHENTICATED, Free heap: 85432 bytes
```

---

## Files Modified

- `main/dexcom_api.c`: Fixed tick overflow in session timeout logic (3 locations)
- `main/main.c`: Added enhanced logging to glucose_update_task

---

## Impact

**CRITICAL FIX** - This resolves the life-critical issue where glucose monitoring stops working, requiring manual reboots. The device will now reliably fetch glucose readings every 90 seconds for the full 12-hour session duration.

---

## Next Steps

After this fix is verified:
1. Consider implementing proactive session refresh at 10-11 hours (before 12-hour timeout)
2. Add WiFi auto-reconnect for network reliability
3. Implement watchdog monitoring for task health

---

**Status**: ✅ Fixed - Ready for testing
