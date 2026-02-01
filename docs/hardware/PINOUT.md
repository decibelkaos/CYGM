# Hardware Pin Configuration
# JC2432W328 (ESP32-D0WD) + M5Stack CardKB
# VERIFIED 2025-01-16

## Display (ST7789 SPI)
LCD_MOSI = 13
LCD_SCLK = 14
LCD_CS = 15
LCD_DC = 2
LCD_BL = 27
LCD_RST = -1  # Not connected
LCD_WIDTH = 320
LCD_HEIGHT = 240

## Touch (CST820 I2C)
TOUCH_SDA = 33
TOUCH_SCL = 32
TOUCH_RST = 25
TOUCH_INT = 21
TOUCH_ADDR = 0x15
TOUCH_SWAP_XY = 1
TOUCH_MIRROR_X = 0
TOUCH_MIRROR_Y = 1
# Uses I2C_NUM_0

## Speaker (PWM -> 8002A amplifier -> P4 connector)
SPEAKER_PIN = 26
# Note: LEDC warning is harmless, speaker works fine

## RGB LED (Active Low - set 0 for ON, 1 for OFF)
LED_RED = 4
LED_GREEN = 16
LED_BLUE = 17

## Button
BOOT_BUTTON = 0  # Only usable button (GPIO 0)
# SW1 = RST/EN (hardware reset, not a GPIO)

## CardKB Keyboard (I2C @ 0x5F)
CARDKB_SDA = 21
CARDKB_SCL = 22
CARDKB_ADDR = 0x5F
# Uses I2C_NUM_1 (separate from touch)

## SD Card (if used)
SD_CS = 5
SD_MOSI = 23
SD_MISO = 19
SD_SCK = 18
