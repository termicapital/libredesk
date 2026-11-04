# Libre Desk development guide

This repository contains two cooperating systems:

1. a Node.js bridge that authenticates to a dedicated LibreLinkUp follower account, validates one current reading, and exposes a small device API; and
2. ESP-IDF firmware for the Waveshare ESP32-C6-LCD-1.3, which retrieves that normalized response and renders it through the bounded C runtime.

The official Libre app/reader remains authoritative. Libre Desk is a convenience display, not an alarm or treatment device.

## Supported baseline

- Linux development host
- Node.js 22 LTS and npm
- Python 3
- GCC, CMake, Make, and Git
- ESP-IDF **v5.5.5** for ESP32-C6
- Exact target board: **Waveshare ESP32-C6-LCD-1.3**, 240×240 ST7789
- Desktop Chrome or Edge for browser flashing over WebSerial

The firmware scripts reject an unexpected or dirty ESP-IDF checkout. The board pin map and accepted orientation are frozen in `docs/decisions/0003-waveshare-esp32-c6-lcd-1.3.md`.

## Clone and install

```sh
git clone https://github.com/termicapital/libredesk.git
cd libredesk
npm ci
./scripts/install-hooks.sh
```

The pre-commit hook requires `gitleaks`. If it is not installed, install it before committing rather than bypassing the hook.

## Architecture

```text
Libre sensor/app
      |
      v
LibreLinkUp follower API
      |
      v
trusted VPS bridge      strict validation, freshness, auth, memory cache
      |
      | authenticated one-minute outbound push
      v
Vercel ingest + Redis   monotonic latest-state transaction, 24-hour TTL
      |
      | stable HTTPS read API + per-device bearer token
      v
firmware/main/net_task  bounded response capture
      |
      v
glucose_core            strict JSON parser and state machine
      |
      v
runtime_core + ui_task  immutable snapshot and LVGL owner queue
      |
      v
240×240 ST7789 display
```

Important boundaries:

- Real LibreLinkUp credentials exist only in an external environment file or secure directory.
- Vercel receives no LibreLinkUp credentials and stores only SHA-256 hashes of distinct read/write bearer tokens.
- Private Redis retains only the latest normalized reading and stable identity for 24 hours.
- The current MVP firmware embeds Wi-Fi credentials, a revocable raw device token, and the HTTPS bridge origin in an ignored local header.
- Generated firmware binaries therefore contain secrets and must not be committed or published as general releases.
- Real glucose values must never enter fixtures, logs, screenshots committed to Git, or issue bodies.

## Node bridge gates

```sh
npm run typecheck
npm test
npm run coverage
npm run build
npm audit --omit=dev
npm audit
```

The bridge entry point for normal deployment is:

```sh
node dist/bridge/src/main.js
```

It reads the validated environment contract in `.env.example`. Use an external file such as `/etc/libre-desk/bridge.env`; do not copy production values into `.env.example`.

`tools/live-bridge/start.ts` is a single-host MVP convenience launcher. It reads JSON from `LIBRE_DESK_SECURE_DIR` and is not the preferred production process definition.

The stable remote device path uses authenticated outbound ingestion from that trusted host into Vercel plus private Redis. See `docs/VERCEL_BRIDGE.md` for the production variables, secure pusher file, scheduler contract, remote proof ladder, and rollback.

## Host firmware gates

These run without ESP-IDF hardware:

```sh
./scripts/test-glucose-core.sh
./scripts/test-runtime-core.sh
./scripts/test-mvp-config.sh
./scripts/test-wifi-startup-flow.sh
./scripts/test-mvp-http-flow.sh
python3 firmware/test/test_runtime_foundation.py -v
python3 firmware/main/test_physical_calibration.py -v
python3 tools/flasher/test_package_firmware.py -v
python3 tools/flasher/test_static_site.py -v
python3 tools/flasher/test_serve_private.py -v
```

`test-runtime-core.sh` executes the production, host-adapter, sanitizer, signed-char, and unsigned-char matrices. Its aggregate coverage gate is intentionally strict; treat a coverage-only failure separately from an execution failure and do not weaken the threshold to make a change pass.

## ESP-IDF setup and clean build

```sh
./scripts/setup-idf.sh
cp firmware/main/mvp_secrets.example.h firmware/main/mvp_secrets.h
# Edit only the ignored mvp_secrets.h with local values.
./scripts/build-display-spike.sh
```

`build-display-spike.sh`:

- verifies ESP-IDF v5.5.5 and its expected commit state;
- regenerates ignored SDK configuration;
- forces the ESP32-C6 target;
- runs host and contract gates;
- performs a full-clean firmware build; and
- packages browser-flasher artifacts from ESP-IDF's generated `flasher_args.json`.

The local file `firmware/main/mvp_secrets.h` is ignored. Confirm before every commit:

```sh
git check-ignore -v firmware/main/mvp_secrets.h
```

## Browser flasher

After a successful build, generated binaries live under `tools/flasher/firmware/` and remain ignored. Serve `tools/flasher/` over HTTPS; remote WebSerial is unavailable from an insecure origin.

For a capability-path private installer:

```sh
python3 tools/flasher/serve_private.py \
  --root tools/flasher \
  --device-config /absolute/private/path/device.json \
  --port 18081
```

The private JSON needs an `installer_path_token` containing 32 random bytes encoded as unpadded base64url. The server loads an immutable release snapshot, applies `Cache-Control: no-store`, and does not log requests.

See `tools/flasher/README.md` and `docs/SECOND_DEVICE.md` for packaging and flashing details.

## UI verification

```sh
make screens
```

This runs the deterministic LVGL host renderer and publishes only synthetic fixtures. Any panel-orientation change additionally requires the physical orientation card and cold-boot checks described in `firmware/README.md`.

## Development workflow

1. Create a feature branch.
2. Add or change a failing test first for behavior changes.
3. Make the minimum implementation change.
4. Run focused tests, then the relevant full matrices.
5. Run `git diff --check` and the staged gitleaks hook.
6. Independently review security, fail-closed behavior, bounded parsing, and credential boundaries.
7. Commit source and documentation only—never private headers, environment files, firmware binaries, tunnel credentials, or real readings.
8. For firmware behavior, build with ESP-IDF and repeat physical acceptance before calling it complete.

## Current MVP limitations

- Build-time Wi-Fi and token provisioning; no finished SoftAP provisioning flow.
- One dedicated follower connection is required; ambiguous multi-patient accounts fail closed.
- No medical alarms or treatment logic.
- A single transient device request currently produces a temporary `SRV` screen instead of retaining and aging the last valid number.
- The Vercel path depends on the trusted VPS sender and the external Vercel/Upstash services; the Cloudflare endpoint remains rollback infrastructure during physical acceptance.
- OTA, rollback, managed diagnostics, and long-duration soak testing remain post-MVP work.

See `PLAN.md` and `docs/M1.md` for the contract and deferred hardening work. The calibration tests use the checked-in, credential-free source snapshot under `firmware/reference/` and need no private Git history.
