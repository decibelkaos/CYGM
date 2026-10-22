# CYGM Wiki

CYGM is a small, open-source display for glucose readings shared by Dexcom, LibreLinkUp, or Nightscout. It uses an inexpensive ESP32 touchscreen board and can sit on a desk or beside the bed.

This wiki covers the practical stuff: choosing the right board, getting firmware onto it, connecting a glucose source, and dealing with the common problems.

## Start here

- [Hardware](Hardware.md): which board to buy and what to check before ordering.
- [Installation](Installation.md): flashing the firmware and first startup.
- [Connecting a CGM](Connecting-a-CGM.md): Dexcom Share, LibreLinkUp, and Nightscout.
- [Daily use](Daily-Use.md): readings, alarms, charts, dimming, and updates.
- [Troubleshooting](Troubleshooting.md): problems we've seen and where to start.
- [Building from source](Building-from-Source.md): ESP-IDF setup and the code layout.
- [Helping out](Contributing.md): bug reports, code submissions, and contributor credit.

For the most current screen-by-screen walkthrough, see the [device guide](https://cygm.me/guide.html). [Firmware releases](https://github.com/decibelkaos/CYGM/releases) document changes between versions.

> **Important:** CYGM is an experimental secondary display, not a medical device. Keep your official CGM app or receiver and its alerts enabled. Confirm readings there before making treatment decisions.
