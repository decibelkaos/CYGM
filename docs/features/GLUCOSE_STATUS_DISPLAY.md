# Glucose Status Display - Enhanced User Feedback

## Summary

✅ **Implemented intelligent status messages** for different glucose data states
🎯 **Better user experience** - Clear feedback during sensor warmup, signal loss, etc.
📊 **Uses Dexcom Share API data** - Infers status from response patterns

---

## Problem: Generic "No data" Message

Previously, all failure scenarios showed the same message:
```
---
No data
```

Users couldn't tell if:
- Sensor is warming up (2 hours)
- Signal loss (out of range)
- Not logged in
- Sensor not connected

---

## Solution: Status-Specific Messages

### Dexcom API Response Patterns

The Dexcom Share API returns limited information:

**Success (with data):**
```json
[
  {
    "WT": "Date(1691455258000)",      // Wall Time (UTC)
    "ST": "Date(1691455258000)",      // System Time
    "DT": "Date(1691455258000-0400)", // Display Time
    "Value": 179,                      // Glucose in mg/dL
    "Trend": "Rising"                  // Text trend
  }
]
```

**Failure scenarios:**
```json
[]  // Empty array - sensor warmup, signal loss, or no sensor
```

**That's it!** No status codes, error fields, or sensor state.

### Intelligent Status Inference

We infer status from:

1. **Empty array + recent login (< 2 hours)** → Sensor warmup
2. **Empty array + old login (> 2 hours)** → No sensor data / signal loss
3. **Data with old timestamp (> 15 min)** → Signal loss
4. **Not authenticated** → Not logged in

---

## Implementation

### New Status Enum (dexcom_api.h)

```c
typedef enum {
    GLUCOSE_STATUS_OK = 0,           // Valid data
    GLUCOSE_STATUS_NO_DATA,          // No readings (empty array)
    GLUCOSE_STATUS_WARMUP,           // Likely sensor warmup (no data + recent auth)
    GLUCOSE_STATUS_SIGNAL_LOSS,      // Likely signal loss (data too old)
    GLUCOSE_STATUS_NOT_AUTHENTICATED // Not logged in
} dexcom_status_t;
```

### Updated Glucose Data Structure

```c
typedef struct {
    int value;              // mg/dL
    dexcom_trend_t trend;   // Trend direction
    time_t timestamp;       // Unix timestamp
    bool valid;             // Data validity flag
    dexcom_status_t status; // Status code (NEW!)
} dexcom_glucose_t;
```

---

## Status Detection Logic

### 1. Sensor Warmup Detection (dexcom_api.c)

```c
if (cJSON_GetArraySize(root) == 0) {
    uint32_t time_since_login = now - last_login_time;

    if (time_since_login < 7200000) {  // Less than 2 hours
        ESP_LOGI(TAG, "Likely sensor warmup period");
        glucose->status = GLUCOSE_STATUS_WARMUP;
    } else {
        glucose->status = GLUCOSE_STATUS_NO_DATA;
    }
    return ESP_FAIL;
}
```

### 2. Signal Loss Detection (dexcom_api.c)

```c
// Check if data is stale (more than 15 minutes old)
time_t now;
time(&now);
int minutes_old = (int)difftime(now, glucose->timestamp) / 60;

if (minutes_old > 15) {
    ESP_LOGW(TAG, "Glucose data is %d minutes old - likely signal loss", minutes_old);
    glucose->status = GLUCOSE_STATUS_SIGNAL_LOSS;
    glucose->valid = false;  // Mark as invalid
} else {
    glucose->status = GLUCOSE_STATUS_OK;
    glucose->valid = true;
}
```

### 3. Display Logic (main.c)

```c
switch (glucose_status) {
    case GLUCOSE_STATUS_WARMUP:
        lv_label_set_text(label_time_ago, "Sensor warmup");
        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_ACCENT), 0);  // Orange
        break;

    case GLUCOSE_STATUS_SIGNAL_LOSS:
        lv_label_set_text(label_time_ago, "Signal loss");
        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_RED), 0);  // Red
        break;

    case GLUCOSE_STATUS_NOT_AUTHENTICATED:
        lv_label_set_text(label_time_ago, "Not logged in");
        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_TEXT_DIM), 0);  // Gray
        break;

    case GLUCOSE_STATUS_NO_DATA:
    default:
        lv_label_set_text(label_time_ago, "No sensor data");
        lv_obj_set_style_text_color(label_glucose, lv_color_hex(COLOR_TEXT_DIM), 0);
        break;
}
```

