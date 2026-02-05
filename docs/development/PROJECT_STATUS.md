# CYGM Project Status
## Continuous Glucose Monitor Display for ESP32
### Last Updated: February 4, 2026

---

## 🎉 BETA STAGE - Ready for Field Testing

**Current Version**: 1.3.3

All core features are implemented and functional. Device is ready for real-world testing and feedback.

### Working Features

#### 1. Hardware Integration ✅
- **Display**: ST7789 320x240 TFT (55MHz SPI, RGB565)
  - **Brightness Control** (v1.3.3): PWM-based backlight (0-100%), saved to NVS
  - Lightbulb icon on CGM card opens brightness overlay
  - Real-time adjustment with slider (0-100%)
  - Automatically sets to 100% during visual alarms
- **Touch**: CST820 capacitive touch with polling mode
- **LED**: RGB LED (active-low PWM control) with visual feedback system
- **Speaker**: DTMF tone generation for alarms
- **Keyboard**: M5Stack CardKB (I2C) for text entry

#### 1a. LED Visual Feedback System ✅ (v1.3.2)
- **WiFi Boot Blink**: Green LED blinks every 200ms during WiFi connection, stops when home screen loads
- **Dexcom Poll Indicator**: Blue LED double-blink sequence before glucose fetch
  - 5 steps with decreasing brightness (100%, 85%, 70%, 60%, 50%)
  - Each step has 2 blinks (50ms on, 50ms off, 50ms on, 150ms gap)
  - Total duration: 1.5 seconds
  - Signals upcoming Dexcom API poll
- **Success Indicator**: Steady green LED for 3 seconds after successful glucose fetch, then 500ms fade-out
- **Error Indicator**: Red LED blinks 500ms on/off for WiFi or Dexcom errors (5 seconds duration)
- **Alarm Flash**: Bright red fast flash (100ms on/off) during active alarms
- **Architecture**: Dedicated FreeRTOS task on Core 0, multiple animation modes with state machine

#### 2. Network & Time ✅
- **WiFi**: WPA2-PSK connection with NVS credential storage
- **SNTP**: Time synchronization with 3 servers (pool.ntp.org, time.google.com, time.nist.gov)
- **Timezone**: America/Los_Angeles with DST support
- **SSL/TLS**: mbedTLS with dynamic buffers (see SSL_MEMORY_FIX.md)

#### 3. Weather Integration ✅
- **Location Lookup** (v1.3.3): Dual-mode system with offline database
  - **City Lookup**: Hierarchical Country → State → City dropdowns
    - USA first, Canada second, then alphabetical
    - Real-time result display with coordinates
  - **Code Lookup**: Real-time postal code search (types as you go)
    - Searches 29,845-entry local database instantly
    - No SSL/network required - 100% offline
  - Both modes: Cancel/OK buttons, timezone auto-detection
- **Local Postal Database**: 2.28MB database with 47 countries
  - US ZIP codes grouped by city (saves space)
  - Covers all USA, Canada, major EU/UK cities
  - Creative Commons Attribution 4.0 (GeoNames)
- **Weather Data**: Current temperature and 7-day forecast (open-meteo.com)
- **Weather Icons**: 7 custom-drawn weather conditions with LVGL canvas

#### 4. Dexcom Integration ✅
- **Authentication**: Two-step login (AccountID → SessionID)
- **Multi-Region**: Supports US, Outside US, and Japan regions
- **Glucose Fetching**: Latest reading with trend arrow
- **Credential Storage**: NVS persistence with encryption
- **Auto-Refresh**: Every 5 minutes when authenticated
- **Trend Arrows**: 9 directional indicators (DoubleUp, SingleUp, FortyFiveUp, Flat, FortyFiveDown, SingleDown, DoubleDown, NotComputable, RateOutOfRange)
- **Trend Arrow Display** (v1.3.0 improvements):
  - Canvas-based implementation (60x60 pixels)
  - Color glow system:
    - Green: Stable trends (FLAT, FORTY_FIVE_UP/DOWN)
    - Orange: Moderate change (SINGLE_UP/DOWN)
    - Red: Rapid movement (DOUBLE_UP/DOWN)
  - Extended shafts (38 pixels for vertical, 38 for horizontal)
  - Rotated patterns from SINGLE_UP template for visual consistency

