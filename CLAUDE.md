# CYGM - Continuous Glucose Monitor Display

## Project Overview
ESP32-based CGM display device (JC2432W328: ESP32 + ST7789 LCD + CST820 touch).
Reads glucose data from Dexcom, LibreLinkUp, or Nightscout and displays it with weather, time, and alarms.

## Tech Stack
- **MCU**: ESP32-D0WD (Xtensa dual-core, no PSRAM, ~320KB RAM)
- **Framework**: ESP-IDF v5.5.2 (C, FreeRTOS)
- **UI**: LVGL v8.4 (via esp_lvgl_port) — NOT v9
- **Display**: ST7789 240x320 SPI LCD (landscape)
- **Touch**: CST820 I2C capacitive touch
- **Build**: CMake + idf.py (Ninja backend)

## Build Commands
**User runs all idf.py commands manually — do NOT run them from Claude.**
```bash
idf.py build                          # Build
idf.py -p COMx flash monitor          # Flash + serial monitor
idf.py fullclean && idf.py build      # Clean rebuild (required after sdkconfig changes)
idf.py menuconfig                     # Interactive config editor
idf.py monitor -p COMx               # Serial monitor only
```

## Project Structure
```
main/
  main.c / main.h             - Entry point, splash, global styles, glucose_update_task
  dexcom_api.c/.h              - Dexcom Share API client (persistent HTTPS)
  wifi_manager.c/.h            - WiFi STA connection management
  nvs_config.c/.h              - NVS persistent key-value storage
  sd_logger.c/.h               - SD card CSV logging
  shared_state.h               - All global variable declarations (externs)
  ui_config.c/.h               - UI theme constants
  lv_conf.h                    - LVGL build configuration
  arrow_images.c/.h            - Trend arrow bitmaps
  features/
    weather_system.c/.h        - Open-Meteo weather API
    time_system.c/.h           - SNTP time sync, timezone
    glucose_history.c/.h       - Glucose reading ring buffer
    geocoding_api.c/.h         - Location geocoding (online + offline)
    update_checker.c/.h        - OTA firmware update checker + downloader
  hardware/
    display.c/.h               - ST7789 + CST820 init via esp_lcd
    buzzer.c/.h                - Piezo buzzer (LEDC PWM)
    battery.c/.h               - ADC battery voltage reading
    led.c/.h                   - RGB status LED control
  tasks/
    background_tasks.c/.h      - FreeRTOS task orchestration (wifi, ui, battery)
  ui/
    home_screen.c/.h           - Main glucose display screen
    cgm_screens.c/.h           - CGM detail/settings screens
    alarm_screens.c/.h         - Alarm configuration UI
    menu_screen.c/.h           - Main menu navigation
    wifi_screens.c/.h          - WiFi setup screens
    time_weather_screens.c/.h  - Time/weather display screens
    glucose_chart.c/.h         - Glucose trend chart widget
```

## Task Architecture
All global task handles declared in `shared_state.h`, defined in `main.c`.

| Task | Stack | Core | Priority | Handle | Purpose |
|------|-------|------|----------|--------|---------|
| wifi_task | 8KB | 0 | 4 | `wifi_task_handle` | WiFi connect, SNTP, Dexcom auth, starts other tasks |
| ui_task | 8KB | 1 | 5 | `ui_task_handle` | Boot sequence, inactivity watchdog |
| glucose_update | 4KB | 0 | 5 | `glucose_task_handle` | 90s periodic glucose fetch + update check |
| weather_task | 4KB | 0 | 3 | `weather_task_handle` | Periodic weather API fetch (HTTP only) |
| time_update | 4KB | 0 | 3 | `time_task_handle` | Clock/date UI updates |
| battery_mon | 3KB | 0 | 2 | `battery_task_handle` | ADC battery voltage monitoring |
| led_task | 2KB | 0 | 5 | local in led.c | LED breathing animation |
| LVGL port | internal | 1 | — | managed by esp_lvgl_port | Display refresh (independent) |

