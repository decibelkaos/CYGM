# Connecting a CGM

CYGM gets readings from a **cloud account or Nightscout site**. It doesn't connect directly to the glucose sensor.

Choose one source during setup:

| Source | Use this when |
| --- | --- |
| Dexcom Share | Your glucose readings are shared through Dexcom's service |
| LibreLinkUp | You use Abbott's LibreLinkUp sharing service |
| Nightscout | You already have a working Nightscout installation |

## Dexcom Share

Use the account details required for Dexcom sharing. Make sure sharing works in the official Dexcom setup first. If CYGM doesn't show a reading immediately, check Wi-Fi, account access, and whether a new measurement has reached Dexcom's cloud.

## LibreLinkUp

Make sure the account works in LibreLinkUp itself before troubleshooting CYGM. Regional login behavior matters. A UK sign-in problem was reported and subsequently confirmed fixed by the original reporter in [issue #2](https://github.com/decibelkaos/CYGM/issues/2).

## Nightscout

You'll need the URL of your working Nightscout instance and any required token. Verify the site is reachable before entering it on CYGM.

A previous bug caused some Nightscout configurations to appear lost after a reboot. That was corrected in **v0.16.0**; see [issue #1](https://github.com/decibelkaos/CYGM/issues/1). If you're running an older version, update first.

## When readings don't match

- Check the timestamp of the **measurement**, not just the time CYGM last contacted the service.
- Verify you're signed into the expected account and viewing the intended person.
- Remember that cloud delivery can lag the sensor.
- Confirm the current reading and alarms in the official CGM app or receiver.

**Don't post account credentials, tokens, or private glucose history in GitHub issues.**