#### 5. Alarm System ✅
- **4 Configurable Alarms**:
  - High Alarm (urgent): Default 250 mg/dL, red text
  - High Warning: Default 180 mg/dL, amber text
  - Low Warning: Default 80 mg/dL, amber text
  - Low Alarm (urgent): Default 55 mg/dL, red text

- **Visual Alarm System**:
  - Pulsing white screen overlay at 2 Hz (250ms on/off)
  - Full-screen slider dismissal (must slide to 100%)
  - LVGL timer-based pulsing for consistent timing
  - Always appears above all UI elements

- **Audio Alarm System**:
  - 5 alarm tones (Beep 1/2/3, Chime, Alert)
  - PWM-based buzzer on GPIO 26
  - Configurable volume (0-100%)
  - Repeating alarms (every 3 seconds) for urgent states
  - One-shot alarms for warnings
  - Tone preview in settings

- **Snooze Functionality**:
  - Snooze/dismiss screen after slider activation
  - Snooze options: 5, 10, 15, 30 minutes
  - Dismiss button for permanent alarm dismissal
  - Unix timestamp-based snooze timer
  - Automatic re-triggering when snooze expires

- **Alarm Configuration**:
  - Per-alarm threshold slider (50-400 mg/dL)
  - Audio/visual alert toggles
  - Audio repeat toggle (urgent vs one-shot)
  - Tone selection with preview
  - Volume control
  - All settings persist to NVS

- **Alarm Logic**:
  - Continuous glucose monitoring
  - Priority-based triggering (urgent over warning)
  - State tracking to prevent spam
  - Glucose text color updates based on active alarm

- **Slider UX Improvements** (v1.3.0):
  - "Slide to disarm" text inside slider (white color)
  - Snap-back animation if released before 100%
  - Slider height: 40px for better visibility
  - Disarm button snoozes all alarms for 3 hours

#### 6. Battery Monitoring ✅
- **Voltage Monitoring**:
  - GPIO 39 (ADC1_CH3) for battery voltage reading
  - 1.46x voltage divider calibration (under load)
  - 10-sample averaging for precision
  - 3 decimal place voltage display (4.106V)
  - Update interval: 30 seconds

- **Battery Display**:
  - Battery icon on home screen (top left of CGM card)
  - Battery percentage (4px spacing to right of icon)
  - Color-coded icons:
    - Green: 90-100% (full battery)
    - White: 60-89% (3/4 battery)
    - Orange: 30-59% (1/2 battery), 10-29% (1/4 battery)
    - Red flashing: <10% (empty battery)

- **Percentage Mapping** (calibrated for under-load voltage):
  - 4.0V or higher = 100% (provides buffer for voltage sag)
  - 3.9V = 90%, 3.8V = 80%, 3.7V = 65%
  - 3.6V = 40%, 3.5V = 20%, 3.4V = 10%
  - 3.2V = 0% (safe shutdown threshold)

- **Low Battery Protection**:
  - 10% warning: 3 beeps at 800 Hz (250ms apart)
  - Red flashing icon at <10%
  - Automatic shutdown at 3.2V to prevent battery damage
  - Deep sleep until USB reconnected

#### 7. User Interface ✅
- **Screens**:
  - Home screen: Time, weather, glucose (if connected)
  - Menu screen: Settings navigation
  - CGM Settings: Provider selection (Dexcom, Nightscout, LibreLink)
  - **CGM Account Status**: Shared screen for all CGM types (Dexcom, Libre, Nightscout)
  - Dexcom Login: Username/password entry with M5Stack CardKB
  - WiFi Settings: Network scan, SSID/password entry
  - Time Settings: Timezone selection, DST toggle, 12/24hr format
  - Weather Settings: Zipcode entry, °F/°C toggle
  - **Alarm Settings**: 4 alarm cards with enable/disable toggles
  - **Alarm Detail Editor**: Threshold, audio/visual toggles, tone selection, volume, repeat
  - **Tone Picker**: 5 alarm tones with preview playback
  - **Snooze/Dismiss Screen**: Glucose reading, snooze options (5/10/15/30 min), dismiss button

