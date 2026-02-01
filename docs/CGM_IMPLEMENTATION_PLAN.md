# CGM Implementation Plan

**Created**: 2026-01-19
**Updated**: 2026-01-22
**Status**: Dexcom Share API Implementation Complete

---

## Overview

Implement CGM (Continuous Glucose Monitor) data fetching and display using the **Dexcom Share API** for direct username/password authentication.

---

## Architecture Decision: Share API vs OAuth2

### Why Dexcom Share API?
- **No OAuth flow required**: Direct username/password authentication
- **No app registration**: Works with existing Dexcom credentials
- **Community proven**: Used by pydexcom, Nightscout, Garmin watch faces
- **Device-friendly**: No web browser redirect needed

### Limitations
- Unofficial API (could change, but has been stable for years)
- Requires Dexcom Share to be enabled in user's Dexcom app
- Session IDs expire (handled with auto-reauthentication)

---

## Implementation Status

### Phase 1: Dexcom Share API (COMPLETE)

**Files Created**:
- `main/dexcom_api.h` - API header with types and declarations
- `main/dexcom_api.c` - Full implementation (~400 lines)

**Core Functions**:
```c
esp_err_t dexcom_api_init(void);                              // Init, load saved creds
esp_err_t dexcom_authenticate(const char *user, const char *pass); // Full auth flow
esp_err_t dexcom_fetch_glucose(dexcom_glucose_t *glucose);    // Get latest reading
bool dexcom_is_authenticated(void);                           // Check session
void dexcom_logout(void);                                     // Clear session + creds
const char* dexcom_trend_arrow(dexcom_trend_t trend);         // Get arrow string
const char* dexcom_trend_description(dexcom_trend_t trend);   // Get description
```

**Data Types**:
```c
typedef enum {
    DEXCOM_REGION_US = 0,
    DEXCOM_REGION_OUS,
    DEXCOM_REGION_JP
} dexcom_region_t;

typedef enum {
    TREND_NONE = 0,
    TREND_DOUBLE_UP,      // Rising quickly
    TREND_SINGLE_UP,      // Rising
    TREND_FORTY_FIVE_UP,  // Rising slowly
    TREND_FLAT,           // Steady
    TREND_FORTY_FIVE_DOWN,// Falling slowly
    TREND_SINGLE_DOWN,    // Falling
    TREND_DOUBLE_DOWN,    // Falling quickly
    TREND_NOT_COMPUTABLE,
    TREND_RATE_OUT_OF_RANGE
} dexcom_trend_t;

typedef struct {
    int value;              // mg/dL
    dexcom_trend_t trend;
    time_t timestamp;
    bool valid;
} dexcom_glucose_t;
```

### Phase 2: NVS Credential Storage (COMPLETE)

**Functions Added to nvs_config**:
```c
esp_err_t nvs_set_dexcom_credentials(const char *username, const char *password);
esp_err_t nvs_get_dexcom_credentials(char *username, size_t user_len, char *password, size_t pass_len);
bool nvs_has_dexcom_credentials(void);
esp_err_t nvs_clear_dexcom_credentials(void);
```

### Phase 3: UI Integration (IN PROGRESS)

**Completed**:
- Updated Dexcom auth screen with "Login with Username" button
- Added disconnect button when connected
- Added screen variables and forward declarations

**TODO**:
1. Create full login screen with username/password text areas
2. Integrate CardKB keyboard for text entry
3. Add login button callback
4. Add status display for connection state

### Phase 4: Home Screen Integration (TODO)

1. Display glucose value on right card (48pt font)
2. Display trend arrow (color-coded)
3. Display time since last reading
4. Create periodic update task (every 5 minutes)

---

## API Reference

### Regional Endpoints
| Region | Base URL |
|--------|----------|
| US | https://share2.dexcom.com/ShareWebServices/Services |
| Outside US | https://shareous1.dexcom.com/ShareWebServices/Services |
| Japan | https://share.dexcom.jp/ShareWebServices/Services |

### Application IDs
| Region | App ID |
|--------|--------|
| US/OUS | d89443d2-327c-4a6f-89e5-496bbb0317db |
| Japan | d8665ade-9673-4e27-9ff6-92db4ce13d13 |

### Authentication Flow

**Step 1: Get Account ID**
```
POST {base}/General/AuthenticatePublisherAccount
Headers: Content-Type: application/json
Body: {
  "accountName": "username_or_email",
  "password": "password",
  "applicationId": "app-id"
}
Response: "account-uuid-in-quotes"
```

**Step 2: Get Session ID**
```
POST {base}/General/LoginPublisherAccountById
Headers: Content-Type: application/json
Body: {
  "accountId": "account-uuid",
  "password": "password",
  "applicationId": "app-id"
}
Response: "session-uuid-in-quotes"
```

**Step 3: Fetch Glucose Data**
```
POST {base}/Publisher/ReadPublisherLatestGlucoseValues?sessionId={id}&minutes=1440&maxCount=1
Headers: Content-Type: application/json
Body: (empty, Content-Length: 0)
Response: [
  {
    "WT": "Date(1731645818222)",
    "ST": "Date(1731645818222)",
    "DT": "Date(1731645818222-0500)",
    "Value": 155,
    "Trend": "Flat"
  }
]
```

### Trend Values
| API Value | Enum | Arrow | Description |
|-----------|------|-------|-------------|
| None | TREND_NONE | ? | Unknown |
| DoubleUp | TREND_DOUBLE_UP | ^^ | Rising quickly |
| SingleUp | TREND_SINGLE_UP | ^ | Rising |
| FortyFiveUp | TREND_FORTY_FIVE_UP | / | Rising slowly |
| Flat | TREND_FLAT | -> | Steady |
| FortyFiveDown | TREND_FORTY_FIVE_DOWN | \ | Falling slowly |
| SingleDown | TREND_SINGLE_DOWN | v | Falling |
| DoubleDown | TREND_DOUBLE_DOWN | vv | Falling quickly |
| NotComputable | TREND_NOT_COMPUTABLE | ? | Cannot compute |
| RateOutOfRange | TREND_RATE_OUT_OF_RANGE | ! | Rate too high |

---

## Security Notes

- Credentials stored in NVS (not encrypted)
- Session IDs stored in RAM only
- HTTPS with certificate verification for all API calls
- Auto-clear credentials on logout

---

## Future Enhancements

1. **LibreLinkUp Support**: Similar direct login approach
2. **Nightscout Support**: REST API with custom URL
3. **Glucose Graph**: LVGL line chart for trends
4. **Alerts**: Audio/visual for high/low readings
