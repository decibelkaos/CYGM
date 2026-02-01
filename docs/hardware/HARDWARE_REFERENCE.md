# JC2432W328 Complete Hardware Reference
## ESP32-D0WD Development Board with 2.8" TFT Display
### Extracted from Manufacturer Demo Code - 2026-01-16

---

## HARDWARE OVERVIEW

- **MCU**: ESP32-WROOM-32 (ESP32-D0WD dual-core)
- **Flash**: 4MB
- **Display**: 2.8" TFT 320x240 (ST7789V3)
- **Touch**: Capacitive (CST820)
- **Audio**: 8002A amplifier with JST connector (P4)
- **LED**: RGB (active-low GPIO control)
- **SD Card**: SPI interface
- **Connectivity**: WiFi 802.11 b/g/n, Bluetooth 4.2 BLE

---

## PIN CONFIGURATION (VERIFIED)

### Display - ST7789V3 (SPI)
| Function | GPIO | Notes |
|----------|------|-------|
| MOSI | 13 | SPI Data |
| SCLK | 14 | SPI Clock |
| CS | 15 | Chip Select |
| DC | 2 | Data/Command |
| BL | 27 | Backlight (PWM capable) |
| RST | -1 | Not connected (use SW reset) |

**SPI Settings:**
- Frequency: 55MHz (tested working)
- Color: RGB565, BGR order
- DMA: Supported (ESP32_DMA)

### Touch - CST820 (I2C)
| Function | GPIO | Notes |
|----------|------|-------|
| SDA | 33 | I2C Data |
| SCL | 32 | I2C Clock |
| RST | 25 | Reset |
| INT | 21 | Interrupt |

**I2C Settings:**
- Address: 0x15
- Speed: 400kHz (standard)

**Calibration (for 320x240 portrait):**
- SWAP_XY = 1
- MIRROR_X = 0  
- MIRROR_Y = 1

**Gesture Support:**
- None (0x00)
- SlideDown (0x01)
- SlideUp (0x02)
- SlideLeft (0x03)
- SlideRight (0x04)
- SingleTap (0x05)
- DoubleTap (0x0B)
- LongPress (0x0C)

### RGB LED (Active Low)
| Color | GPIO | ON State | OFF State |
|-------|------|----------|-----------|
| RED | 4 | LOW (0) | HIGH (1) |
| GREEN | 16 | LOW (0) | HIGH (1) |
| BLUE | 17 | LOW (0) | HIGH (1) |

**PWM Fade Example:**
```c
ledcSetup(0, 2000, 8);  // Channel 0, 2kHz, 8-bit
ledcAttachPin(4, 0);    // Attach RED LED
ledcWrite(0, 128);      // 50% brightness
```

### Speaker/Buzzer
| Function | GPIO | Notes |
|----------|------|-------|
| BUZZER | 26 | Via 8002A amplifier |

**Output:** P4 JST 1.25mm 2-pin connector

**Tone Generation (ESP-IDF):**
```c
ledc_timer_config_t timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_10_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 1000,  // Frequency
    .clk_cfg = LEDC_AUTO_CLK
};
```

### SD Card (SPI)
| Function | GPIO | Notes |
|----------|------|-------|
| CS | 5 | Chip Select |
| MOSI | 23 | Data Out |
| MISO | 19 | Data In |
| SCK | 18 | Clock |

### Buttons
| Button | GPIO/Pin | Function |
|--------|----------|----------|
| BOOT | GPIO 0 | User button (active low, INPUT_PULLUP) |
| SW1 | RST/EN | Hardware reset ONLY (not a GPIO!) |

**Note:** Only ONE user-accessible button available!

---

## WIFI EXAMPLES

### Station Mode (Connect to Router)
```c
#include <WiFi.h>

WiFi.mode(WIFI_STA);
WiFi.setSleep(false);  // Disable power save for better response
WiFi.begin("SSID", "password");
WiFi.setAutoReconnect(true);

while (WiFi.status() != WL_CONNECTED) {
    delay(500);
}
Serial.println(WiFi.localIP());
```

### Access Point Mode
```c
WiFi.softAP("ESP_AP", "12345678");
Serial.println(WiFi.softAPIP());
```

### SmartConfig (Phone App Configuration)
```c
WiFi.mode(WIFI_MODE_STA);
WiFi.beginSmartConfig();
while (!WiFi.smartConfigDone()) {
    delay(500);
}
// SSID: WiFi.SSID()
// Password: WiFi.psk()
```

### TCP Server
```c
WiFiServer server;
server.begin(10000);  // Port 10000

WiFiClient client = server.available();
if (client && client.connected()) {
    char c = client.read();
    client.print("Response");
}
```

### HTTP Web Server
```c
WiFiServer server(80);
server.begin();

WiFiClient client = server.available();
// Parse HTTP request
// Send HTML response
client.println("HTTP/1.1 200 OK");
client.println("Content-type:text/html");
```

---

## BLUETOOTH LOW ENERGY (BLE)

```c
#include <BLEDevice.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEDevice::init("ESP32-BLE");
BLEServer *pServer = BLEDevice::createServer();
BLEService *pService = pServer->createService(SERVICE_UUID);
BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
);
pCharacteristic->setValue("Hello World");
pService->start();
BLEDevice::startAdvertising();
```

---

## LVGL INTEGRATION

