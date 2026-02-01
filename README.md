# CYGM - Continuous Glucose Monitor Display
## Hardware: JC2432W328 (ESP32-D0WD) + M5Stack CardKB

**Project Status**: 🎉 **BETA STAGE** - Ready for Field Testing
**Version**: 1.3.2
**Last Updated**: January 31, 2026

An open-source, low-cost CGM display supporting multiple data sources (Dexcom Share, Nightscout, LibreLinkUp).

---

## Quick Start

### Build & Flash
```bash
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

### Hardware
- **Display**: ST7789V3 320x240 TFT (SPI)
- **Touch**: CST820 capacitive (I2C)
- **Speaker**: 8002A amplifier on GPIO 26
- **Keyboard**: M5Stack CardKB (I2C)
- **LED**: RGB (GPIO 4, 16, 17)

---

## Features
✅ Multi-CGM support (Dexcom, Nightscout, LibreLinkUp)
✅ **Comprehensive alarm system** - Visual (pulsing white screen) and audio alerts
✅ **Snooze functionality** - Slider dismissal with 5/10/15/30 minute snooze + 3-hour disarm
✅ **Audio repeat** - Configurable repeating alarms for urgent states
✅ **Battery monitoring** - Voltage display, percentage, low battery protection (shutdown at 3.2V)
✅ **Enhanced Trend Arrows** - Larger arrows (60x60 canvas) with improved design:
   - Green glow for stable trends (flat, 45° angles)
   - Orange glow for single up/down arrows
   - Red glow for double up/down arrows
   - Rotated from SINGLE_UP pattern for consistency
   - Extended shafts for better visibility
✅ Weather display with location detection (memory time-sharing)
✅ Audio feedback (boot sound, connection confirmation, alarm tones)
✅ **LED visual feedback** - RGB LED status indicators:
   - Green blink (200ms) during WiFi boot sequence
   - Blue double-blink ramp (5 steps, 1.5s) before Dexcom poll
   - Green fade (3s steady, 500ms fade) on successful glucose fetch
   - Red blink (500ms) for errors
   - Bright red flash (100ms) during alarms
✅ Intelligent status messages (warmup, signal loss, no data)
✅ Physical keyboard for easy setup
✅ Large, readable display with trend arrows
✅ Persistent settings (NVS storage)
✅ **No API keys required** - perfect for open-source diabetic community!

---

## Architecture

### Modular Structure (v1.3.1+)

**Refactored 2026-01-31**: main.c reduced from 6,524 lines to 1,420 lines (78% reduction)

```
main/
├── hardware/          # Hardware abstraction
│   ├── led.c/h       # RGB LED PWM control
│   ├── buzzer.c/h    # Tone generation, alarms
│   ├── battery.c/h   # ADC monitoring, shutdown protection
│   └── display.c/h   # LCD, touch, CardKB, LVGL
├── features/          # High-level features
│   ├── time_system.c/h    # SNTP, timezone, time display
│   └── weather_system.c/h # Geocoding, Open-Meteo API
├── ui/                # UI screens
│   ├── home_screen.c/h    # Time, weather, glucose
│   ├── menu_screen.c/h    # Settings menu
│   ├── wifi_screens.c/h   # WiFi configuration
│   ├── cgm_screens.c/h    # CGM provider setup
│   └── alarm_screens.c/h  # Alarm configuration
├── tasks/             # Background FreeRTOS tasks
│   └── background_tasks.c/h # Core 0/1 task management
├── shared_state.h     # Global state declarations
└── main.c             # Coordinator (1,420 lines)
```

**Benefits**: Clear separation of concerns, easy navigation, maintainable codebase

## Documentation

📖 **[RULES.md](RULES.md)** - Claude development guidelines and project constraints

### Hardware Reference
- **[Hardware Reference](docs/hardware/HARDWARE_REFERENCE.md)** - Complete pin mapping, code examples, and hardware specifications
- **[Pinout](docs/hardware/PINOUT.md)** - Quick pin reference table

### Feature Documentation
- **[Alarm System](docs/features/ALARM_SYSTEM.md)** - Configurable glucose alarms with audio/visual alerts
- **[Audio Feedback](docs/features/AUDIO_FEEDBACK_2026-01-28.md)** - Boot sound & success confirmation with status preservation fix
- **[SD Card Logging](docs/features/SD_CARD_LOGGING_2026-01-29.md)** - Comprehensive debugging logs for overnight testing
- **[CGM Screen Design Pattern](docs/features/CGM_SCREEN_DESIGN_PATTERN.md)** - Reusable UI pattern for CGM account screens
- **[Glucose Status Display](docs/features/GLUCOSE_STATUS_DISPLAY.md)** - Intelligent status messages (warmup, signal loss, etc.)

### Development & Fixes
- **[Project Status](docs/development/PROJECT_STATUS.md)** - Complete feature list, memory usage, and architecture overview
- **[Versioning Guide](docs/development/VERSIONING.md)** - Version history, stages (Alpha/Beta/Release), and update instructions
- **[SSL Memory Fix](docs/development/SSL_MEMORY_FIX.md)** - Critical SSL configuration for multiple HTTPS connections
- **[Memory Optimization](docs/development/MEMORY_OPTIMIZATION_2026-01-27.md)** - Audio implementation and memory fixes (morning)
- **[Audio & Weather Fix](docs/development/AUDIO_WEATHER_FIX_2026-01-27.md)** - Buzzer test beep, LVGL reduction, weather delay (evening session 1)
- **[Critical Memory Fixes](docs/development/CRITICAL_MEMORY_FIXES_2026-01-27_EVENING.md)** - NULL pointer crashes, tone picker fix, weather disabled (evening session 2)
- **[Open-Meteo HTTP Weather](docs/development/OPENMETEO_HTTP_IMPLEMENTATION_2026-01-28.md)** - HTTP weather attempt (morning session)
- **[Weather HTTPS Required](docs/development/WEATHER_HTTPS_REQUIRED_2026-01-28.md)** - Nominatim requires HTTPS, memory critically tight (evening)
- **[Memory Time-Sharing](docs/development/MEMORY_TIMESHARING_2026-01-28.md)** - Dexcom and Weather share memory pool (evening)
- **[Audio Feedback & Status Fix](docs/development/SESSION_SUMMARY_2026-01-28_LATE_EVENING.md)** - Boot/success sounds, status preservation bug fix (late evening)
- **[SD Card Logging](docs/development/SD_LOGGING_IMPLEMENTATION_2026-01-29.md)** - Debug overnight glucose fetch failures (morning - latest)

---

## Third-Party Services

Weather data powered by:
- **[Open-Meteo](https://open-meteo.com/)** - Free weather API for open-source projects (CC BY 4.0)
- **[OpenStreetMap Nominatim](https://nominatim.org/)** - Free geocoding service (ODbL, requires HTTPS)

---

## License
MIT License - Built for the diabetes DIY community

---

**⚠️ Medical Disclaimer**: This is not a medical device. Use for informational purposes only. Always confirm readings with your approved CGM device.
