# Changelog

All notable changes to the CYGM project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned Features
- Nightscout API integration
- LibreLinkUp API integration
- Historical glucose graph (last 3 hours)
- Web-based configuration interface (HTTP server, SPIFFS, REST API)
- Developer tools for UI customization
- Icon/trend arrow editor
- OTA firmware updates

## [1.3.0] - 2026-01-31

### Added - Battery Monitoring System
- **Battery Voltage Monitoring**
  - GPIO 39 (ADC1_CH3) for battery voltage reading
  - 10-sample averaging for precision (reduces noise)
  - 3 decimal place voltage display (e.g., 4.106V)
  - Voltage divider calibration: 1.46x (calibrated under load)

- **Battery Display**
  - Battery icon on home screen (top left of CGM card)
  - Battery percentage display (4px spacing to the right of icon)
  - Color-coded battery icons:
    - Green: 90-100% (full battery)
    - White: 60-89% (3/4 battery)
    - Orange: 30-59% (1/2 battery) and 10-29% (1/4 battery)
    - Red flashing: 0-9% (empty battery)

- **Battery Percentage Mapping**
  - Calibrated for under-load voltage (realistic usage)
  - 4.0V or higher = 100% (provides buffer for voltage sag)
  - 3.9V = 90%, 3.8V = 80%, 3.7V = 65%
  - 3.6V = 40%, 3.5V = 20%, 3.4V = 10%
  - 3.2V = 0% (safe shutdown threshold)

- **Low Battery Protection**
  - 10% warning: 3 beeps (800 Hz, 250ms apart)
  - Red flashing icon at <10% battery
  - Automatic shutdown at 3.2V to prevent battery damage
  - Deep sleep mode until USB reconnected

### Changed - Alarm System Improvements
- **Slider UX Enhancements**
  - "Slide to disarm" text moved inside slider
  - Text color changed to white for better visibility
  - Slider height increased to 40px to accommodate label
  - Snap-back animation: if released before 100%, slider returns to 0

- **Disarm Button Functionality**
  - Changed from permanent dismiss to 3-hour snooze
  - Button text: "Disarm All Alarms (3 Hours)"
  - Provides extended quiet period (180 minutes)

- **LED Breathing Effect**
  - Disabled LED breathing task to reduce power consumption
  - LED now off by default

### Fixed
- **Critical**: Fixed crash when sliding alarm to disarm
  - Root cause: Deleting slider label that was already auto-deleted with parent slider
  - Solution: Set label pointer to NULL before deleting parent slider

## [1.2.0] - 2026-01-30

### Added - Visual & Audio Alarm System
- **Visual Alarm System**
  - Pulsing white screen overlay at 2 Hz (250ms on/off cycles)
  - Full-screen slider dismissal mechanism (must slide to 100%)
  - Visual alarm always appears above all other UI elements
  - LVGL timer-based pulsing effect for consistent timing

- **Snooze/Dismiss Functionality**
  - Snooze/dismiss screen after slider activation
  - Snooze options: 5, 10, 15, 30 minutes
  - Dismiss button for permanent alarm dismissal
  - Unix timestamp-based snooze timer for accuracy
  - Alarm re-triggers after snooze expiration, even if glucose value unchanged
  - Current glucose value displayed prominently on snooze screen

- **Audio Alarm Repeat**
  - Configurable audio repeat toggle in alarm detail editor
  - Repeating alarms play tone every 3 seconds until dismissed
  - Default: Urgent alarms (high/low) repeat, warnings play once
  - Audio stops when slider is dismissed
  - LVGL timer-based repeat mechanism

- **Alarm Detail Editor Improvements**
  - Added "Repeat Until Dismissed" toggle switch
  - Removed color selection UI (hardcoded red for urgent, amber for warnings)
  - Shifted UI elements for better spacing and readability
  - All settings save to NVS and persist across reboots

### Changed
- Alarm system now accessible only from main Settings menu (removed from Dexcom screen)
- Back button on alarm settings screen now returns to Settings menu instead of Dexcom screen
- Snooze screen layout optimized with proper vertical spacing
- Dismiss button reduced from 50 to 35 pixels height to prevent overlap

### Fixed
- **Critical**: Fixed LoadProhibited crash when pressing back button on Alarm settings screen
  - Root cause: Attempted to load NULL `screen_dexcom_auth` pointer
  - Solution: Changed to load `screen_menu` instead
- **Critical**: Fixed snooze timer not re-triggering alarm after expiration
  - Root cause: Alarm only triggered on state changes, not snooze expiration
  - Solution: Added separate snooze expiration check independent of state changes
- **Critical**: Fixed white screen not pulsing during visual alarm
  - Root cause: Used wrong opacity function (`lv_obj_set_style_opa` instead of `lv_obj_set_style_bg_opa`)
  - Solution: Changed to background-specific opacity and added `lv_obj_invalidate()` for forced redraw