- **Design Patterns**:
  - **CGM Account Status Screen**: All CGM implementations share the same layout
    - Only header title changes per CGM type
    - Consistent UX across Dexcom, Libre, Nightscout
    - See [CGM_SCREEN_DESIGN_PATTERN.md](CGM_SCREEN_DESIGN_PATTERN.md)

- **Text Entry**: Full QWERTY keyboard via M5Stack CardKB
  - Uppercase/lowercase (Shift key)
  - Symbols (Fn key)
  - Backspace, Enter, Cancel
  - Password masking

- **Touch Input**: Single-tap gesture recognition with 300ms debounce

#### 8. Configuration Storage ✅
- **NVS Partitions**:
  - WiFi credentials (SSID, password)
  - Time settings (timezone, DST, format)
  - Weather settings (zipcode, unit)
  - Dexcom credentials (username, password)
  - **Alarm settings** (4 alarms with thresholds, colors, tones, volumes, repeat settings)

- **Persistence**: All settings survive reboots

---

## 🔧 Implemented But Disabled

### Web Server & Developer Tools (Disabled to Save Memory)

**Status**: ✅ Fully implemented but set to `ENABLE_WEB_SERVER=0` in main.c

**Location**:
- Backend: `main/web_server.c`, `main/ui_config.c`
- Frontend: `web/` directory (index.html, settings.html, developer.html)
- Partition: SPIFFS (1MB at 0x210000)

**Why Disabled**:
- Web server consumes ~30KB heap
- SSL connections for Weather + Dexcom require available heap
- Current priority is core CGM functionality

**To Enable**: Change `#define ENABLE_WEB_SERVER 0` to `1` in main.c (line ~90)

**Features When Enabled**:
- HTTP server on port 80
- REST API for all device settings
- Live UI preview with screenshot capture
- Drag-and-drop UI element editor
- Custom trend arrow designer
- Custom weather icon designer

---

## 🏗️ Code Organization (v1.3.1)

**Major Refactoring Completed: 2026-01-31**

### Before & After
- **Original main.c**: 6,524 lines (monolithic)
- **Refactored main.c**: 1,420 lines (78% reduction!)
- **Modules Created**: 13 modules with clear separation of concerns

### Module Breakdown
```
Hardware (4 modules):
- led.c (61 lines) - RGB LED PWM control
- buzzer.c (177 lines) - Tone generation, boot/success/alarm sounds
- battery.c (282 lines) - ADC monitoring, voltage-to-%, shutdown protection
- display.c (223 lines) - LCD/SPI, touch (Core 1), CardKB, LVGL init

Features (2 modules):
- time_system.c (259 lines) - SNTP, timezone handling, time display
- weather_system.c (1,253 lines) - Geocoding, Open-Meteo API, weather display

UI (5 modules):
- menu_screen.c (91 lines) - Main settings menu
- home_screen.c (208 lines) - Time, weather, glucose display
- wifi_screens.c (603 lines) - WiFi scan, list, password entry
- cgm_screens.c (737 lines) - CGM provider configuration
- alarm_screens.c (1,039 lines) - Alarm settings, tone picker, snooze

Tasks (1 module):
- background_tasks.c (259 lines) - UI task (Core 1), WiFi task (Core 0)

Shared State (1 module):
- shared_state.h (282 lines) - Global variable declarations
```

### Benefits Achieved
- ✅ **100% feature parity** - Zero functionality changes
- ✅ **Clear separation of concerns** - Hardware, features, UI, tasks
- ✅ **Easy maintenance** - Each module 60-1,250 lines (avg ~400)
- ✅ **Modular architecture** - main.c is now just a coordinator
- ✅ **Improved navigation** - Easy to locate and fix bugs
- ✅ **Safe modifications** - Changes isolated to specific modules
- ✅ **Full git history** - 14 commits documenting each extraction

