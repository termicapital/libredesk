# LibreDesk

Libre Desk is a headless-first ESP32-C6 desk display backed by a private-use LibreLinkUp bridge. The official FreeStyle Libre app/reader remains the authoritative source for readings, alarms, and treatment decisions.

## Get started

This community repository contains the bridge, firmware source, deployment templates, tests, and synthetic screen previews. Run your own bridge and configure it with your own dedicated LibreLinkUp follower account, Wi-Fi network, and device tokens. No hosted bridge or shared API credentials are provided.

1. Clone `https://github.com/termicapital/libredesk.git` and install Node.js 22 LTS, npm, Python 3, GCC, CMake, Make, and Git.
2. Run `npm ci`, `npm run typecheck`, `npm test`, and `npm run build`.
3. Follow [the device setup guide](docs/SECOND_DEVICE.md) to generate private configuration, deploy your bridge, build the firmware, and flash the exact Waveshare ESP32-C6-LCD-1.3 board. The first device follows the same process.
4. Keep private files outside this checkout. Copy only the placeholder firmware header locally when the build requires it. Build your own firmware; device-specific binaries contain private configuration.

To explore without a LibreLinkUp account or hardware, run `npm run mock-upstream -- --scenario normal --port 8787` and see the [synthetic screen gallery](docs/screens/index.html). Mock readings are test data.

See [DEVELOPMENT.md](DEVELOPMENT.md) for all build gates and [SECURITY.md](SECURITY.md) for the publication boundary. The project source is available under the [MIT license](LICENSE); vendored third-party files retain their own licenses.

## Safety boundary

This is a glanceable convenience display, **not a medical device**. It has no alarm, sound, vibration, flashing alert, treatment guidance, or dosing logic. Colour is optional recall of a value already available in the official app, not a notification.

The implementation enforces one absolute invariant:

> An old, malformed, future-dated, implausible, or otherwise invalid glucose value must never remain on screen looking current.

The bridge therefore publishes one strictly validated normalized reading or an explicit error. It never invents, interpolates, repairs, or refreshes a stale value. Rejected polls do not overwrite the last valid in-memory reading, and that reading disappears from the API once it exceeds the configured maximum age. The trusted local bridge keeps readings only in memory. The Vercel path stores only the latest normalized reading in private Redis with a 24-hour TTL so serverless reads remain available.

## Implemented

- frozen OpenAPI/JSON Schema device boundary and golden fixtures;
- scenario-driven mock LibreLinkUp upstream with runtime switching;
- in-house LibreLinkUp login, regional redirect, ticket refresh, one-time 401 re-authentication, graph retrieval, timeout, and terms-of-use handling;
- strict timestamp, value, unit, target-range, freshness, trend, patient-identity, and unit-normalization rules;
- monotonic age-aware cache, single-flight polling, capped exponential backoff, and jitter;
- Fastify API with hashed bearer-token authentication, revocation, ETag/304, per-token limits, atomic telemetry throttling, HTTPS-only OTA manifest delivery, hourly coredump throttling, and three-file/2 MiB coredump retention;
- stable Vercel device API backed by Upstash Redis Free, frozen schema-1 compatibility, a clock-aware schema-2 route, separate read/write credentials, atomic timestamp-plus-stable-identity ingestion, shared ingestion rate limiting, and a silent supervised one-minute sender from the trusted VPS;
- recursive secret/health-value log redaction and aggregate-only Prometheus metrics;
- validated environment configuration and graceful runtime lifecycle;
- digest-pinned, non-root/read-only-oriented Docker and Caddy deployment definitions;
- separate-process mock/bridge smoke test;
- official Waveshare ESP32-C6-LCD-1.3 board definition in `docs/decisions/0003-waveshare-esp32-c6-lcd-1.3.md`;
- exact-board ESP-IDF v5.5.5 firmware using `esp_lcd` plus pinned `esp_lvgl_port`/LVGL, Wi-Fi/SNTP startup, certificate-verified HTTPS polling, a zero-padded `HH:MM` clock anchored to LibreLinkUp local measurement wall time, and a self-hosted ESP Web Tools flasher;
- platform-independent C99 `glucose_core` with strict bounded contract parsing, monotonic freshness, fail-closed display-state invariants, saturating backoff, native CTest, ASan/UBSan, enforced coverage, and pinned ESP32-C6 compilation;
- shared 240×240 LVGL `ui_screens` component plus a memory-only host simulator that renders and verifies 37 PNG scenarios, a contact sheet, and a self-contained review page with byte-identical consecutive output on the pinned build host.