- Fixed compilation errors due to missing forward declarations for alarm functions
- Fixed snooze screen layout with dismiss button overlapping snooze buttons

## [1.1.0] - 2026-01-29

### Added - SD Card Logging
- Comprehensive SD card logging system for debugging
- Logs glucose fetch operations, timestamps, and errors
- Overnight testing support to debug data fetch failures
- Log file management with rotation

### Changed
- Improved logging infrastructure for better debugging

## [1.0.0] - 2026-01-28

### Added - Audio Feedback & Memory Time-Sharing
- **Boot sound** - Audio confirmation on device startup
- **Success confirmation** - Audio feedback for successful operations
- **Memory time-sharing** - Dexcom and Weather share HTTPS SSL memory pool
  - Reduces peak memory usage from 120KB to 40KB
  - Enables both features to coexist on ESP32-D0WD

### Fixed
- **Critical**: Fixed status preservation bug - Boot/success sounds no longer overwrite glucose status
- **Critical**: Weather and Dexcom SSL memory conflicts resolved with time-sharing

## [0.9.0] - 2026-01-27

### Added - Alarm System Foundation
- **4 Configurable Alarms**
  - High Alarm (urgent): Default 250 mg/dL, red
  - High Warning: Default 180 mg/dL, amber
  - Low Warning: Default 80 mg/dL, amber
  - Low Alarm (urgent): Default 55 mg/dL, red

- **Alarm Configuration UI**
  - Alarm settings screen with 4 alarm cards
  - Enable/disable toggles for each alarm
  - Detailed alarm editor screen
  - Threshold slider (50-400 mg/dL range)
  - Audio alert toggle
  - Visual alert toggle
  - Tone selection (5 tones: Beep 1/2/3, Chime, Alert)
  - Volume slider (0-100%)
  - Settings persist to NVS

- **Tone Playback System**
  - PWM-based buzzer on GPIO 26
  - 5 alarm tones with configurable volume
  - Tone preview in tone picker screen
  - Implemented using LEDC (PWM) peripheral

- **Alarm Logic**
  - Continuous glucose monitoring in Dexcom task
  - Priority-based alarm selection (urgent over warning)
  - State tracking to prevent repeated triggers
  - Glucose text color updates based on active alarm

### Fixed
- **Critical**: Dexcom SSL handshake failures after authentication
  - Root cause: Multiple SSL connections (120KB peak) exceeded available heap
  - Solution: Persistent HTTP client with connection reuse
  - Results: Peak SSL usage reduced to 40KB, all operations succeed

## [0.8.0] - 2026-01-27 (Evening)

### Added
- Weather display with Open-Meteo API
- OpenStreetMap Nominatim geocoding (HTTPS required)
- Memory optimization for weather HTTPS connections

### Changed
- Reduced LVGL memory usage
- Implemented weather fetch delay to prevent memory conflicts

### Fixed
- NULL pointer crashes in various components
- Tone picker UI fixes
- Weather module memory leaks

## [0.7.0] - 2026-01-27 (Morning)

### Added
- Audio implementation with tone generator
- DTMF tone support for keyboard feedback

### Changed
- Memory optimization to accommodate audio features

## [0.6.0] - 2026-01-26

### Added
- Glucose status display with intelligent messages
  - "Warmup" - Sensor warming up
  - "Signal Lost" - No recent data
  - "No Data" - Never received data
- CGM screen design pattern for reusable UI
- Hardware reference documentation

### Changed
- Improved UI consistency across screens
- Enhanced error handling and user feedback

## [0.5.0] - 2026-01-25

### Fixed
- **Critical**: SSL memory fix for multiple HTTPS connections
  - Configured mbedTLS buffer sizes
  - Enabled connection reuse
  - Optimized certificate verification

## [0.4.0] - 2026-01-24

### Added
- Multi-CGM support (Dexcom, Nightscout, LibreLinkUp)
- Physical keyboard integration (M5Stack CardKB)
- Touch screen support (CST820)
- Trend arrows for glucose direction

## [0.3.0] - 2026-01-23

### Added
- Dexcom Share API integration
- OAuth2 authentication flow
- Persistent HTTP client for API calls

## [0.2.0] - 2026-01-22

### Added
- WiFi manager with connection handling
- NVS (Non-Volatile Storage) configuration system
- Settings persistence across reboots

## [0.1.0] - 2026-01-20

### Added
- Initial project structure
- Basic LVGL UI framework
- Display driver (ST7789V3 320x240)
- Hardware abstraction layer

---

## Legend

- **Added** - New features
- **Changed** - Changes to existing functionality
- **Deprecated** - Soon-to-be removed features
- **Removed** - Removed features
- **Fixed** - Bug fixes
- **Security** - Vulnerability fixes

---

**Note**: This CHANGELOG was created on 2026-01-30. Earlier entries are reconstructed from documentation and git history.