### Display Driver Setup (Arduino)
```c
#include <lvgl.h>
#include <TFT_eSPI.h>

static const uint16_t screenWidth = 240;
static const uint16_t screenHeight = 320;

TFT_eSPI tft = TFT_eSPI();
static lv_color_t *buf1, *buf2;

// DMA buffer allocation
buf1 = (lv_color_t*)heap_caps_malloc(
    sizeof(lv_color_t) * screenWidth * 200,
    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
);
buf2 = (lv_color_t*)heap_caps_malloc(
    sizeof(lv_color_t) * screenWidth * 200,
    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
);

lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 200);

// Flush callback with DMA
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.pushImageDMA(area->x1, area->y1, w, h, (uint16_t*)color_p);
    lv_disp_flush_ready(disp);
}
```

### Touch Input Driver
```c
#include "CST820.h"
CST820 touch(33, 32, 25, 21);  // SDA, SCL, RST, INT

void my_touchpad_read(lv_indev_drv_t *indev, lv_indev_data_t *data) {
    uint16_t touchX, touchY;
    uint8_t gesture;
    
    if (touch.getTouch(&touchX, &touchY, &gesture)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}
```

### Working LVGL Demos
- lv_demo_widgets() - ✅ Working
- lv_demo_benchmark() - ✅ Working
- lv_demo_stress() - ✅ Working
- lv_demo_keypad_encoder() - Works (needs encoder)
- lv_demo_music() - ❌ Not working

---

## TFT_eSPI CONFIGURATION

Key settings from User_Setup.h:
```c
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_INVERSION_OFF

#define ESP32_DMA
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   27

#define SPI_FREQUENCY  55000000
#define SPI_READ_FREQUENCY  20000000
```

---

## ESP-IDF NOTES

### Component Dependencies
```yaml
dependencies:
  esp_lcd:
  driver:
  esp_timer:
```

### LEDC for Audio (v5.x)
```c
#include "driver/ledc.h"

ledc_timer_config_t timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_10_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = frequency,
    .clk_cfg = LEDC_AUTO_CLK
};

ledc_channel_config_t channel = {
    .gpio_num = 26,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 512,  // 50% duty
    .hpoint = 0
};
```

### GPIO for LEDs
```c
gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << 4) | (1ULL << 16) | (1ULL << 17),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
};
gpio_config(&io_conf);

// Active low: 0 = ON, 1 = OFF
gpio_set_level(4, 0);   // RED ON
gpio_set_level(16, 1);  // GREEN OFF
gpio_set_level(17, 1);  // BLUE OFF
```


---

## M5STACK CARDKB KEYBOARD

### Connection
| CardKB | ESP32 GPIO | Notes |
|--------|------------|-------|
| SCL | 22 | I2C Clock |
| SDA | 21 | I2C Data |
| 5V | 5V | Power |
| GND | GND | Ground |

**I2C Settings:**
- Address: 0x5F
- Speed: 100kHz
- Port: I2C_NUM_1 (separate from touch on I2C_NUM_0)

### Key Values
| Key Combo | Output |
|-----------|--------|
| Single press | Lowercase letter |
| Shift + key | Uppercase letter |
| Sym + key | Symbol (second value) |
| Double-tap Shift | Lock uppercase |
| Double-tap Sym | Lock symbols |

### Special Keys
| Key | Value |
|-----|-------|
| Enter | 0x0D |
| Backspace | 0x08 |
| ESC | 0x1B |
| Tab | 0x09 |
| Space | 0x20 |

### ESP-IDF Code
```c
#define CARDKB_ADDR  0x5F
#define I2C_PORT     I2C_NUM_1

// Read single key
uint8_t cardkb_read(void) {
    uint8_t data = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CARDKB_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return data;
}
```
---

## DEMO FILES INVENTORY

| Folder | Description |
|--------|-------------|
| 1_2_Factory_samples_Capacitive_touch | Full LVGL demo with touch |
| 3_3_LED | RGB LED control and PWM fade |
| 3_5_key | Button input (BOOT only) |
| 3_8_pwm | PWM backlight control |
| 4_1_wifi_AP | WiFi Access Point mode |
| 4_2_wifi_STA | WiFi Station mode |
| 4_3_wifi_SmartConfig | Phone app WiFi config |
| 4_4_wifi_STA_TCP_Server | TCP server example |
| 4_5_WIFI_STA_TCP_Client | TCP client example |
| 4_7_WIFI Web Servers LED | HTTP web server |
| 5_1_bleService | BLE peripheral example |
| 6_1_SD_Jpg | SD card JPEG display |
| 8_1_Buzzer_PiratesOfTheCaribian | Speaker/buzzer tones |

---

## KNOWN ISSUES & GOTCHAS

1. **SW1 is NOT a GPIO** - It's connected to RST/EN for hardware reset only
2. **RGB LEDs are active-low** - Write 0 to turn ON, 1 to turn OFF
3. **Speaker uses PWM** - Not DAC; use LEDC timer for tone generation
4. **Touch calibration required** - SWAP_XY and MIRROR_Y needed for correct orientation
5. **ESP-IDF v5.x changes** - Legacy DAC API deprecated, use LEDC instead
6. **SD card shares SPI pins** - Different CS pin (GPIO 5) from display

---

## BUILD COMMANDS

### ESP-IDF
```bash
cd C:\Projects\CYGM
idf.py build
idf.py flash monitor -p COM40
```

### Arduino IDE Settings
- Board: ESP32 Dev Module
- Flash Size: 4MB
- Partition Scheme: Default
- Upload Speed: 921600

---

*Generated from JC2432W328_DEMO source files*
*Last verified: 2026-01-16*

