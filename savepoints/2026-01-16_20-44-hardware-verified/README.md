# CYGM - Continuous Glucose Monitor Display
## Hardware: JC2432W328 (ESP32-D0WD)

### Project Status: Hardware Verification COMPLETE ✓

---

## Verified Pin Configuration

| Component | GPIO | Interface | Status |
|-----------|------|-----------|--------|
| **Display** | 13,14,15,2,27 | SPI (ST7789) | ✅ Working |
| **Touch** | 33,32,25,21 | I2C (CST820) | ✅ Working |
| **Speaker** | 26 | PWM → 8002A amp | ✅ Working |
| **RGB LED** | 4,16,17 | GPIO (active low) | ✅ Working |
| **BOOT Button** | 0 | GPIO input | ✅ Working |
| **SW1 Button** | RST/EN | Hardware reset | ⚠️ Not usable as GPIO |

---

## Pin Details

### Display (ST7789 320x240 SPI)
- MOSI: GPIO 13
- SCLK: GPIO 14
- CS: GPIO 15
- DC: GPIO 2
- Backlight: GPIO 27
- RST: Not connected (use -1)

### Touch (CST820 I2C @ 0x15)
- SDA: GPIO 33
- SCL: GPIO 32
- RST: GPIO 25
- INT: GPIO 21
- Calibration: SWAP_XY=1, MIRROR_Y=1

### Speaker
- GPIO 26 (DAC2/PWM)
- Connected via 8002A audio amplifier
- Output: P4 connector (JST 1.25mm 2-pin)

### RGB LED (Active Low)
- Red: GPIO 4
- Green: GPIO 16
- Blue: GPIO 17
- Set LOW to turn ON, HIGH to turn OFF

### Buttons
- BOOT: GPIO 0 (active low, usable after boot)
- SW1: Connected to RST/EN (hardware reset only)

---

## Build Commands
```
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

## Next Steps
1. WiFi connectivity
2. Nightscout API integration
3. CGM data display UI
4. High/Low glucose alarms
5. Historical graph display

---

## Hardware Notes
- Board: JC2432W328 (Guition W3 series)
- MCU: ESP32-WROOM-32 (ESP32-D0WD)
- Flash: 4MB
- Display: 2.8" 320x240 TFT
- Touch: Capacitive (CST820)
- Power: 5V USB-C

---

## Documentation

- **[HARDWARE_REFERENCE.md](HARDWARE_REFERENCE.md)** - Complete hardware reference with code examples
- **[PINOUT.md](PINOUT.md)** - Quick pin reference
- **[STATUS.md](STATUS.md)** - Project status and roadmap