---

## 🐛 Critical Fixes Applied

### SSL Memory Allocation (Jan 27, 2026)

**Problem**: ESP32 ran out of heap memory when trying to establish multiple HTTPS connections.

**Solution**:
- Reduced mbedTLS buffer sizes from 20KB to 8KB per connection
- Enabled dynamic SSL buffer management
- Disabled peer certificate retention

**Files Modified**:
- `sdkconfig` (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN, CONFIG_MBEDTLS_DYNAMIC_BUFFER, etc.)

**Documentation**: See [SSL_MEMORY_FIX.md](SSL_MEMORY_FIX.md) for full details.

**Result**: Weather API and Dexcom API now work simultaneously ✅

### Alarm Snooze Screen Dismiss Fix (Jan 31, 2026)

**Problem**: After tapping snooze buttons, the screen would not dismiss and return to home. Device would crash with LoadProhibited exception.

**Root Cause**:
1. Screen dismissal code was inside `if (visual_alarm_active)` check
2. LVGL screens were being deleted before loading new screen (wrong order)

**Solution**:
- Moved screen dismissal outside the visual_alarm_active check
- Changed order: Load new screen FIRST, then delete old screens
- Added proper cleanup of visual_alarm_overlay and screen_alarm_snooze

**Files Modified**:
- `main/ui/alarm_screens.c` (alarm_snooze_event_cb function)

**Result**: Snooze buttons now properly dismiss screen and return to home ✅

---

## 📊 Memory Usage

### Flash (4MB Total)
```
Partition Table:
- nvs       (24KB)  @ 0x009000 - WiFi/config data
- phy_init  (4KB)   @ 0x00F000 - RF calibration
- factory   (2MB)   @ 0x010000 - App binary
- spiffs    (1MB)   @ 0x210000 - Web files (unused if web server disabled)
- Free      (~1MB)  - Available for future use

App Binary Size: 1.47 MB (539 KB free in 2MB partition)
```

### Heap (242 KB DRAM)
```
At Boot:
- DRAM: 242 KB available
- After WiFi init: ~180 KB
- After LVGL init: ~140 KB
- After Weather API: ~120 KB (during connection)
- After Dexcom API: ~100 KB (during connection)
- Idle state: ~130 KB

Critical Requirements:
- Each HTTPS connection needs 8KB buffer + ~20KB for handshake
- Minimum 60KB free heap for new SSL connection
```

### IRAM (22 KB)
```
Used for:
- WiFi driver
- LVGL rendering (some functions)
- Interrupt handlers
```

---

## 🏗️ Architecture

### Dual-Core Design
```
Core 0 (WiFi & Network):
- WiFi connection management
- SNTP time synchronization
- Weather API task (10 min intervals)
- Dexcom API task (5 min intervals when authenticated)

Core 1 (UI & Input):
- LVGL rendering task (10ms tick)
- Touch polling task (50ms)
- M5Stack CardKB input
- Display updates
```

**Why Dual-Core**: Prevents I2C conflicts between WiFi and CST820 touch controller.

### Task Stack Sizes
```
WiFi Task:     6KB  (Core 0)
LVGL Task:     8KB  (Core 1)
Touch Task:    4KB  (Core 1)
Weather Task:  8KB  (Core 0)
Dexcom Task:   8KB  (Core 0)
```

---

## 📝 Configuration Files

### sdkconfig (ESP-IDF Configuration)
**Critical Settings**:
```bash
# Partition
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# SSL/TLS (DO NOT CHANGE - see SSL_MEMORY_FIX.md)
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y

# FreeRTOS
CONFIG_FREERTOS_HZ=100
CONFIG_FREERTOS_UNICORE=n  (dual-core)

# LVGL
CONFIG_LV_MEM_SIZE=65536  (64KB for LVGL heap)
```

### partitions.csv
```csv
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,
spiffs,   data, spiffs,  0x210000, 0x100000,
```

