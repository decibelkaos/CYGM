# Building from source

CYGM's firmware uses **ESP-IDF v5.5** and **LVGL v8.4**. You need the ESP-IDF toolchain installed before building.

## Build and flash

From the firmware repository:

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your board's serial port.

Changing `CONFIG_MBEDTLS_*` settings requires a clean build:

```bash
idf.py fullclean
idf.py build
```

The default TLS buffer settings matter on this no-PSRAM board. Read the comments in `sdkconfig.defaults` before changing them.

## Where things live

| Location | Main responsibility |
| --- | --- |
| `main/hardware/` | Display, touch, speaker, battery, LED |
| `main/features/` | Time, weather, history, OTA |
| `main/ui/` | Screens, charts, alarms, night display |
| `main/tasks/` | FreeRTOS background work |
| `main/*_api.c` | Dexcom, LibreLinkUp, Nightscout access |
| `main/main.c` | Startup and core application behavior |

## Testing changes

A successful compile isn't proof that a change behaves correctly on the hardware. For power and display changes, test USB charging, USB with a full battery, USB without a battery, and battery-only operation where applicable.

For glucose code, keep measurement timestamps distinct from fetch times. Duplicate network responses shouldn't make old measurements appear fresh.

For alarm-related changes, test carefully on a secondary device while keeping your official CGM alerts enabled.

See [Contributing](Contributing.md) before sending a pull request.
