# Hardware

## The board

CYGM is built for the **JC2432W328C**, the **capacitive-touch** version of the ESP32 display board.

| Part | What it uses |
| --- | --- |
| Microcontroller | ESP32-D0WD |
| Screen | 2.8-inch, 320 × 240 ST7789 |
| Touch | CST820 capacitive touch |
| Wireless | 2.4 GHz Wi-Fi |
| Sound | 8002A amplifier; add a speaker for louder alarms |
| Other | RGB status LED, microSD slot, optional LiPo battery |

**Check the C in the board model before buying.** There is a nearly identical resistive-touch version. It may look right in a listing, but the touch interface isn't compatible with this firmware.

CYGM can run plugged in without a battery. A battery is optional, not a requirement.

## Case

A printable case is available on [MakerWorld](https://makerworld.com/en/models/2542672-cygm-case).

## Before powering up

Use a suitable USB power source and cable for your board. Check the board and battery for physical damage. If you're adding a LiPo battery, verify polarity and connector compatibility rather than assuming two connectors with the same shape are wired the same way.

The simplest first test is with the bare, unmodified board. Get the touchscreen and Wi-Fi setup working before putting everything in a case.
