# Installation

You don't need to compile firmware just to try CYGM.

## Install from the browser

1. Connect the **JC2432W328C capacitive-touch** board to your computer with a USB data cable.
2. Open [cygm.me](https://cygm.me) and use the browser firmware flasher.
3. Follow the flasher prompts and allow the board to restart.
4. Complete the on-device setup, including Wi-Fi and your preferred glucose provider.

If the browser doesn't see the board, try a known-good **data** cable and another USB port. Some cables only carry power.

## After the first boot

Set the correct time zone and glucose units, then check that the incoming values match your official CGM app. Set up alarms and brightness the way you want them.

CYGM polls its selected cloud service about every 90 seconds. That doesn't mean your sensor publishes a new glucose measurement every 90 seconds. Look at the reading timestamp rather than expecting every fetch to change the value.

## Updating later

CYGM supports firmware updates over Wi-Fi. On the device, open **Settings → About → Check for Updates**.

Read the release notes before installing a beta. If a change affects charging, dimming, readings, or alarms, verify it on your own setup after updating.

## Manual installation

If you're developing the firmware or the browser installer isn't appropriate, use the [source build instructions](Building-from-Source.md).