---

## 🔮 Future Enhancements

### High Priority
- [ ] Nightscout API integration (alternative to Dexcom)
- [ ] LibreLinkUp API integration (Abbott Freestyle Libre)
- [ ] High/low glucose alarms with speaker
- [ ] Battery voltage monitoring (if portable power added)

### Medium Priority
- [ ] Re-enable web server when more memory optimization done
- [ ] OTA firmware updates
- [ ] Glucose trend graph (last 3 hours)
- [ ] Custom color themes

### Low Priority
- [ ] Bluetooth Low Energy for phone sync
- [ ] SD card logging
- [ ] Multiple Dexcom accounts (follow multiple people)

---

## 📚 Key Files

### Core Application
```
main/main.c              - Main application, UI screens, task management
main/nvs_config.c/h      - NVS storage for all settings
main/wifi_manager.c/h    - WiFi connection management
main/dexcom_api.c/h      - Dexcom Share API client
```

### Hardware Drivers
```
main/cst820_driver.c/h   - Touch controller (I2C)
main/tone_generator.c/h  - DTMF tones for speaker
```

### Web Server (Disabled)
```
main/web_server.c/h      - HTTP REST API server
main/ui_config.c/h       - UI layout storage & rendering
web/*                    - HTML/CSS/JS frontend
```

### Documentation
```
README.md                - Project overview
SSL_MEMORY_FIX.md        - Critical SSL configuration notes
HARDWARE_REFERENCE.md    - Complete hardware pinout and specs
PINOUT.md                - Quick reference
RULES.md                 - Development guidelines
```

---

## 🎯 Current Capabilities

**What the device does now:**
1. ✅ Connects to WiFi
2. ✅ Syncs time via SNTP
3. ✅ Shows local weather
4. ✅ Authenticates with Dexcom Share
5. ✅ Displays current glucose reading + trend
6. ✅ Auto-refreshes every 5 minutes
7. ✅ Saves all credentials to NVS
8. ✅ Full UI navigation with touch + keyboard
9. ✅ **4 configurable glucose alarms** (high/low with audio/visual alerts)
10. ✅ **Battery monitoring** (voltage, percentage, low battery protection)

**What it doesn't do yet:**
- ❌ Nightscout support
- ❌ LibreLinkUp support
- ❌ Historical glucose graph (last 3 hours)
- ❌ Web-based configuration

---

## 🚀 Next Steps

Based on the current state, here are recommended next priorities:

### 1. Add Nightscout Support
- Implement Nightscout REST API client
- Add server URL configuration
- Support authentication tokens

### 2. Add LibreLinkUp Support
- Implement LibreLinkUp REST API client
- Add authentication flow
- Support glucose fetching from Freestyle Libre sensors

### 3. Add Historical Graph
- Use LVGL chart widget
- Store last 3 hours in memory
- Show alongside current glucose

### 4. Memory Optimization for Web Server
- Profile exact memory usage
- Consider enabling only when not actively fetching CGM data
- Or wait for PSRAM hardware upgrade

---

## ⚠️ Important Notes

### DO NOT Modify sdkconfig SSL Settings
The SSL configuration is critical for multiple HTTPS connections. See [SSL_MEMORY_FIX.md](SSL_MEMORY_FIX.md) before changing any mbedTLS settings.

### Web Server Trade-off
The web server is fully implemented but disabled to prioritize core CGM functionality. It can be re-enabled once memory is optimized or if PSRAM is added.

### Dual-Core Architecture Required
Do not move WiFi tasks to Core 1 or touch tasks to Core 0 - this causes I2C conflicts.

---

**🎉 Project has reached BETA stage - Ready for field testing! (v1.3.0)**

✅ All core functionality operational and tested
✅ Battery monitoring with low-battery protection
✅ Complete alarm system (visual, audio, snooze, configurable thresholds)
✅ SSL memory management optimized
✅ Credentials persisting correctly
✅ Full touch + keyboard navigation
✅ Ready for additional CGM sources (Nightscout, LibreLink)