---

## Status Messages and Colors

| Status | Display Text | Glucose Color | Likely Cause |
|--------|-------------|---------------|--------------|
| **OK** | "X mins ago" | Blue/Green/Red (alarm) | Valid data |
| **Warmup** | "Sensor warmup" | Orange | New sensor within 2 hours |
| **Signal Loss** | "Signal loss" | Red | Transmitter out of range or data >15min old |
| **No Data** | "No sensor data" | Gray | Sensor not connected or failed |
| **Not Authenticated** | "Not logged in" | Gray | No Dexcom credentials |

---

## User Experience Examples

### Scenario 1: New Sensor (2-hour warmup)

**Timeline:**
```
00:00 - Insert sensor, start session
00:01 - Device shows: "---" / "Sensor warmup" (Orange)
01:59 - Still shows: "---" / "Sensor warmup" (Orange)
02:00 - First reading: "179 mg/dL" / "Just now" (Blue)
```

### Scenario 2: Transmitter Out of Range

**Timeline:**
```
10:00 - Last reading: "165 mg/dL" / "Just now"
10:05 - Walk away from transmitter
10:16 - Device shows: "165 mg/dL" / "Signal loss" (Red) [data >15min old]
10:20 - Return to range
10:21 - New reading: "162 mg/dL" / "Just now" (Blue)
```

### Scenario 3: Sensor Fails

**Timeline:**
```
14:00 - Sensor fails (empty array from Dexcom)
14:01 - Device shows: "---" / "No sensor data" (Gray)
14:30 - Replace sensor
14:31 - Device shows: "---" / "Sensor warmup" (Orange)
16:31 - First reading: "142 mg/dL" / "Just now" (Blue)
```

### Scenario 4: Not Logged In

**Timeline:**
```
Boot - Device shows: "---" / "Not logged in" (Gray)
User enters credentials
Login success
Device shows: "---" / "Sensor warmup" (Orange) [if within 2hr]
or: "179 mg/dL" / "Just now" (Blue) [if sensor already active]
```

---

## Files Modified

### 1. `main/dexcom_api.h`
- Added `dexcom_status_t` enum (5 status codes)
- Added `status` field to `dexcom_glucose_t` struct

### 2. `main/dexcom_api.c`
- Line ~609: Initialize status to `NOT_AUTHENTICATED`
- Line ~732: Detect warmup vs no-data (empty array)
- Line ~760: Detect signal loss (stale timestamp)
- Line ~779: Log status in glucose fetch

### 3. `main/main.c`
- Line ~163: Added `glucose_status` global variable
- Line ~1650: Enhanced `update_glucose_display()` with status-specific messages
- Line ~1813: Store status on successful fetch
- Line ~1818: Store status on failed fetch
- Line ~4020: Reset status on logout
- Line ~4387: Store status on manual login
- Line ~4400: Store status on login failure
- Line ~5654: Store status on auto-login

---

## Testing Checklist

### Normal Operation
- [ ] Valid glucose data shows value + "X mins ago"
- [ ] Data updates every 90 seconds
- [ ] Trend arrow displays correctly

### Sensor Warmup
- [ ] Insert new sensor, start session
- [ ] Device shows "---" / "Sensor warmup" (Orange)
- [ ] After 2 hours, first reading appears
- [ ] Status changes to normal operation

### Signal Loss
- [ ] Move transmitter out of range
- [ ] After 15 minutes, shows "Signal loss" (Red)
- [ ] Return to range, status clears
- [ ] New reading appears

### No Sensor
- [ ] Remove sensor or let it expire
- [ ] Device shows "---" / "No sensor data" (Gray)
- [ ] Insert new sensor, status changes to warmup

