# Dexcom Share API Reference

**Updated**: 2026-01-22
**API Type**: Dexcom Share (Unofficial Community API)

---

## Overview

This project uses the **Dexcom Share API**, an unofficial API discovered by reverse-engineering the Dexcom mobile apps. It's the same API used by:
- pydexcom (Python)
- Nightscout
- Garmin watch faces
- Many DIY diabetes projects

**Key Benefits**:
- Direct username/password authentication
- No OAuth2 flow required
- No app registration needed
- Works with existing Dexcom credentials

---

## Quick Start

```c
#include "dexcom_api.h"

// Initialize
dexcom_api_init();

// Set region (default is US)
dexcom_set_region(DEXCOM_REGION_US);

// Authenticate
esp_err_t ret = dexcom_authenticate("username", "password");
if (ret == ESP_OK) {
    // Fetch glucose
    dexcom_glucose_t glucose;
    if (dexcom_fetch_glucose(&glucose) == ESP_OK && glucose.valid) {
        printf("Glucose: %d mg/dL\n", glucose.value);
        printf("Trend: %s\n", dexcom_trend_arrow(glucose.trend));
    }
}

// Logout when done
dexcom_logout();
```

---

## API Functions

### dexcom_api_init()
Initialize the Dexcom API client. Loads saved credentials from NVS if available.

```c
esp_err_t dexcom_api_init(void);
```

**Returns**: ESP_OK on success

---

### dexcom_set_region()
Set the regional server to use.

```c
void dexcom_set_region(dexcom_region_t region);
```

**Parameters**:
- `region`: One of DEXCOM_REGION_US, DEXCOM_REGION_OUS, DEXCOM_REGION_JP

**Default**: DEXCOM_REGION_US

---

### dexcom_authenticate()
Authenticate with username and password. Performs full 2-step flow:
1. Get account ID from username/password
2. Get session ID from account ID/password

```c
esp_err_t dexcom_authenticate(const char *username, const char *password);
```

**Parameters**:
- `username`: Dexcom username, email, or phone number
- `password`: Dexcom password

**Returns**: ESP_OK on success, ESP_FAIL on authentication failure

**Note**: Credentials are stored in NVS on successful authentication.

---

### dexcom_fetch_glucose()
Fetch the latest glucose reading.

```c
esp_err_t dexcom_fetch_glucose(dexcom_glucose_t *glucose);
```

**Parameters**:
- `glucose`: Pointer to structure to fill with glucose data

**Returns**: ESP_OK on success

**Note**: Automatically re-authenticates if session has expired.

---

### dexcom_is_authenticated()
Check if we have a valid session.

```c
bool dexcom_is_authenticated(void);
```

**Returns**: true if authenticated and session is valid

---

### dexcom_logout()
Clear stored credentials and session.

```c
void dexcom_logout(void);
```

---

### dexcom_trend_arrow()
Get ASCII arrow representation of trend.

```c
const char* dexcom_trend_arrow(dexcom_trend_t trend);
```

**Returns**: Arrow string ("^^", "^", "/", "->", "\", "v", "vv", "?", "!")

---

### dexcom_trend_description()
Get human-readable trend description.

```c
const char* dexcom_trend_description(dexcom_trend_t trend);
```

**Returns**: Description string ("Rising quickly", "Steady", etc.)

---

## Data Types

### dexcom_region_t
```c
typedef enum {
    DEXCOM_REGION_US = 0,   // United States
    DEXCOM_REGION_OUS,      // Outside US (Europe, etc.)
    DEXCOM_REGION_JP        // Japan
} dexcom_region_t;
```

### dexcom_trend_t
```c
typedef enum {
    TREND_NONE = 0,
    TREND_DOUBLE_UP,        // Rising quickly (3+ mg/dL/min)
    TREND_SINGLE_UP,        // Rising (2 mg/dL/min)
    TREND_FORTY_FIVE_UP,    // Rising slowly (1 mg/dL/min)
    TREND_FLAT,             // Steady
    TREND_FORTY_FIVE_DOWN,  // Falling slowly
    TREND_SINGLE_DOWN,      // Falling
    TREND_DOUBLE_DOWN,      // Falling quickly
    TREND_NOT_COMPUTABLE,
    TREND_RATE_OUT_OF_RANGE
} dexcom_trend_t;
```

### dexcom_glucose_t
```c
typedef struct {
    int value;              // mg/dL
    dexcom_trend_t trend;   // Trend direction
    time_t timestamp;       // Unix timestamp
    bool valid;             // Data validity flag
} dexcom_glucose_t;
```

---

## Regional Servers

| Region | Base URL | App ID |
|--------|----------|--------|
| US | share2.dexcom.com | d89443d2-327c-4a6f-89e5-496bbb0317db |
| Outside US | shareous1.dexcom.com | d89443d2-327c-4a6f-89e5-496bbb0317db |
| Japan | share.dexcom.jp | d8665ade-9673-4e27-9ff6-92db4ce13d13 |

---

## Error Handling

The API returns ESP_OK on success or ESP_FAIL on failure. Common failure cases:
- Invalid credentials
- Network connection failed
- Session expired (auto-retry handled internally)
- Dexcom Share not enabled on user's account

---

## Requirements

For the Dexcom Share API to work:
1. User must have a Dexcom CGM (G5, G6, G7, or ONE)
2. User must have the Dexcom mobile app installed
3. **Dexcom Share must be enabled** in the mobile app
4. User must have at least one follower set up (even if unused)

---

## References

- pydexcom: https://github.com/gagebenne/pydexcom
- Ldalvik/DexcomAPI: https://github.com/Ldalvik/DexcomAPI (ESP32 Arduino)
- Calebh101/dexcom: https://github.com/Calebh101/dexcom (API documentation)
