# Hardware Pin Configuration
# JC2432W328 (ESP32-D0WD)
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

## Speaker (PWM -> 8002A amplifier -> P4 connector)
SPEAKER_PIN = 26

## RGB LED (Active Low - set 0 for ON, 1 for OFF)
LED_RED = 4
LED_GREEN = 16
LED_BLUE = 17

## Button
BOOT_BUTTON = 0  # Only usable button (GPIO 0)
# SW1 = RST/EN (hardware reset, not a GPIO)

## Unused/Available GPIOs
# Most GPIOs are used by display, touch, SD card
# Check schematic before using any "free" pins