### Not Authenticated
- [ ] Log out from Dexcom
- [ ] Device shows "---" / "Not logged in" (Gray)
- [ ] Log back in, status updates

---

## Limitations

### 1. Cannot Distinguish All Scenarios

Since Dexcom only returns empty array `[]`, we cannot definitively tell:
- Sensor warmup vs signal loss vs no sensor
- Transmitter battery low
- Sensor malfunction

We use **heuristics** (time since login, data age) to make educated guesses.

### 2. Warmup Detection Assumes New Sensor

If user logs out/in during active sensor session, warmup detection may incorrectly trigger.

**Mitigation:** Only show warmup if empty array AND < 2 hours since login.

### 3. Signal Loss Detection Threshold

15-minute threshold is aggressive:
- Dexcom transmits every 5 minutes
- 3 missed transmissions = signal loss

**Rationale:** Better to warn early than wait too long.

---

## Future Enhancements

### 1. Persistent Last-Known Status
Store status in NVS to maintain state across reboots:
```c
nvs_save_glucose_status(glucose_status);
```

### 2. Historical Status Log
Track when status changes occurred:
```
12:00 - OK
12:16 - Signal Loss
12:21 - OK
14:35 - Sensor Warmup
16:35 - OK
```

### 3. User Notifications
Show toast/popup when status changes:
- "Sensor warmup started (2 hours remaining)"
- "Signal lost - move closer to transmitter"
- "Sensor ready - first reading received"

### 4. Status Icon
Add small icon next to glucose value:
- ⏳ Warmup
- 📡 Signal loss
- ❌ No sensor
- 🔓 Not logged in

---

## Dexcom API Documentation

### Official Documentation
- [Dexcom Developer Portal](https://developer.dexcom.com/)
- [Authentication](https://developer.dexcom.com/docs/dexcom/authentication/)
- [Scopes & Access](https://developer.dexcom.com/docs/dexcom/scopes-access/)

### Unofficial Share API (What We Use)
- [pydexcom GitHub](https://github.com/gagebenne/pydexcom) - Python library for Dexcom Share
- Uses username/password (no OAuth secrets needed)
- Perfect for open-source projects

### Why Dexcom Share Is Best for Open-Source
1. ✅ **No API keys** - Users provide their own credentials
2. ✅ **No OAuth secrets** - Username/password only
3. ✅ **No HIPAA compliance** - Data already belongs to user
4. ✅ **Simple authentication** - 2-step login (account ID → session ID)
5. ✅ **Persistent sessions** - 12-hour timeout, minimal re-auth

---

## Alternative: Nightscout Integration

**Nightscout** is an open-source CGM monitoring platform:
- User self-hosts Nightscout server (Heroku, Azure, etc.)
- Nightscout bridges Dexcom Share → their server
- Device reads from Nightscout URL (can be public, no auth)

**Advantages:**
- Multiple data sources (Dexcom, Libre, Medtronic, etc.)
- Historical data and charts
- Community-driven features

**Disadvantages:**
- Requires self-hosting
- Still needs Dexcom credentials to bridge
- More complex setup

**Implementation Consideration:** Could add Nightscout as an alternative data source option.

---

## Summary

**Before:**
```
---
No data
```
*User thinks: "Is it warming up? Did I lose signal? Is it broken?"*

**After:**
```
---
Sensor warmup (Orange)
```
or
```
---
Signal loss (Red)
```
or
```
---
No sensor data (Gray)
```
*User knows exactly what's happening!*

---

**Sources:**
- [Dexcom API Documentation](https://developer.dexcom.com/)
- [pydexcom GitHub](https://github.com/gagebenne/pydexcom)
- [Nightscout Project](https://github.com/nightscout/cgm-remote-monitor)
- [Dexcom Support - Sensor Warmup](https://www.dexcom.com/en-us/faqs/what-to-do-if-dexcom-g6-sensor-fails-during-warmup)
- [Dexcom Support - Signal Loss](https://www.dexcom.com/en-us/faqs/how-should-i-respond-to-the-signal-loss-alert)

*Implemented: 2026-01-28 Evening*
*Status: Ready for testing*
