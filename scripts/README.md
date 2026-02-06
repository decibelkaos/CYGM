# CYGM Screenshot Server

## Quick Start

1. **Run the screenshot server on your computer:**
   ```bash
   python screenshot_server.py
   ```

2. **Note the IP address** shown in the GUI (e.g., `192.168.1.100`)

3. **Update ESP32 configuration:**
   - Open `main/web_server.c`
   - Find the line `#define SCREENSHOT_SERVER_IP "192.168.1.100"`
   - Change it to your computer's IP address shown in the GUI
   - Rebuild and flash the ESP32

4. **Take screenshots:**
   - On your ESP32's web interface, navigate to `/api/ui/screenshot`
   - Or use: `curl http://YOUR_ESP32_IP/api/ui/screenshot`
   - Screenshots will automatically appear in the `screenshots/` folder

## Features

- **Auto-detect computer IP** - No manual configuration needed
- **GUI interface** - Easy to use, shows real-time activity
- **Timestamped files** - Screenshots saved as `screenshot_YYYYMMDD_HHMMSS.bmp`
- **Auto-create folder** - Screenshots directory created automatically
- **One-click folder access** - Open screenshots folder from GUI

## Requirements

- Python 3.6 or higher (tkinter included with Python)
- ESP32 and computer on same WiFi network

## Troubleshooting

**Server won't start:**
- Check if port 8080 is already in use
- Try running as administrator/sudo

**ESP32 can't connect:**
- Verify IP address in `web_server.c` matches the GUI
- Check firewall isn't blocking port 8080
- Ensure both devices are on same network

**No screenshots received:**
- Check ESP32 serial output for errors
- Verify web server is enabled on ESP32 (`ENABLE_WEB_SERVER 1`)
