# Contributing to CYGM

Thanks for helping make CYGM better. Bug reports, reproducible test results, documentation, translations, and code contributions are welcome.

CYGM is a **secondary, non-medical CGM display**. Never use CYGM as the sole source for diabetes treatment decisions or alerts; continue using your official CGM app or receiver. Please treat glucose accuracy, timestamps, credential handling, and alarms as safety-sensitive.

## Before you start

1. Search [existing issues](https://github.com/decibelkaos/CYGM/issues) and [pull requests](https://github.com/decibelkaos/CYGM/pulls) for similar work.
2. For significant changes, open an issue first and describe your approach.
3. Keep pull requests focused. Link the relevant issue with `Refs #123` (only use `Fixes #123` when the change is fully tested and ready to resolve it).
4. Do not post CGM credentials, API tokens, real patient readings, Wi-Fi passwords, or personal health information in issues, logs, screenshots, or commits.

## Local development

The repository uses **ESP-IDF v5.5** and **LVGL v8.4**. The reference board is the **JC2432W328C capacitive-touch** variant.

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

See [README.md](README.md) for board identification, project structure, and build notes. Changes to `CONFIG_MBEDTLS_*` may require `idf.py fullclean && idf.py build`. Avoid unrelated formatting, generated build files, and secrets.

## Submitting a pull request

- Fork the repository, create a descriptive branch, and target `main` unless the maintainer asks for another base.
- Describe **what changed**, **why**, and **how you verified it**.
- Include relevant device variant, battery/USB power state, CGM provider, and firmware version.
- For UI changes, include screenshots without personal glucose details.
- Note tests you *didn't* run and potential regressions. Do not claim hardware verification if you only built the code.
- The maintainer will review and decide when a fix enters beta and stable firmware. A PR being open or mergeable does not imply it has been accepted or tested.

## Testing guidance

At minimum, run a clean build for source changes and describe manual hardware tests when available. For behavior involving power, test USB with battery charging, USB with battery full, USB without a battery (when supported), and battery-only operation as applicable.

For glucose readings, preserve the original measurement timestamp, distinguish a repeated fetch from a newly measured value, and verify stale-data/age behavior. For alarm changes, check out-of-range conditions, missing data, and restart behavior without relying on CYGM as your only alert.

## Reporting bugs

Create an issue with: firmware version, provider, reproducible steps, expected behavior, actual behavior, and a sanitized log if useful. Please avoid posting live access tokens or identifiable medical data.

## Credit

We credit verified community improvements in release notes and recognize contributors in [CONTRIBUTORS.md](CONTRIBUTORS.md). Thank you for helping other builders and families use CYGM.
