# CYGM — a dedicated CGM display

A ~$40 open-source glucose display you build yourself. CYGM puts shared readings
from **Dexcom Share**, **LibreLinkUp**, or **Nightscout** on a 2.8" color
touchscreen — with configurable bedside alerts, trend charts, weather, a night
face built for 3am, and over-the-air updates. No CYGM account, no CYGM
subscription.

CYGM is a **secondary display**. It reads what your CGM system publishes to its
cloud, so you keep using your official app or receiver and keep its alerts on.

**Website & device guide:** [cygm.me](https://cygm.me) ·
[cygm.me/guide.html](https://cygm.me/guide.html)

---

## Hardware

| Part | Detail |
|---|---|
| Board | JC2432W328**C** — the **capacitive** variant (ESP32-D0WD, 2.4 GHz WiFi) |
| Display | 2.8" 320×240 ST7789 TFT (SPI @ 40 MHz) |
| Touch | CST820 capacitive (I2C) |
| Audio | Amplified speaker output (8002A); optional speaker makes alarms louder |
| Extras | RGB status LED, microSD slot, optional LiPo battery |
| Case | 3D-printable ([MakerWorld](https://makerworld.com/en/models/2542672-cygm-case)) |

A visually identical **resistive** board is sold under nearly the same name and
its touch will not work. Check the listing says capacitive.

The easiest install is the **browser flasher at [cygm.me](https://cygm.me)** —
no toolchain needed.

## Features (v0.16.x)

- **Three data sources**: Dexcom Share, LibreLinkUp, Nightscout — pick one
  on-device, switch anytime. Nightscout is a data source rather than a CGM
  maker, so through it CYGM shows any CGM your Nightscout setup already handles
- **Glanceable home screen**: big value with signed delta, trend arrow with a
  sliding two-arrow alert for rapid change, a countdown arc until CYGM next
  checks for data, and stale readings that visibly gray out
- **Polling, not magic**: CYGM checks the cloud about every 90 seconds. How
  often a genuinely new reading appears is set by your CGM system — Dexcom
  typically every five minutes, Libre 3 and 3 Plus as often as every minute
- **Charts**: 1/3/6/12/24 h with Low, Average, High and **Configured Range**,
  the share of readings between *your* configured warning thresholds. That is
  not clinical Time in Range. GMI and CV are shown as short-window estimates
  and are not comparable with a standard 14-day report
- **Alarm engine**: four threshold tiers, 28 tones (including a randomized loud
  sequence designed to reduce habituation), escalating volume, quiet hours,
  predictive low warning, a data-gap alert that suggests likely causes, and a
  non-disableable urgent-low safety floor with a full-screen takeover
- **Night face**: scheduled dim hands the screen to one huge zone-colored
  number; the time stays small in the top bar so it is not misread as a
  glucose value
- **Units & locale**: mg/dL and mmol/L, 12/24 h clock, 80+ timezones, weather
  with sunrise/sunset
- **OTA updates** over WiFi, with a one-time card after each update listing
  what changed; optional CSV logging to microSD
- **Runs fine without a battery**: settings survive power loss, so a
  permanently plugged-in build comes back up configured after an outage

## Privacy

There is no CYGM account and no CYGM cloud. CGM credentials are stored locally
on the device and are sent only to the CGM service you selected, when CYGM signs
in to it. Readings go from that service straight to your device; none pass
through a CYGM server. Flash and storage encryption are **not** enabled in this
build, and firmware images are not separately signed, so install only from a
source you trust and run **Erase Device** before passing a unit on.

Full detail, including every host the device contacts:
[cygm.me#privacy](https://cygm.me#privacy).

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
├── hardware/    # display/touch, audio, battery, LED
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

- [Open-Meteo](https://open-meteo.com/) — weather and place-name geocoding (CC BY 4.0)
- [Zippopotam.us](https://zippopotam.us/) — postal-code lookup

## License

MIT — built for the diabetes DIY community. #WeAreNotWaiting

---

**⚠️ Medical disclaimer**: CYGM is an open-source **experimental secondary CGM
display**. It is **not FDA-cleared or FDA-approved** and has **not been
clinically validated**. Do not use it as the sole basis for a treatment
decision, or as a replacement for your prescribed CGM receiver or app and its
alerts. Always confirm glucose information on your official CGM system before
dosing or treating.
