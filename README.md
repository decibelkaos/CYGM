# CYGM — a dedicated CGM display

A ~$40 open-source glucose display you build yourself. CYGM puts live readings
from **Dexcom Share**, **LibreLinkUp**, or **Nightscout** on a 2.8" color
touchscreen — with alarms that actually wake you, trend charts, weather, a
night face built for 3am, and over-the-air updates. No subscriptions, no app
in the way, no account with us.

**Website & device guide:** [cygm.me](https://cygm.me) ·
[cygm.me/guide.html](https://cygm.me/guide.html)

---

## Hardware

| Part | Detail |
|---|---|
| Board | JC2432W328 (ESP32-D0WD, 2.4 GHz WiFi) |
| Display | 2.8" 320×240 ST7789 TFT (SPI @ 40 MHz) |
| Touch | CST820 capacitive (I2C) |
| Audio | Piezo speaker via 8002A amplifier |
| Extras | RGB status LED, microSD slot, optional LiPo battery |
| Case | 3D-printable ([MakerWorld](https://makerworld.com/en/models/2542672-cygm-case)) |

The easiest install is the **browser flasher at [cygm.me](https://cygm.me)** —
no toolchain needed.

## Features (v0.16.x)

- **Three CGM providers**: Dexcom Share, LibreLinkUp, Nightscout — pick one
  on-device, switch anytime
- **Glanceable home screen**: big value with signed delta, trend arrow with a
  sliding two-arrow alert for rapid change, countdown arc to the next reading,
  stale readings visibly gray out
- **Charts**: 1/3/6/12/24 h with Low/Avg/High, time-in-range, GMI and CV
- **Alarm engine**: four threshold tiers, 28 tones (including a random loud
  sequence your brain can't tune out), escalating volume, quiet hours,
  predictive low warning, data-gap alert, and a non-disableable urgent-low
  safety floor with a full-screen takeover
- **Night face**: scheduled dim hands the screen to one huge zone-colored
  number; the time stays small in the top bar so it can never be misread as
  a glucose value
- **Units & locale**: mg/dL and mmol/L, 12/24 h clock, 80+ timezones, weather
  with sunrise/sunset (Open-Meteo)
- **OTA updates** over WiFi, with a one-time card after each update listing
  what changed; optional CSV logging to microSD
- **Runs fine without a battery**: settings survive power loss, so a
  permanently plugged-in build comes back up configured after an outage

## Building from source

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/) and its
toolchain.

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

Notes:
- After changing any `CONFIG_MBEDTLS_*` option: `idf.py fullclean && idf.py build`.
  The TLS buffer configuration in `sdkconfig.defaults` is load-bearing on this
  no-PSRAM board — see the comments there before changing it.
- LVGL is **v8.4** (via `esp_lvgl_port`); the UI deliberately avoids LVGL
  animations, shadows, and label recoloring, which destabilize this hardware.

## Repository layout

```
main/
├── hardware/    # display/touch, buzzer, battery, LED
├── features/    # time/SNTP, weather, geocoding, glucose history, OTA checker
├── ui/          # each screen, the chart, and the alarm/night UI
├── tasks/       # FreeRTOS background task orchestration
├── *_api.c      # Dexcom Share / LibreLinkUp / Nightscout clients
└── main.c       # entry point, alarm engine, trend arrow renderer
```

## Documentation

- [Device guide](https://cygm.me/guide.html) — setup, every screen, tips,
  troubleshooting
- [Release notes](https://github.com/decibelkaos/CYGM/releases) — what changed
  in each version

## Third-party services

- [Open-Meteo](https://open-meteo.com/) — weather (CC BY 4.0)
- [OpenStreetMap Nominatim](https://nominatim.org/) — geocoding (ODbL)

## License

MIT — built for the diabetes DIY community. #WeAreNotWaiting

---

**⚠️ Medical disclaimer**: CYGM is not a medical device and is not FDA-cleared
or clinically validated. It is a second screen, never your source of truth —
never make treatment decisions from it, and always confirm readings on your
approved CGM receiver or app.
