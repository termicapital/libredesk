# Libre Desk firmware

ESP-IDF firmware for the **Waveshare ESP32-C6-LCD-1.3** with its 240×240 ST7789 panel. The current vertical slice has been physically accepted with successive real readings through a certificate-verified HTTPS bridge.

The official Libre app/reader remains authoritative. This firmware has no medical alarm or treatment function.

## Pinned target and toolchain

- ESP-IDF `v5.5.5`
- target `esp32c6`
- exact board pin map: `docs/decisions/0003-waveshare-esp32-c6-lcd-1.3.md`
- pinned `esp_lvgl_port` and LVGL through `firmware/main/idf_component.yml`
- application version: `firmware/version.txt`

Install the toolchain from the repository root:

```sh
./scripts/setup-idf.sh
```

The scripts default to a user-local Linux checkout but accept documented path overrides. They reject an unexpected or dirty ESP-IDF tree.

## Current boot and runtime flow

1. Initialize NVS, the accepted display port, LVGL, and the sole UI owner task.
2. Keep the active-high GPIO22 backlight off until panel RAM has been cleared and an actual DMA completion is observed.
3. Publish `starting`, then join the configured 2.4 GHz Wi-Fi network.
4. Publish `Time` while SNTP establishes sane wall time for TLS validation.
5. Poll `<bridge-origin>/api/v2/latest` with the device bearer token and the ESP-IDF Mozilla certificate bundle. The bridge retains `/api/v1/latest` only for already-flashed schema-1 firmware.
6. Capture headers and body in fixed-capacity storage, then obtain the final HTTP status after `esp_http_client_perform()` as required by ESP-IDF event ordering.
7. Parse only the strict schema-2 normalized JSON contract through `glucose_core`, including LibreLinkUp local measurement wall time.
8. Advance the internal local clock from monotonic reading age at 1 Hz in a runtime task independent of the blocking HTTP worker, and publish an immutable snapshot only when visible/state semantics change.
9. Publish through the one-element overwrite queue owned by the UI task; replace and rebase the display on every later valid reading without rebooting or reflashing.

A malformed, oversized, future-dated, stale, implausible, or unauthenticated response never produces a number.

Fresh and aging screens show a zero-padded 24-hour `HH:MM` clock and the dominant glucose number without a visible unit label. Seconds remain internal for rollover. Stale, setup, authentication, Wi-Fi, server-error, and other number-free states suppress the clock.

## Local MVP provisioning

Copy the non-secret template:

```sh
cp firmware/main/mvp_secrets.example.h firmware/main/mvp_secrets.h
```

Edit only the ignored `mvp_secrets.h`:

```c
#define MVP_WIFI_SSID "..."
#define MVP_WIFI_PASSWORD "..."
#define MVP_BRIDGE_ORIGIN "https://REPLACE_WITH_STABLE_HOSTNAME"
#define MVP_DEVICE_TOKEN "..."
```

Confirm the ignore rule:

```sh
git check-ignore -v firmware/main/mvp_secrets.h
```

Prototype limitation: these values are compiled into the application binary. Treat every generated `.bin` and `.elf` as private. Changing Wi-Fi, token, or hostname currently requires a rebuild and reflash. See `docs/SECOND_DEVICE.md` for safe per-device generation.

## Host tests

```sh
./scripts/test-glucose-core.sh
./scripts/test-runtime-core.sh
./scripts/test-mvp-config.sh
./scripts/test-wifi-startup-flow.sh
./scripts/test-mvp-http-flow.sh
python3 firmware/test/test_runtime_foundation.py -v
python3 firmware/main/test_physical_calibration.py -v
```

The HTTP-flow regression includes the real ESP-IDF callback order—headers and body may arrive before the final status is available—and verifies that late error/304 status assignment still fails closed.

## Clean build and package

```sh
./scripts/build-display-spike.sh
```

Despite its historical name, this is the canonical MVP firmware build. It:

- validates the local provisioning boundary;
- runs required host tests;
- regenerates ignored `sdkconfig` from defaults;
- full-cleans the ESP-IDF build;
- forces the exact target/toolchain; and
- packages ESP Web Tools artifacts using generated `flasher_args.json` offsets.

Generated outputs remain ignored:

- `firmware/build/`
- `tools/flasher/firmware/*.bin`
- `firmware-releases/*.bin` and `*.elf`

Package tests:

```sh
python3 tools/flasher/test_package_firmware.py -v
python3 tools/flasher/test_static_site.py -v
python3 tools/flasher/test_serve_private.py -v
```

See `tools/flasher/README.md` for browser installation.

## Accepted display behavior

The panel path:

1. drives microSD CS/GPIO4 high before shared SPI traffic;
2. initializes SPI2 mode 0 at a conservative 12 MHz;
3. uses RGB element order, little-endian RGB565 input, inversion, zero gaps, and no hardware XY swap/mirroring;
4. rotates partial LVGL draw buffers and flush coordinates 90 degrees in software; and
5. caps MVP backlight brightness at 25%.

The finished-product pose has USB-C exiting from the **right**.

The asymmetric physical orientation card was accepted on 2026-08-23:

- red top-left;
- green top-right;
- blue bottom-left;
- white bottom-right; and
- upright, non-mirrored yellow digit `1` in the centre.

Hardware `swap_xy` must remain disabled on this partial-buffer path; physical testing showed wrapped/clumped dirty rectangles. Any orientation change must rerun the deterministic calibration test, perform two clean builds, and repeat the physical card plus cold-boot acceptance.

## Physical live-data acceptance

The MVP was accepted after the exact board:

- joined Wi-Fi and synchronized time after cold boot;
- reached the authenticated HTTPS bridge;
- rendered a real normalized reading with unit, trend, age, and signal state; and
- replaced it with a later real reading without reflashing.

No real value is stored in repository evidence.

## Known firmware limitation

A single transport failure currently maps immediately to a temporary `SRV` screen and the next successful poll restores the reading. Post-MVP hardening should retain and age the last valid reading through isolated failures while still removing it at the strict freshness boundary.
