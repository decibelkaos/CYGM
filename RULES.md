# Project Rules - CYGM (CGM Display)

**Last Updated**: 2026-01-27

These are the working rules for AI assistance on this project. Update this file as needed.

---

## AI Behavior Rules

### 1. Documentation Organization

**📁 Project Structure:**
```
/
├── README.md (clean directory - DO NOT MODIFY without asking)
├── RULES.md (this file - Claude behavior guidelines)
├── docs/
│   ├── hardware/        - Hardware specs, pinouts, schematics
│   ├── features/        - Feature documentation and design patterns
│   └── development/     - Status updates, fixes, optimizations
```

**📝 Documentation Rules:**
- ❌ **DO NOT create files in project root** (except README.md, RULES.md)
- ✅ **DO create documentation in appropriate `docs/` subfolder:**
  - Hardware info → `docs/hardware/`
  - Feature documentation → `docs/features/`
  - Development logs, fixes, status → `docs/development/`
- ✅ **Use dated filenames for revisions:**
  - Example: `MEMORY_OPTIMIZATION_2026-01-27.md`
  - Example: `SSL_FIX_2026-01-16.md`
- ✅ **Keep README.md clean and organized** - it's the entry point
- ✅ **Update README.md links** when creating new docs

### 2. File Handling
- ❌ **DO NOT read PDF files directly**
  - PDFs waste tokens and are hard to parse
  - ✅ **DO**: Ask user to convert PDFs to text first

- ✅ **DO reference demo application source code**
  - Location: `Documents/` folder (if provided)
  - This has most of the answers for implementation
  - Always check demo code before asking questions

### 3. Build & Command Execution
- ❌ **DO NOT run ESP-IDF commands** (idf.py, build, flash, etc.)
  - User runs ALL IDF commands manually
  - ✅ **DO**: Tell user exactly what command to run

- ❌ **DO NOT create scripts or helper files** (.bat, .sh, .ps1, etc.)
  - ✅ **DO**: Ask user directly to do things
  - Keep it simple - no automation scripts

### 4. Code Quality
- ✅ **Keep code simple** - Don't overcomplicate
  - Write straightforward, readable code
  - Avoid over-engineering
  - Avoid premature optimization

- ✅ **Break work into manageable pieces**
  - Don't try to solve everything at once
  - Small, incremental changes
  - Validate each step before moving on

### 5. Token Management
- ✅ **Save tokens** - Be efficient
  - Don't read entire large files unless necessary
  - Use targeted reads with offset/limit
  - Summarize instead of repeating full content
  - Break large tasks into smaller processes

### 6. Development Logging
- ✅ **Create development logs in `docs/development/`**
  - Use dated filenames (e.g., `FEATURE_NAME_2026-01-27.md`)
  - Document: Issues found, goals, steps taken, results
  - Include code snippets, configurations, testing steps
  - Don't log: Casual conversations, status checks

- ✅ **Update PROJECT_STATUS.md when major milestones reached**
  - Located at: `docs/development/PROJECT_STATUS.md`
  - Update feature lists, memory usage, known issues

---

## Project-Specific Rules

### Hardware
- **Device**: JC2432W328 (ESP32-D0WD + 2.8" TFT + Capacitive Touch)
- **Display**: ST7789V3 320x240 (SPI)
- **Touch**: CST820 (I2C, address 0x15)
- **Speaker**: 8002A amplifier on GPIO 26 (PWM via LEDC)
- **LED**: RGB (GPIO 4, 16, 17 - active low)
- **Reference**: See `docs/hardware/HARDWARE_REFERENCE.md`

### Development
- **Framework**: ESP-IDF v5.5.2 (NOT Arduino/PlatformIO)
- **Target**: esp32 (NOT esp32s3, esp32c3, etc.)
- **Graphics**: LVGL 8.4.0
- Use ESP-IDF component registry for drivers when available
- Prefer esp_lcd API for display, esp_lcd_touch for touch

### Pin Assignments (DO NOT CHANGE)
- **Display SPI**: MOSI=13, SCLK=14, CS=15, DC=2, BL=27
- **Touch I2C**: SDA=33, SCL=32, RST=25, INT=21
- **Speaker**: GPIO 26 (PWM)
- **RGB LED**: RED=4, GREEN=16, BLUE=17 (active-low)
- **CardKB**: SDA=21, SCL=22 (I2C_NUM_1, addr 0x5F)
- **Boot Button**: GPIO 0

### Memory Constraints
- **Total RAM**: ~226KB heap
- **LVGL**: 36KB (configurable via menuconfig)
- **SSL Context**: ~40KB (persistent during session)
- **Target Free Heap**: 70-80KB minimum for stable SSL operation

---

## Documentation Index

### Quick Reference Docs
📖 Start here: [README.md](README.md) - Project overview and doc index

### Hardware
- [Hardware Reference](docs/hardware/HARDWARE_REFERENCE.md) - Complete hardware specs with code examples
- [Pinout](docs/hardware/PINOUT.md) - Quick pin reference table

### Features
- [Alarm System](docs/features/ALARM_SYSTEM.md) - Configurable glucose alarms
- [CGM Screen Pattern](docs/features/CGM_SCREEN_DESIGN_PATTERN.md) - Reusable UI pattern

### Development
- [Project Status](docs/development/PROJECT_STATUS.md) - Current state, features, memory usage
- [SSL Memory Fix](docs/development/SSL_MEMORY_FIX.md) - Critical SSL configuration (2026-01-16)
- [Memory Optimization](docs/development/MEMORY_OPTIMIZATION_2026-01-27.md) - Audio + memory fixes (latest)

---

## How to Update Rules

When user says "Add this rule..." or "Change rule...":
1. Edit this file directly
2. Keep format clean and organized
3. Update the "Last Updated" date at the top
4. Confirm change with user

---

## Quick Commands

**Build & Flash:**
```bash
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

**Configure:**
```bash
idf.py menuconfig
# Component config → LVGL → Memory Settings → [36 KB recommended]
```

**Clean Build:**
```bash
idf.py fullclean
idf.py build
```

---

## User Responsibilities
- Run all `idf.py` commands
- Handle build/flash operations
- Convert PDFs to text when needed
- Test hardware functionality

## AI Responsibilities
- Write/edit code
- Create organized documentation in `docs/` folder
- Provide analysis and debugging
- Give clear, actionable instructions
- Keep documentation structure clean

---

*Keep this organized. Keep the root clean. Use the docs/ folder properly.*
