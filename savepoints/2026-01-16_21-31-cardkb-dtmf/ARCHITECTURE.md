# CYGM Architecture

## Overview
Continuous Glucose Monitor display supporting multiple CGM data sources.
Users select their CGM type; firmware handles API differences.

## System Architecture

```
┌────────────────────────────────────────────────────────────┐
│                      CYGM Device                           │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │   Display    │  │   Speaker    │  │   RGB LED    │     │
│  │   ST7789     │  │   Alarms     │  │   Status     │     │
│  │   320x240    │  │   DTMF beep  │  │   Alerts     │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
│                                                            │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                 UI Manager                           │  │
│  │  - Glucose display (large value + trend arrow)      │  │
│  │  - Mini graph (recent readings)                     │  │
│  │  - Settings menu                                    │  │
│  │  - Alarm notifications                              │  │
│  └─────────────────────────────────────────────────────┘  │
│                           │                                │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              CGM Data Manager                        │  │
│  │  - Polls active CGM source                          │  │
│  │  - Normalizes data to common format                 │  │
│  │  - Tracks stale data (>5 min old)                   │  │
│  │  - Triggers alarms on high/low                      │  │
│  └─────────────────────────────────────────────────────┘  │
│                           │                                │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              CGM Source Drivers                      │  │
│  ├─────────────────────────────────────────────────────┤  │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐   │  │
│  │  │ Nightscout  │ │Dexcom Share │ │ LibreLinkUp │   │  │
│  │  │   Driver    │ │   Driver    │ │   Driver    │   │  │
│  │  └─────────────┘ └─────────────┘ └─────────────┘   │  │
│  │         (future: Medtronic, Eversense, etc.)        │  │
│  └─────────────────────────────────────────────────────┘  │
│                           │                                │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              WiFi Manager                            │  │
│  │  - Connect to configured network                    │  │
│  │  - Credential entry via CardKB                      │  │
│  │  - Connection status display                        │  │
│  └─────────────────────────────────────────────────────┘  │
│                           │                                │
│  ┌─────────────────────────────────────────────────────┐  │
│  │              NVS Config Storage                      │  │
│  │  - WiFi credentials                                 │  │
│  │  - CGM source + credentials                         │  │
│  │  - User preferences                                 │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                            │
│  ┌──────────────┐  ┌──────────────┐                       │
│  │   CardKB     │  │ BOOT Button  │                       │
│  │   Keyboard   │  │ Quick action │                       │
│  └──────────────┘  └──────────────┘                       │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

## Data Structures

### Common Glucose Reading
```c
typedef enum {
    TREND_DOUBLE_UP,      // ↑↑ Rising fast (>3 mg/dL/min)
    TREND_UP,             // ↑  Rising
    TREND_UP_SLIGHT,      // ↗  Rising slightly
    TREND_FLAT,           // →  Stable
    TREND_DOWN_SLIGHT,    // ↘  Falling slightly
    TREND_DOWN,           // ↓  Falling
    TREND_DOUBLE_DOWN,    // ↓↓ Falling fast
    TREND_UNKNOWN         // ?  No trend data
} glucose_trend_t;

typedef struct {
    uint16_t value;           // mg/dL (or mmol/L * 18)
    glucose_trend_t trend;    // Direction arrow
    int64_t timestamp;        // Unix timestamp (ms)
    bool is_stale;            // >5 minutes old
    bool is_valid;            // Data successfully fetched
} glucose_reading_t;
```

### CGM Source Configuration
```c
typedef enum {
    CGM_SOURCE_NONE,
    CGM_SOURCE_NIGHTSCOUT,
    CGM_SOURCE_DEXCOM_SHARE,
    CGM_SOURCE_LIBRELINKUP,
    // Future sources added here
} cgm_source_t;

typedef struct {
    cgm_source_t source;
    char url[128];            // Nightscout URL or API endpoint
    char username[64];        // For Dexcom/Libre login
    char password[64];        // For Dexcom/Libre login
    char token[64];           // API token (Nightscout)
} cgm_config_t;
```

### User Preferences
```c
typedef struct {
    bool use_mmol;            // false=mg/dL, true=mmol/L
    uint16_t high_threshold;  // High alert (default: 180 mg/dL)
    uint16_t low_threshold;   // Low alert (default: 70 mg/dL)
    uint16_t urgent_high;     // Urgent high (default: 250 mg/dL)
    uint16_t urgent_low;      // Urgent low (default: 55 mg/dL)
    bool sound_enabled;       // Alarm sounds on/off
    uint8_t brightness;       // Display brightness 0-100
} user_prefs_t;
```

## CGM API Details

### Nightscout
- **Base URL**: User-provided (e.g., `https://mysite.herokuapp.com`)
- **Auth**: API token in header `api-secret: SHA1(token)`
- **Endpoints**:
  - Current: `GET /api/v1/entries/current.json`
  - Recent: `GET /api/v1/entries.json?count=12`
- **Response**: `{"sgv": 142, "direction": "Flat", "date": 1705432800000}`

### Dexcom Share
- **US URL**: `https://share2.dexcom.com`
- **International**: `https://shareous1.dexcom.com`
- **Auth Flow**:
  1. POST `/ShareWebServices/Services/General/LoginPublisherAccountByName`
  2. Returns session ID
  3. Use session for data requests
- **Get Data**: `POST /ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues`
- **Response**: `[{"Value": 142, "Trend": "Flat", "WT": "/Date(1705432800000)/"}]`

### LibreLinkUp
- **URL**: `https://api.libreview.io`
- **Auth**: Login returns auth token
- **Endpoints**: `/llu/connections`, `/llu/connections/{id}/graph`
- **Note**: Requires accepted terms of service

## Implementation Phases

### Phase 1: Core Infrastructure
- [ ] WiFi manager with CardKB input
- [ ] NVS storage for config
- [ ] Settings menu UI
- [ ] Basic glucose display UI

### Phase 2: First CGM Source (Dexcom Share)
- [ ] Dexcom Share API driver
- [ ] HTTPS client setup
- [ ] Session management
- [ ] Data parsing

### Phase 3: Nightscout Support
- [ ] Nightscout API driver
- [ ] Token authentication
- [ ] URL configuration

### Phase 4: Display & Alerts
- [ ] Large glucose value display
- [ ] Trend arrows
- [ ] Color coding (green/yellow/red)
- [ ] Mini graph
- [ ] Audio alarms

### Phase 5: Polish
- [ ] LibreLinkUp driver
- [ ] OTA firmware updates
- [ ] Battery optimization (if applicable)
- [ ] Error handling & recovery

## File Structure
```
CYGM/
├── main/
│   ├── main.c              # App entry point
│   ├── wifi_manager.c/h    # WiFi connection
│   ├── nvs_config.c/h      # Settings storage
│   ├── cgm_manager.c/h     # Data manager
│   ├── cgm_nightscout.c/h  # Nightscout driver
│   ├── cgm_dexcom.c/h      # Dexcom Share driver
│   ├── cgm_libre.c/h       # LibreLinkUp driver
│   ├── ui_manager.c/h      # Display UI
│   ├── alarm_manager.c/h   # Sound/LED alerts
│   └── cardkb.c/h          # Keyboard input
├── components/
│   └── (external libs if needed)
├── CMakeLists.txt
└── README.md
```

## OTA Updates
- Check version on boot: `GET https://your-server.com/cygm/version.json`
- If newer, prompt user to update
- Download firmware binary
- Flash via ESP-IDF OTA partition

---
*Architecture v1.0 - 2025-01-16*