## Key Conventions
- All UI runs on main task or LVGL port task; use `lvgl_port_lock()/unlock()` from other tasks
- FreeRTOS tasks for background work (glucose polling, weather updates)
- NVS for persistent config (WiFi creds, alarm thresholds, preferences)
- Ghost button style + card-based UI theme (see `ui_config.h` for colors)
- OTA dual-slot partition layout (ota_0 + ota_1, no SPIFFS)
- Commit messages: `Category: description` (Feature:, Fix:, Style:, Refactor:, Docs:)
- All new global variables: declare `extern` in `shared_state.h`, define in `main.c`
- Do NOT create files in project root — use `docs/` subfolders (see RULES.md)

## Critical: TLS/SSL Configuration
These settings in `sdkconfig.defaults` were debugged extensively. **Do not change without understanding the impact.**

```
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192     # OTA uses Range requests (4KB chunks), Dexcom responses fit in 8KB
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=1024     # Device sends small requests (<500B); mbedTLS splits larger ones automatically
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y             # Allocate/free TLS buffers on demand (saves idle RAM)
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y   # Free handshake config after completion
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y       # Free CA cert after verification
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y    # Server CA not in ESP-IDF bundle
```

**NEVER enable `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`** — it aggressively resizes buffers post-handshake, causing `-0x7100` read errors on streaming connections. This is a known ESP-IDF bug (issues #17816, #11833). Use `DYNAMIC_BUFFER` instead.

**Server info**: cygm.me runs Apache on IONOS, 4-cert Sectigo chain, TLS 1.2. Sends 16384-byte TLS application data records for firmware binaries.

## Critical: OTA Memory Management
OTA over HTTPS requires ~17KB contiguous heap for the TLS IN buffer. The ESP32's TLSF allocator cannot defragment. Before OTA download:

1. `update_stop_all_tasks()` deletes ALL non-essential tasks (~35KB freed)
2. WiFi driver runs independently (event-driven) — safe to delete our wifi_task
3. LVGL port has its own refresh task — safe to delete our ui_task
4. After OTA failure: device **reboots** (deleted tasks can't be recreated)
5. The OTA fallback task stack (6KB) fragments heap — keep it minimal

## Memory Gotchas
- ESP32 has ~320KB RAM, no PSRAM — every KB matters
- `heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` is the critical metric, not total free
- Dexcom persistent SSL client holds ~15-20KB while open — close with `dexcom_close_persistent_client()` before OTA
- `network_mutex` serializes all SSL operations — always acquire before network I/O
- LVGL v8 API: `lv_obj_create`, `lv_label_create`, etc. (NOT v9 syntax)
- Always use `esp_http_client` for HTTP requests (not raw sockets)
- **ALWAYS** call `esp_http_client_close()` before `esp_http_client_cleanup()` — prevents socket leaks
- **NEVER** use `lv_anim_start()`, shadows, or `lv_label_set_recolor()` — causes device freeze
- Always close SSL after each glucose fetch for ALL TLS providers
- Delete background tasks before creating any new task >3KB on fragmented heap

## sdkconfig Danger Zones
Changes to these require `idf.py fullclean && idf.py build`:
- Any `CONFIG_MBEDTLS_*` option (TLS buffer sizes, cipher suites, dynamic buffer)
- `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` — must be 16384 for Apache OTA compatibility
- `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` — must stay DISABLED
- ECC curve options (SECP384R1 required for Dexcom certs)
- `CONFIG_MBEDTLS_SHA512_C` — must stay enabled (SHA-384 for Dexcom)

## Debugging Workflow
1. Serial logs are saved to `logs/serial.log` — read with `Read` tool
2. Look for `E (` prefix for errors, `W (` for warnings
3. TLS errors: `-0x7100` = BAD_INPUT_DATA (buffer resize bug), `-0x7F00` = ALLOC_FAILED (need more contiguous memory)
4. Memory issues: check `largest_block` in UPDATE/CYGM log lines
5. SD card logs via `sd_log()` — flushed with `sd_logger_flush()`

## Additional Rules
See [RULES.md](RULES.md) for:
- Root cleanliness rules (don't create files in project root)
- Hardware pin assignments (DO NOT CHANGE)
- Documentation organization (`docs/` subfolders)
- User vs AI responsibilities
