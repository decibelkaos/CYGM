# CYGM Screenshot Tools

## Serial Screenshot Tool (WORKING! ✅)

The **serial_screenshot.py** tool captures screenshots from your ESP32 via the serial port - no extra memory needed on the device!

### Quick Start

1. **Install Python dependencies:**
   ```bash
   pip install pyserial pillow
   ```

2. **Run the screenshot tool:**
   ```bash
   python serial_screenshot.py
   ```

3. **In the GUI:**
   - Select your ESP32's COM port (auto-detected)
   - Click "Connect"
   - Click "📷 Take Screenshot"
   - Screenshots are automatically saved to `screenshots/` folder

### How It Works

- ESP32 streams raw RGB565 pixel data over serial
- Python converts it to PNG on your computer
- Uses almost ZERO extra RAM on ESP32
- Works with your current hardware!

### Commands Available on ESP32 Serial Monitor

You can also type these commands in your serial monitor:
- `screenshot` - Capture and send screenshot
- `status` - Show system status
- `help` - List available commands

---

## HTTP Screenshot Server (Disabled)

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