The source supports a local bridge, a Cloudflare tunnel, and an optional authenticated Vercel/Redis relay. Each user must deploy and validate their own installation. Build-time device provisioning remains a prototype constraint.

## Reproduce or extend it

- `DEVELOPMENT.md` — workstation setup, architecture, tests, build gates, and contribution workflow.
- `docs/SECOND_DEVICE.md` — provision, build, flash, and verify another ESP32-C6 display without copying credentials between devices.
- `docs/PERMANENT_TUNNEL.md` — replace the temporary Quick Tunnel with a named Cloudflare tunnel, stable hostname, and restart-on-boot services.
- `docs/VERCEL_BRIDGE.md` — durable VPS-to-Vercel ingestion, security boundaries, scheduler setup, verification, and rollback.
- `firmware/README.md` — exact board, private build configuration, firmware tests, and packaging.

Generated firmware binaries, Wi-Fi credentials, LibreLinkUp credentials, raw device tokens, tunnel credentials, and real glucose values are deliberately absent from Git.

## Development gate

Requirements: Node.js 22 LTS and npm.

```sh
npm ci
npm run typecheck
npm test
npm run coverage
npm run build
npm audit --omit=dev
npm audit
```

The platform-independent firmware-domain gate is:

```sh
scripts/test-glucose-core.sh
```

It runs strict native C99 tests, ASan/UBSan, and enforced state-machine coverage. The exact ESP32-C6 toolchain compile remains part of `scripts/build-display-spike.sh`.

Regenerate and verify every UI state with the exact managed LVGL 9.5.0 source:

```sh
make screens
```

The command verifies the managed LVGL lock/version/component hash and a canonical digest of every source-tree path, file byte, and symlink target. It runs native model-invariant tests, renders twice to prove byte-identical output on the current pinned host, validates an exact 40-file artifact set across 37 scenarios, checks independent decoded-RGB hashes for all 12 no-number states, exact RGB565 range colors, every trend mask and orientation, visible-ink-centered number raster equivalence across 2-digit, symmetric/asymmetric 3-digit, and widest decimal layouts, widest-layout jitter clearance, the larger centered clock footprint, dimming, an exact one-line SSID raster, and exact jitter, then publishes `docs/screens/index.html` plus 37 individual 240×240 PNGs. The complete run holds an inherited exclusive lock on the no-follow, owner/mode/inode-validated repository-parent namespace plus a separate repository identity descriptor, so replacing the repository pathname cannot create a second renderer lock domain. All writable build artifacts live in a private per-run workspace. Publication copies single-link regular files through directory descriptors and uses signal-masked, invocation-owned rollback with atomic Linux `renameat2(RENAME_NOREPLACE)` moves. It performs no file or directory deletion: successful replacement retains the complete previous generation in an ignored quarantine, and rollback retains the copied failed candidate, avoiding loss of a recoverable generation or deletion of a swapped entry. The values are synthetic visual fixtures. The rendered contact sheet is software evidence; panel color, viewing-angle brightness, and night comfort remain physical-device gates.

Coverage is enforced above 90% for executable bridge-domain statements, branches, functions, and lines. Repository commits are scanned with gitleaks; install the local pre-commit hook with:

```sh
scripts/install-hooks.sh
```

## Scenario-driven mock

Start the mock upstream; its clock defaults to the real current time so the production freshness checks remain meaningful:

```sh
npm run mock-upstream -- --scenario normal --port 8787
```

Select a deterministic time only for fixture-oriented work:

