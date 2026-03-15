# CYGM Master To-Do List

**Last Updated:** 2026-03-15
**Current Version:** 0.5.1 Beta (firmware v1.7.0 on server)

---

## Completed Features

### Hardware
- [x] ST7789 LCD display (320x240, SPI, landscape)
- [x] CST820 capacitive touch (polling mode)
- [x] RGB LED visual feedback (WiFi, Dexcom poll, success, error, alarm)
- [x] Piezo buzzer alarm tones (5 tones, configurable volume)
- [x] Battery monitoring (ADC, voltage divider, low-battery shutdown)
- [x] Brightness control (PWM backlight, 1-100%, saved to NVS)

### Networking & Time
- [x] WiFi STA with multi-network NVS storage (up to 3 saved)
- [x] SNTP time sync (3 servers)
- [x] Timezone with DST support
- [x] TLS/SSL with dynamic buffers (VARIABLE_BUFFER_LENGTH bug avoided)

### Dexcom Integration
- [x] Share API client (persistent HTTPS, auto-reauth)
- [x] Multi-region support (US, OUS, Japan)
- [x] Credential storage in NVS
- [x] 90-second polling with trend arrows
- [x] Glucose history ring buffer
- [x] Glucose trend chart (LVGL chart widget)
- [x] Sensor change mode handling

### Weather
- [x] Open-Meteo API (current + hi/lo temps, conditions, sunrise/sunset)
- [x] Online geocoding via Open-Meteo (worldwide, returns timezone)
- [x] Geocoding results cached in NVS (coordinates + location name)
- [x] Animated weather icons (7 conditions)
- [x] Configurable update interval (5-90 min)

### Alarm System
- [x] 4 configurable alarms (high alarm/warning, low warning/alarm)
- [x] Visual alarm overlay (pulsing, slider dismiss)
- [x] Audio alarms (5 tones, repeat/one-shot, volume control)
- [x] Snooze system (5/10/15/30 min, 3hr dismiss)
- [x] All alarm settings persist to NVS

### UI
- [x] Home screen (time, weather, glucose with expanded CGM view)
- [x] Menu system with card-based ghost button theme
- [x] WiFi setup screens (scan, connect, multi-network management)
- [x] Dexcom login screens (username/password entry)
- [x] Time/weather settings screens
- [x] Alarm settings + detail editor + tone picker
- [x] Brightness overlay
- [x] On-screen QWERTY keyboard (caps, symbols, backspace)
- [x] Glucose fetch progress bar + manual fetch

### Infrastructure
- [x] OTA dual-slot partition layout (ota_0 + ota_1, rollback)
- [x] OTA firmware update checker + downloader
- [x] SD card CSV glucose logging (toggle in settings)
- [x] SD card screenshot capture (BMP)
- [x] Code refactoring (monolithic main.c -> 13+ modules)
- [x] Website with ESP Web Tools flasher (merged binary)
- [x] 13 efficiency optimizations applied (compiler -Os, IPv6 disabled, nano printf, etc.)

---

## Open Items

### Optimization
- [ ] **Log level INFO -> WARN for production** (Item M from efficiency checklist)
  - Removes hundreds of ESP_LOGI() format strings from flash
  - Keep INFO during development, WARN for release builds
  - `idf.py menuconfig` -> Component config -> Log -> Default log verbosity -> Warning

### Future Features — High Priority
- [ ] **Nightscout API integration** — alternative CGM data source, REST API with custom URL
- [ ] **LibreLinkUp API integration** — Abbott Freestyle Libre support

### Future Features — Medium Priority
- [ ] **Custom color themes** — user-selectable UI palettes

### Future Features — Low Priority
- [ ] **BLE phone sync** — Bluetooth Low Energy for phone notifications
- [ ] **Multiple Dexcom accounts** — follow multiple people

---

## Resolved / Won't Do

- ~~Web server~~ — removed from codebase; ESP32 doesn't have enough RAM alongside SSL
- ~~Offline postal code database~~ — 2.3MB data won't fit in flash; online geocoding works worldwide
- ~~SSL_IN_CONTENT_LEN reduction~~ — must stay 16384 for Apache OTA compatibility
- ~~VARIABLE_BUFFER_LENGTH~~ — causes -0x7100 errors, permanently disabled
- ~~Web flasher issues~~ — all fixes applied (COM port dialog, flash terminal, merged binary)
