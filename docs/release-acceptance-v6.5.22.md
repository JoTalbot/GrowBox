# GrowBox v6.5.22 Release Acceptance

## Release state

- Firmware version: v6.5.22
- Main branch: main
- OTA manifest: firmware/remote/version.json
- GitHub Actions build: passed for commit b34ccb6
- Release assets: main, dry, factory-4MB, OTA-Service

## Acceptance checklist

- [x] Firmware compiles with PlatformIO
- [x] v6.5.22 firmware binary published
- [x] OTA manifest points to v6.5.22
- [x] Telegram watering controls use inline Status buttons
- [x] Android/iPhone phone UI contains responsive mobile layout
- [x] Mobile UI has polling fallback and SSE acceleration
- [x] README version and release notes synchronized
- [ ] Physical ESP32 OTA upgrade verified on hardware
- [ ] Physical Android acceptance verified against the deployed controller

## Known historical issue

Issue #1 reported the v6.5.20 `Verify Bin Header Failed` failure. The OTA implementation was subsequently changed to validate the downloaded binary and use the Arduino ESP32 HTTPClient/Update APIs in v6.5.21/v6.5.22.

Physical acceptance is the final gate before treating the OTA path as fully production-proven.