```sh
npm run mock-upstream -- --scenario stale_20m --port 8787 --now 2026-08-22T09:15:00.000Z
```

The full catalogue is `contracts/scenarios.yaml`. The mock control API and test fixtures contain no usable credentials or real health data.

## Device token generation

A device token is 32 random bytes encoded as canonical unpadded base64url. The ESP32 receives the raw token; the bridge configuration stores only its SHA-256 hash. Generate both once without putting either in Git:

```sh
python3 - <<'PY'
import hashlib, secrets
raw = secrets.token_urlsafe(32)
print(f"raw device token (provision only): {raw}")
print(f"DEVICE_TOKEN_HASHES={hashlib.sha256(raw.encode()).hexdigest()}")
PY
```

Keep the raw token in the provisioning workflow or password manager. Up to two active hashes may be configured during rotation. A canonical but inactive token receives HTTP 403; a missing or malformed token receives HTTP 401.

## Local compiled smoke test

1. Build the repository.
2. Start the mock on one port.
3. Start `dist/bridge/src/main.js` with `UPSTREAM_SOURCE=mock`, the mock URL, test-only Libre placeholders, and a SHA-256 device-token hash supplied through the environment.
4. Run:

```sh
BASE_URL=http://127.0.0.1:18080 \
DEVICE_TOKEN="$RAW_TEST_DEVICE_TOKEN" \
scripts/smoke.sh
```

The smoke script verifies unauthenticated liveness plus both authenticated latest-reading contracts without printing credentials, local wall time, or glucose values. Schema 1 remains a frozen 12-field compatibility response; schema 2 adds required `libre_local_time` for clock-aware firmware.

## Production configuration

Copy `.env.example` to a location outside the repository, normally `/etc/libre-desk/bridge.env`, replace every placeholder, and set mode `0600` or `0400`.

Required values:

- `LIBRE_EMAIL`, `LIBRE_PASSWORD`: dedicated follower account, never the primary Libre account;
- `LLU_APP_VERSION`, `LLU_PRODUCT`: client headers isolated from firmware;
- `DEVICE_TOKEN_HASHES`: one or two comma-separated SHA-256 hashes;
- `UPSTREAM_BASE_URL`, `UPSTREAM_SOURCE`;
- polling, maximum-age, urgent-low, optional target fallback, and firmware-directory settings shown in `.env.example`.

`RANGE_URGENT_LOW` is explicitly set to **70 mg/dL**. Normal target low/high values come from the Libre account. `RANGE_LOW` and `RANGE_HIGH` are permitted only as a pair and are used only if upstream omits both account thresholds.

## Container deployment

Set a hostname in `LIBRE_HOST`, point DNS at the host, ensure ports 80/443 are available, and run from a clean committed tree:

```sh
BRIDGE_ENV_FILE=/etc/libre-desk/bridge.env \
LIBRE_HOST=REPLACE_WITH_STABLE_HOSTNAME \
deploy/deploy.sh
```

`deploy/deploy.sh` validates environment-file permissions, Docker availability, the Compose model, Git cleanliness, and port availability before building and starting the bridge/Caddy stack. Caddy terminates HTTPS, applies HSTS/security headers, serves `/flash`, and proxies the API.

Compose parsing can be checked without deployment:

```sh
BRIDGE_ENV_FILE=/absolute/path/to/bridge.env \
docker compose -f deploy/docker-compose.yml config -q
```

The environment-file path should be absolute because Compose resolves it from the deployment project directory.

## Privacy and credentials

Never commit or log:

- LibreLinkUp email/password;
- auth tickets, JWTs, raw device tokens, or token hashes;
- real glucose measurements or identifiable telemetry fixtures;
- production environment files or firmware provisioning data.

Operational logs expose only aggregate outcomes and redacted error categories. Coredumps are accepted only through the authenticated bounded endpoint and written with mode `0600` to the configured ephemeral coredump directory.

See `PLAN.md` for milestone acceptance criteria and `docs/decisions/` for approved non-obvious decisions.
