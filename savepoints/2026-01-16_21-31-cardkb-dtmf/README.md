# CYGM - Continuous Glucose Monitor Display
## Hardware: JC2432W328 (ESP32-D0WD) + M5Stack CardKB

### Project Status: Hardware Verification COMPLETE ✓

An open-source, low-cost CGM display supporting multiple data sources.

---

## Features
- **Multi-CGM Support**: Nightscout, Dexcom Share, LibreLinkUp
- **Large Display**: Easy-to-read glucose value and trend arrow
- **Audible Alarms**: High/low glucose alerts with DTMF tones
- **Physical Keyboard**: M5Stack CardKB for easy configuration
- **Portable**: Small form factor, USB-C powered

---

## Hardware

| Component | GPIO | Status |
|-----------|------|--------|
| Display (ST7789) | 13,14,15,2,27 | ✅ Working |
| Touch (CST820) | 33,32,25,21 | ✅ Working |
| Speaker | 26 | ✅ Working |
| RGB LED | 4,16,17 | ✅ Working |
| BOOT Button | 0 | ✅ Working |
| CardKB | 21,22 (I2C) | ✅ Working |

---

## Supported CGM Sources
- **Nightscout** - Self-hosted, CGM-agnostic
- **Dexcom Share** - Direct Dexcom connection
- **LibreLinkUp** - Abbott Freestyle Libre

---

## Build Commands
```bash
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

---

## Documentation

- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design and API details
- **[HARDWARE_REFERENCE.md](HARDWARE_REFERENCE.md)** - Complete hardware reference with code examples
- **[PINOUT.md](PINOUT.md)** - Quick pin reference
- **[STATUS.md](STATUS.md)** - Project status and roadmap

---

## License
Open source - MIT License

---

*Built for the diabetes DIY community*
