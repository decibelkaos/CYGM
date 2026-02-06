# CYGM Web Firmware Flasher

Browser-based firmware installer for the CYGM Continuous Glucose Monitor Display.
Uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) to flash ESP32 firmware
directly from Chrome or Edge — no drivers or software installation needed.

## How It Works

1. **User visits the page** (hosted on GitHub Pages or any static host)
2. **Selects a firmware version** from the dropdown (populated from `releases.json`)
3. **Clicks "Install Firmware"** — browser prompts for serial port selection
4. **ESP Web Tools handles the flash** — progress bar, chip detection, error handling

## File Structure

```
website/
├── index.html          # The flasher webpage (self-contained)
├── releases.json       # Firmware version manifest
├── README.md           # This file
└── firmware/           # Binary files organized by version
    └── v1.5.1/
        ├── cygm-merged.bin       # Single merged binary (preferred)
        ├── cygm.bin              # App firmware only
        ├── bootloader.bin        # Bootloader
        ├── partition-table.bin   # Partition table
        └── spiffs.bin            # SPIFFS web files
```

## Adding a New Firmware Release

### Step 1: Build the firmware
```bash
cd C:\Projects\CYGM
idf.py build
```

### Step 2: Create a merged binary
```bash
cd build
esptool --chip esp32 merge_bin ^
  -o cygm-merged.bin ^
  --flash_mode dio --flash_freq 40m --flash_size 4MB ^
  0x1000 bootloader/bootloader.bin ^
  0x8000 partition_table/partition-table.bin ^
  0x10000 cygm.bin ^
  0x310000 spiffs.bin
```

### Step 3: Create the version folder
```bash
mkdir website\firmware\vX.Y.Z
copy build\cygm-merged.bin website\firmware\vX.Y.Z\
copy build\cygm.bin website\firmware\vX.Y.Z\
copy build\bootloader\bootloader.bin website\firmware\vX.Y.Z\
copy build\partition_table\partition-table.bin website\firmware\vX.Y.Z\
copy build\spiffs.bin website\firmware\vX.Y.Z\
```

### Step 4: Update releases.json

Add the new version to the `versions` array. Move `"latest": true` to the new entry
and remove it from the old one:

```json
{
  "versions": [
    {
      "version": "1.6.0",
      "date": "2026-02-15",
      "stage": "Beta",
      "notes": "Nightscout API support, historical glucose graph",
      "files": {
        "merged": "firmware/v1.6.0/cygm-merged.bin",
        "app": "firmware/v1.6.0/cygm.bin",
        "bootloader": "firmware/v1.6.0/bootloader.bin",
        "partitions": "firmware/v1.6.0/partition-table.bin",
        "spiffs": "firmware/v1.6.0/spiffs.bin"
      },
      "latest": true
    },
    {
      "version": "1.5.1",
      "date": "2026-02-05",
      "stage": "Beta",
      "notes": "Tick overflow fix, battery monitoring improvements",
      "files": {
        "merged": "firmware/v1.5.1/cygm-merged.bin"
      }
    }
  ]
}
```

**Important**: Newest versions go at the top of the array.

### Step 5: Commit and push
```bash
git add website/
git commit -m "Add firmware v1.6.0"
git push
```

## Hosting on GitHub Pages

1. Push the `website/` folder to your GitHub repo
2. Go to **Settings → Pages**
3. Set source to the branch/folder containing the website
4. Your flasher will be live at `https://yourusername.github.io/CYGM/`

## Merged Binary vs Individual Files

**Merged binary (recommended)**: Single file at offset 0. Simplest approach, includes
everything. Created with `esptool merge_bin`.

**Individual files**: If you don't provide a `merged` key in releases.json, the flasher
falls back to flashing each file individually using the offsets defined in the manifest.
This works but requires all four files to be present.

## Browser Compatibility

| Browser | Support |
|---------|---------|
| Chrome (desktop) | ✅ Full support |
| Edge (desktop) | ✅ Full support |
| Chrome (Android) | ❌ No Web Serial |
| Safari | ❌ No Web Serial |
| Firefox | ❌ No Web Serial |

## Security Notes

- Firmware files are served over HTTPS (GitHub Pages)
- Web Serial requires user interaction (port selection dialog)
- No data is sent to any server — everything runs client-side
- Users can inspect the .bin files before flashing
