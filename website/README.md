# CYGM Website & Firmware

Website and firmware distribution for the CYGM Continuous Glucose Monitor Display.

## File Structure

```
website/
├── index.html          # Landing page
├── version.json        # Current version (used by device OTA checker)
├── releases.json       # All versions + firmware paths + release notes
├── cygm-manifest.json  # ESP Web Tools manifest (browser flashing)
├── photos/             # Gallery photos and device screenshots
└── firmware/
    └── v0.5.0/
        ├── cygm-merged.bin       # Single merged binary (for browser flash)
        ├── cygm.bin              # App firmware (for OTA updates)
        ├── bootloader.bin        # Bootloader
        ├── partition-table.bin   # Partition table
        └── ota_data_initial.bin  # OTA data partition
```

## Releasing a New Version

### 1. Build the firmware
```bash
idf.py build
```

### 2. Create a merged binary
```bash
cd build
esptool --chip esp32 merge_bin ^
  -o cygm-merged.bin ^
  --flash_mode dio --flash_freq 40m --flash_size 4MB ^
  0x1000 bootloader/bootloader.bin ^
  0x8000 partition_table/partition-table.bin ^
  0xd000 ota_data_initial.bin ^
  0x10000 cygm.bin
```

### 3. Copy files to website
```bash
mkdir website\firmware\vX.Y.Z
copy build\cygm-merged.bin website\firmware\vX.Y.Z\
copy build\cygm.bin website\firmware\vX.Y.Z\
copy build\bootloader\bootloader.bin website\firmware\vX.Y.Z\
copy build\partition_table\partition-table.bin website\firmware\vX.Y.Z\
copy build\ota_data_initial.bin website\firmware\vX.Y.Z\
```

### 4. Update JSON files
- `version.json` — update version, date, firmware_url
- `releases.json` — add new entry at top, move `"latest": true`
- `cygm-manifest.json` — update version and firmware path

### 5. Update `shared_state.h`
Update `CYGM_VERSION_*` defines to match the new version.

### 6. Commit and deploy
```bash
git add website/ main/shared_state.h
git commit -m "Release: vX.Y.Z Beta"
git tag vX.Y.Z
```

## Version Scheme

- `0.x` — Pre-release beta (current)
- `1.0` — First stable public release
- Versions follow semver: `major.minor.patch`
