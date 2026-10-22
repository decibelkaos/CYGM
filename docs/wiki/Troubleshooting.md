# Troubleshooting

Start with the simple things. Most first-boot problems are board selection, USB cable, Wi-Fi, or account setup.

| Symptom | What to check |
| --- | --- |
| Browser flasher can't see the board | Use a USB data cable; try another port and a compatible browser. |
| Screen works but touch doesn't | Verify the board is the **JC2432W328C capacitive** version. |
| No glucose readings | Check Wi-Fi, selected provider, credentials, and whether the provider's own app/site has current data. |
| Readings seem old | Compare **measurement timestamps**. A successful cloud fetch doesn't necessarily bring a new measurement. |
| LibreLinkUp sign-in trouble | Confirm LibreLinkUp itself works and update CYGM; the UK issue was confirmed fixed in [#2](https://github.com/decibelkaos/CYGM/issues/2). |
| Nightscout setup seems lost after power failure | Upgrade to at least v0.16.0; background in [#1](https://github.com/decibelkaos/CYGM/issues/1). |
| Screen dims while on USB power | See [#3](https://github.com/decibelkaos/CYGM/issues/3); a proposed fix is being tested in [PR #4](https://github.com/decibelkaos/CYGM/pull/4). |
| Reading age looks wrong on duplicate fetches | See [#5](https://github.com/decibelkaos/CYGM/issues/5); investigation and testing are ongoing. |

## Asking for help

Before opening an [issue](https://github.com/decibelkaos/CYGM/issues), note:

- CYGM firmware version.
- Board version and whether a battery is installed.
- Your selected provider.
- Exactly what happened and how to reproduce it.
- What you expected instead.

A photo of the screen can help, but remove identifiable information. Don't share your passwords, Nightscout tokens, or personal health information.

If CYGM shows an unexpected reading, use your official CGM app or receiver to check it before acting.
