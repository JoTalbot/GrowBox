# GrowBox v6.5.28 Release Acceptance

## Release identity

- Release: [v6.5.28](https://github.com/JoTalbot/GrowBox/releases/tag/v6.5.28)
- Source branch: `main`
- Firmware version source: `firmware/remote/release-version.json`
- OTA manifest: `firmware/remote/version.json`
- Build workflow: [Build firmware](https://github.com/JoTalbot/GrowBox/actions/workflows/build-firmware.yml)

## Automated acceptance

- [x] Release metadata identifies 6.5.28.
- [x] Normal and dry firmware version macros match release metadata.
- [x] OTA manifest points to the v6.5.28 main firmware asset.
- [x] GitHub Release contains main, dry, factory-4MB, and OTA-Service binaries.
- [x] Build firmware workflow on main completed successfully for v6.5.28.
- [x] GitHub Pages deployment completed successfully after release publication.
- [ ] Re-run the full firmware build and static checks against the proposed next release commit before publishing any new release.

## Hardware acceptance — required before claiming production verification

- [ ] Confirm GPIO16 legacy light relay stays energized and GPIO19 controls the managed light relay.
- [ ] Verify emergency light shutdown at the configured over-temperature threshold.
- [ ] Verify heater fail-safe behavior when DHT22 data is missing or invalid, including manual override.
- [ ] Verify tank float and leak inputs with sensors physically connected and enabled.
- [ ] Verify each watering zone, soak delay, and low-soil-sensor fallback under supervision.
- [ ] Verify OTA upgrade on the actual ESP32, including interrupted/invalid download handling and successful reboot.
- [ ] Verify Telegram status, inline watering/relay controls, and state refresh after each action.
- [ ] Verify mobile dashboard on a physical Android/iPhone device.

## Known limitations

- Hardware acceptance above cannot be proven by GitHub Actions; it requires the actual controller and sensors.
- A historical failed run on feature branch `c65eaa9` stopped at the static checker with `release metadata version mismatch`; the main-branch v6.5.28 release build subsequently succeeded. Do not treat the historical feature-branch failure as a current main-branch failure.
- Do not publish a new firmware version solely to change documentation. A new release must include a validated firmware change and a passing build.
