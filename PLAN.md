# Libre Desk — Implementation Plan (software)

**Target reader:** an autonomous coding agent running on the VPS, with the device owner as the human operator.
**Date:** 2026-08-22
**Status:** implementation in progress; M1 contract/mock-upstream and M2 mock-backed bridge are complete. M1.5 exact-board firmware, `esp_lcd`/`esp_lvgl_port` integration, browser flashing, color/orientation calibration, and cold-boot acceptance were completed on 2026-08-23. §15 was answered on 2026-08-22 and is folded into this document.

---

## 0. TL;DR and the one structural insight

Build two things and three simulators:

| Thing | Where it runs | Who builds it | Who verifies it |
|---|---|---|---|
| `bridge` — LibreLinkUp → normalized JSON over HTTPS | VPS | agent | agent (fully) |
| `firmware` — ESP32-C6 client + display | the desk | agent | agent (logic + pixels), human (the physical device) |
| `mock-upstream` — fake Abbott | VPS | agent | agent |
| `uisim` — LVGL host simulator rendering UI states to PNG | VPS | agent | agent, human reviews PNGs |
| `flasher` — browser WebSerial flash page | VPS, used from Windows Chrome | agent | human clicks once |

**The structural insight that should drive every technical decision:**

> The implementing agent is headless. It cannot see the screen, press the button, or plug in the cable.

That single fact inverts the usual embedded tradeoffs. It means the plan must maximise *what can be proven on the VPS* and minimise *what requires a human at the desk*. Concretely:

1. All non-trivial firmware logic lives in a **pure C component with zero ESP dependencies**, compiled and unit-tested natively on the VPS.
2. The **UI is developed in a host LVGL simulator** that renders every state to PNG, so the agent can iterate on layout without hardware and the human reviews a contact sheet instead of squinting at a 1.3" panel.
3. **Firmware flashing happens once over a cable**, then everything moves to **OTA with rollback**, so the human is removed from the loop after M6.
4. **Core dumps ship to the bridge**, so a panic on the desk becomes a full symbolicated backtrace in the agent's terminal.

Everything below is in service of those four points.

---

## 1. Scope, non-goals, and safety framing

### In scope (v1)
A single always-on desk display showing: latest glucose value, trend arrow, unit, reading age, connection/health status. Plus the server-side bridge that feeds it, and the tooling to build, deploy, flash, and debug it without the agent ever touching the hardware.

### Explicit non-goals
- No direct Bluetooth/NFC to the sensor. The phone + official app remain the only thing talking to the sensor.
- No alarms, no buzzer, no vibration, no high/low alerting of any kind.
- No charts, history, menus, or on-device web UI.
- No treatment or dosing information.
- No inbound ports opened on the home router — the device is a pure HTTPS client.

### Safety framing (state this in the README and keep it true in code)
This is a **glanceable convenience display**, not a medical device. The official FreeStyle Libre app/reader remains the authoritative source for readings, alarms, and any treatment decision. The design rule that follows from that is absolute and is tested explicitly:

> **An old, malformed, or time-invalid glucose value must never remain on screen looking current.**

Two hard corollaries the agent must never violate:
- **Never synthesise a value.** No interpolation, no last-known-good re-display in the normal large-number format, no placeholder numbers in fallback code paths, no plausible-looking defaults in tests that could leak into production paths. When in doubt, the screen shows `--` and a reason.
- **Fail closed.** The bridge returns an explicit HTTP error rather than a stale or invented reading. The device shows a degraded state rather than the last number.

### Operational reality to acknowledge, not litigate
LibreLinkUp has no documented public consumer API. The bridge uses the private backend that the LibreLinkUp Android app uses. That backend changes without notice — header requirements, version pinning, and regional endpoints have all changed historically. This is a personal-use project against your own account and your own data; the plan simply assumes **the upstream client is the volatile part** and isolates it accordingly (§5.2). Expect to patch it once or twice a year.

---

## 2. Decisions (made, with rationale and escape hatches)

These are decided so the agent does not burn a week re-deciding them. Each has an escape hatch.

### D1 — Firmware framework: **ESP-IDF v5.5.x + LVGL 9** (not Arduino core + LVGL 8)

Your original notes preferred the Arduino/LVGL-8 vendor baseline. I'm recommending against it *because the developer is headless*, which was not the assumption behind that note.

| Need | ESP-IDF 5.5 | Arduino core 3.x |
|---|---|---|
| Reproducible container build (`espressif/idf:v5.5.5`) | native | arduino-cli, workable but fussier |
| Host-side unit tests of pure logic | native (CMake/CTest, linux target) | needs hand-rolled harness |
| OTA **with rollback** (`esp_ota_mark_app_valid_cancel_rollback`) | first-class | bolted on, no rollback |
| Core dump to flash + symbolication | first-class (`esp-coredump`) | absent |
| Task watchdog, per-task subscribe | first-class | partial |
| Mozilla root bundle for TLS (`esp_crt_bundle`) | first-class | available |
| NVS namespaces, encryption | first-class | wrapped, lossy |
| Vendor demo code exists | not for this board | yes |

The only column Arduino wins is the last one — and it wins it once, for one afternoon. IDF wins the columns that matter every day for the next year, in particular **rollback**, which is what lets the agent ship firmware without you standing by with a USB cable.

- **Pin ESP-IDF v5.5.x** (v5.5.5 was current as of 2026-07-16). Do **not** start on v6.0.x: `esp_lvgl_port` / `esp-bsp` compatibility there is unverified. Upgrading to v6 is a deliberate later task with its own soak.
- Use `espressif/esp_lvgl_port` (handles LVGL tick, mutex, and `esp_lcd` plumbing) plus the in-tree `esp_lcd_panel_st7789` driver. No vendor display code needed — only the pin map and any init quirks.

**Escape hatch:** M4 (first light on the panel) is timeboxed to **one working session**. If the ST7789V2 does not initialise correctly under IDF within that box, fall back to Arduino core 3.x + LVGL 8.3.10 *for the display layer only*, keep `glucose_core` framework-agnostic C so nothing else is lost, and re-attempt IDF later. Record the decision in `docs/decisions/`.

### D2 — Bridge: **Node 22 LTS + TypeScript + Fastify + zod + undici**, with an **in-house LibreLinkUp client**

Do **not** take a runtime dependency on `@diakem/libre-link-up-api-client`, `pylibrelinkup`, or `nightscout-librelink-up`. Vendor roughly 250 lines instead.

Rationale: the upstream client is precisely the part that breaks. When Abbott bumps the required `version` header or adds a new one, you want the agent to fix it in five minutes by editing one file — not to wait on an unmaintained npm package, or to fork it under pressure. Those three projects remain extremely valuable as **reference implementations to diff against** (Appendix B); they are read, not imported.

zod is doing real work here, not ceremony: it is the enforcement point for "reject malformed, missing, implausible, or future-dated readings."

**Escape hatch:** if the in-house client proves painful, `pylibrelinkup` can be run as a sidecar process behind the same internal interface.

### D3 — Device credentials are fully separated from Abbott credentials
The ESP32 holds: Wi-Fi SSID/password, bridge base URL, one device token, and display preferences. It never holds, and can never obtain, the LibreLinkUp email/password or the Abbott auth ticket. Compromising the device leaks your Wi-Fi password and roughly one recent glucose value. Revoking the device token is a one-line operation that does not touch the Libre account.

### D4 — **The server computes age.** The device does not rely solely on its own clock
Every response carries `server_time` and a precomputed `age_s`. The device still runs SNTP (it needs correct time for TLS validation anyway), but the freshness state machine keys off `age_s + monotonic_seconds_since_receipt`, not off `now() - timestamp`.

This eliminates an entire class of bugs where a wrong clock (post-reboot, DST, timezone, NTP failure) makes a four-hour-old reading render as "1m". Given the absolute rule in §1, this is worth two extra fields.

### D5 — Trend enum: **emit only what Libre 2 actually reports** in v1
LibreLinkUp's `TrendArrow` is `1=SingleDown, 2=FortyFiveDown, 3=Flat, 4=FortyFiveUp, 5=SingleUp`, with `6=NotComputable` and `7=RateOutOfRange` also observed. That is five directions, not eight.

The eight-value enum stays as the wire contract (so the firmware never needs a contract change), but v1 populates only:

| TrendArrow | wire value |
|---|---|
| 5 | `rising` |
| 4 | `rising_slightly` |
| 3 | `stable` |
| 2 | `falling_slightly` |
| 1 | `falling` |
| 6, 7, missing, unrecognised | `unknown` |

`rising_fast` / `falling_fast` are **reserved and unused in v1**. Deriving them from rate-of-change would mean inventing a clinical-looking signal the official app does not show you — exactly the kind of derived judgement this project should not make. Revisit only if you explicitly want it (§16 Q5).

### D6 — Flash once over cable, then OTA forever
M4 delivers a browser-based flasher page on the VPS (ESP Web Tools / WebSerial; works in Chrome and Edge on Windows, requires HTTPS, which you already have). You click **Connect → Install** once. From then on the agent publishes firmware to an OTA manifest and the device self-updates, with automatic rollback on a failed boot. No local toolchain on the Windows machine, ever.

### D7 — Three simulators, built before or alongside the firmware
`mock-upstream`, `uisim`, and (optional) Wokwi CI. Details in §7. These are not nice-to-haves; they are the agent's only sensory organs.

### D8 — Firmware is split pure/impure at a hard boundary
`components/glucose_core/` is C99 with **zero** `esp_*` includes: JSON validation, trend mapping, unit conversion, freshness state machine, string formatting, backoff calculation. It compiles with plain gcc and is unit-tested on the VPS with full branch coverage. Target: more than 70% of firmware bugs are catchable without hardware.

### D9 — In-memory cache only on the bridge; no health data at rest
Only the latest validated reading is kept, in process memory. It is not written to disk, not written to a database, not persisted across restarts. Rationale: (a) the device must not show a cached value after a restart anyway, so persistence buys nothing; (b) it keeps health data off the VPS disk and out of backups entirely. A restart simply produces a `503` for up to ~60 s until the first poll lands, which the device renders as a normal degraded state.

### D11 — **Glucose-range colouring is in v1**, and the thresholds come from your own Libre app

Requested explicitly. I had recommended against it on the grounds that range classification is a clinical judgement — but that objection dissolves if the bridge does not *invent* the ranges. It doesn't have to: `/llu/connections` returns **`targetLow` and `targetHigh` for the patient**, which are the target range you configured in the Libre app itself. Use those.

- The bridge reads `targetLow` / `targetHigh` from the connection object and classifies server-side, emitting a `range` field plus the thresholds it used (§4). One source of truth, changeable in your Libre app without reflashing anything.
- `URGENT_LOW` uses a separate configurable threshold, **70 mg/dL (3.9 mmol/L)**, confirmed by the device owner before M2. It is the one number in the system not read from your account.
- Env overrides (`RANGE_LOW`, `RANGE_HIGH`, `RANGE_URGENT_LOW`) exist for the case where the connection object doesn't carry them; if neither source is available, the bridge emits `range: "unknown"` and the device falls back to freshness-only colouring. It never guesses a range.

**Colour and freshness must not fight over the same channel.** Resolution:

| channel | encodes |
|---|---|
| **hue** | glucose range (`urgent_low` / `low` / `in_range` / `high`) |
| **brightness / opacity** | freshness (`FRESH` full, `AGING` dimmed) |
| **absence of a number** | `STALE` and every error state — always neutral grey, no hue at all |

So a dim red number means "low, and this reading is getting old", and a stale reading can never appear in *any* colour, which keeps the §1 rule intact.

**One caveat to keep in mind and to state in the README:** a red number can read as an alarm. It is not one. The device has no alarm capability, makes no sound, does not flash, and must not be relied on to tell you about a low. Colour here is recall of a value you already have, not notification of one you don't.

### D10 — Contract first, frozen at M1
The JSON contract (§4) is written, schema-validated, and pinned in `contracts/` before either side is built. Both the bridge and the firmware are written against it and tested against the same fixtures. The agent must not change the contract unilaterally after M1 (§14).

---

## 3. Architecture and repository layout

```
Libre 2 sensor  --BLE/NFC-->  phone (official app)  --internet-->  Abbott / LibreView cloud
                                                                          |
                                                    dedicated LibreLinkUp follower account
                                                                          |
                                          [ VPS ]  poller ---> validator ---> in-memory latest
                                                                          |
                                                       Fastify  GET /api/v1/latest  (Bearer)
                                                                          |
                                                              Caddy (TLS, HSTS, rate limit)
                                                                          |
                                                    [ desk ]  ESP32-C6  polls every 30-60 s
```

### Monorepo

```
libre-desk/
├── contracts/
│   ├── openapi.yaml                 # single source of truth for the device API
│   ├── latest.schema.json           # JSON Schema, consumed by BOTH sides
│   └── fixtures/                    # golden responses: fresh, aging, stale, error, malformed...
├── bridge/
│   ├── src/
│   │   ├── upstream/                # LibreLinkUp client (the volatile part, isolated)
│   │   │   ├── client.ts            # login, region redirect, ToU, connections, graph
│   │   │   ├── headers.ts           # product/version/account-id  <-- edit here when Abbott changes
│   │   │   └── types.ts
│   │   ├── domain/                  # validation + normalization (pure, heavily tested)
│   │   │   ├── validate.ts
│   │   │   ├── normalize.ts
│   │   │   └── freshness.ts
│   │   ├── poller.ts                # single-flight, jitter, exponential backoff
│   │   ├── cache.ts                 # in-memory latest only
│   │   ├── http/
│   │   │   ├── server.ts
│   │   │   ├── auth.ts              # hashed device tokens, constant-time compare
│   │   │   ├── routes.latest.ts
│   │   │   ├── routes.health.ts
│   │   │   ├── routes.telemetry.ts  # device logs + coredump intake
│   │   │   └── routes.ota.ts        # firmware manifest + binary
│   │   └── obs/                     # metrics, redacting logger
│   ├── test/                        # vitest: unit + contract + scenario
│   └── Dockerfile
├── firmware/
│   ├── main/
│   │   ├── main.c                   # init order, event loop wiring
│   │   ├── net_task.c               # HTTPS client, ETag, retries
│   │   ├── ui_task.c                # LVGL, owns the screen
│   │   ├── provisioning.c           # SoftAP setup portal, NVS
│   │   ├── timekeeping.c            # SNTP + monotonic
│   │   ├── ota.c                    # esp_https_ota + rollback confirm
│   │   ├── diag.c                   # coredump upload, heap watermark, log shipping
│   │   └── board.h                  # PIN MAP - single place, derived in M4
│   ├── components/
│   │   ├── glucose_core/            # PURE C99, no esp_*  <-- the tested brain
│   │   │   ├── include/glucose_core.h
│   │   │   ├── parse.c  trend.c  freshness.c  format.c  backoff.c
│   │   │   └── test/                # CTest, runs on the VPS
│   │   └── ui_screens/              # LVGL layout, also builds on host
│   ├── partitions.csv
│   ├── sdkconfig.defaults
│   └── sdkconfig.defaults.esp32c6
├── tools/
│   ├── mock-upstream/               # fake Abbott, scenario-driven
│   ├── uisim/                       # LVGL host sim -> PNG per state
│   ├── flasher/                     # static ESP Web Tools page + manifest
│   └── scenario/                    # CLI to drive bridge+device through every state
├── deploy/
│   ├── Caddyfile
│   ├── docker-compose.yml           # the deployment (§5.5)
│   └── deploy.sh                    # pull, build, up, smoke - idempotent
├── firmware-releases/               # signed OTA binaries + manifest, mounted read-only
├── docs/
│   ├── RUNBOOK.md
│   ├── screens/                     # committed PNG contact sheets of every UI state
│   └── decisions/                   # one short ADR per non-obvious choice
└── PLAN.md                          # this file
```

---

## 4. The device API contract (freeze this first)

### `GET /api/v1/latest`

**Request**
```
GET /api/v1/latest HTTP/1.1
Host: <bridge-host>
Authorization: Bearer <device-token>
If-None-Match: "<etag from last 200>"
User-Agent: libre-desk/<fw-version> (<device-id>)
Accept-Encoding: identity
```

`Accept-Encoding: identity` is deliberate — gzip on a 200-byte body costs the ESP32 more RAM than it saves in bytes.

**200 OK**
```json
{
  "glucose": 112,
  "unit": "mg/dL",
  "trend": "stable",
  "range": "in_range",
  "target_low": 70,
  "target_high": 180,
  "timestamp": "2026-08-22T09:15:00Z",
  "age_s": 47,
  "server_time": "2026-08-22T09:15:47Z",
  "source": "librelinkup",
  "poll_after_s": 45,
  "schema": 1
}
```

| field | type | notes |
|---|---|---|
| `glucose` | number | integer for mg/dL; one decimal for mmol/L |
| `unit` | `"mg/dL"` or `"mmol/L"` | defaults to the unit your LibreLinkUp account reports; overridable per device. Internal storage is always mg/dL |
| `trend` | enum (D5) | 8 values defined, 6 emitted in v1 |
| `range` | `"urgent_low"` / `"low"` / `"in_range"` / `"high"` / `"unknown"` | classified server-side against your own Libre target range (D11). `unknown` when no thresholds are available — the device then uses freshness-only colouring |
| `target_low` / `target_high` | number, same unit as `glucose` | echoed for transparency and for the runbook; sourced from `/llu/connections` |
| `timestamp` | RFC3339 UTC | **derived from `FactoryTimestamp`, never from server fetch time** |
| `age_s` | integer >= 0 | `server_time - timestamp`, computed server-side (D4) |
| `server_time` | RFC3339 UTC | |
| `source` | string | `"librelinkup"` or `"mock"` — the device shows a marker when not `librelinkup` |
| `poll_after_s` | integer | server-directed pacing; device clamps to [20, 300] |
| `schema` | integer | bump only on breaking change; device refuses an unknown major |

Headers: `ETag`, `Cache-Control: no-store`, `X-Request-Id`.

**304 Not Modified** — nothing changed since the client's ETag. Empty body. The device keeps its value and refreshes `age_s` from the `server_time` echoed in a response header. This is the common case and saves roughly 90% of bytes and parse time.

**Error responses** — always this shape, never a partial success:
```json
{ "error": "upstream_unavailable", "detail": "no validated reading within max_age", "retry_after_s": 30 }
```

| status | `error` | meaning | device state |
|---|---|---|---|
| 401 | `invalid_token` | bad or absent token | `AUTH_ERROR` (screen: `--` + `AUTH`) |
| 403 | `revoked_token` | token revoked | `AUTH_ERROR` |
| 429 | `rate_limited` | too many requests | back off to `Retry-After` |
| 503 | `upstream_unavailable` | no validated reading available | `NO_DATA` |
| 503 | `upstream_auth_failed` | bridge cannot log in to Abbott | `NO_DATA` + `SRV` marker |
| 500 | `internal` | bug | `SRV_ERROR` |

The bridge **never** returns 200 with a fabricated, guessed, or expired-beyond-`max_age` value.

### Other endpoints
- `GET /healthz` — no auth, no health data. `{ok, upstream_ok, last_success_age_s, version}`. For uptime monitoring.
- `GET /metrics` — Prometheus, token-protected or bound to localhost.
- `POST /api/v1/telemetry` — device to bridge: firmware version, uptime, min free heap, Wi-Fi RSSI, reconnect count, HTTP error counts — **and no glucose values**. Rate-limited to 1 per 5 min.
- `POST /api/v1/coredump` — base64 core dump on the boot after a panic. The agent symbolicates it with the matching ELF.
- `GET /api/v1/ota/manifest` — `{version, url, sha256, min_prev_version}`. Token-protected.

---

## 5. Bridge design

### 5.1 Poller state machine
```
BOOT -> LOGIN -> RESOLVE_PATIENT -> POLL <-> BACKOFF
                                      |
                                  (401) -> LOGIN (max 1 retry, then hard backoff)
```
- Base interval **60 s**, jitter +/- 10 s. Libre 2 followers get roughly one new value per minute when the phone is uploading; polling faster gains nothing and looks abusive.
- **Single-flight**: device requests never trigger an upstream call. The poller is the only thing that talks to Abbott. Ten devices polling every 5 s would still produce exactly one upstream request per minute.
- Backoff on failure: 60 → 120 → 240 → 480 → 900 s cap, with jitter. Reset on success.
- The auth ticket is long-lived (observed around six months). Cache it in memory with its `expires`, refresh at 90% of TTL. Steady state should be **well under one login per day**.
- On `redirect: true` in the login response, re-issue against `https://api-<region>.libreview.io` and remember the region.
- Handle the terms-of-use / privacy-policy interstitial explicitly: log a clear, actionable error ("accept the new terms in the LibreLinkUp app, then restart the bridge") rather than looping.

### 5.2 Upstream client — the volatile surface
Everything Abbott-specific lives in `src/upstream/`. Currently required:

```
POST {base}/llu/auth/login          {email, password}
GET  {base}/llu/connections
GET  {base}/llu/connections/{patientId}/graph
```
Headers on every request:
```
content-type: application/json
accept-encoding: gzip
cache-control: no-cache
product: llu.android
version: <LLU_APP_VERSION>               # e.g. 4.16.0 - ENV VAR, will need bumping over time
authorization: Bearer <authTicket.token>
account-id: <sha256hex(data.user.id)>    # required since app v4.16.0
```

Make `version` and `product` **environment variables, not constants**. When Abbott starts rejecting the pinned version, the fix must be a config change and a restart — not a code deploy.

The latest reading comes from `data.connection.glucoseMeasurement` on the graph response (it is also present on `/llu/connections`). Fields used:

| field | use |
|---|---|
| `FactoryTimestamp` | **the timestamp to publish** — UTC |
| `Timestamp` | ignore for logic; it is patient-local with no offset, a classic footgun |
| `ValueInMgPerDl` | canonical value; store this, convert on output |
| `Value` + `GlucoseUnits` | `GlucoseUnits` sets the **default display unit** (the account's own preference); `Value` is a cross-check — log a warning on disagreement with `ValueInMgPerDl` |
| `TrendArrow` | mapped per D5 |
| `targetLow` / `targetHigh` (on the **connection**, not the measurement) | **the range thresholds** (D11). Read once per login, refresh with each connections call |
| `isHigh` / `isLow` | cross-check the computed `range` against Abbott's own flags; log a warning on disagreement, but trust the numeric comparison |
| `MeasurementColor` | not used — its 1/2/3/4 semantics vary between observed API versions and are not worth depending on |

**Region and units are auto-detected, not configured** (§15 Q1/Q2). Region comes from the login redirect: attempt the default base URL, follow `data.redirect: true` to `https://api-<region>.libreview.io`, then cache the resolved region in memory and log it once at startup. Units default to the account's `GlucoseUnits`. Both are overridable by env var for the case where detection is wrong, but neither requires you to know the answer in advance.

### 5.3 Validation (`domain/validate.ts`) — reject, don't repair
Each of these produces a rejection, an incremented counter, and a redacted log line — never a coerced value:
- non-numeric, `NaN`, or missing `ValueInMgPerDl`
- value outside **[20, 500] mg/dL** (implausible / sensor-error range)
- unparseable timestamp
- `timestamp > server_time + 120 s` (future-dated: clock skew or upstream bug)
- `age_s > MAX_AGE_S` (default 3600) → treated as no data
- unrecognised trend → `unknown`, not a guess
- schema mismatch anywhere → reject the whole reading

If the current reading fails validation, the previously cached valid reading is **kept and continues to age**; it is not replaced by garbage, and its timestamp is not refreshed. Once it ages past `MAX_AGE_S`, the endpoint starts returning 503.

### 5.4 Auth on the device endpoint
- Token = 32 random bytes, base64url. Stored **hashed** (SHA-256 is sufficient here because the token is high-entropy; argon2 is unnecessary).
- Constant-time comparison.
- Support **two active tokens** simultaneously so rotation never bricks the device mid-flight.
- Per-token rate limit: 120 requests/hour, burst 10. 429 with `Retry-After`.
- Revocation = remove the hash from config + reload. Takes effect within seconds and does not touch the Libre account.

### 5.5 Deployment — **Docker Compose** (§15 Q3)

Two services, one network, no volumes carrying health data.

```yaml
services:
  bridge:
    build: ./bridge
    restart: unless-stopped
    env_file: [ /etc/libre-desk/bridge.env ]     # 0600, root-owned, OUTSIDE the repo
    read_only: true
    tmpfs: [ /tmp ]
    user: "10001:10001"                          # non-root
    cap_drop: [ ALL ]
    security_opt: [ "no-new-privileges:true" ]
    mem_limit: 256m
    healthcheck:
      test: ["CMD", "node", "-e", "fetch('http://localhost:8080/healthz').then(r=>process.exit(r.ok?0:1))"]
      interval: 30s
      start_period: 20s
    volumes:
      - ./firmware-releases:/srv/firmware:ro     # OTA binaries, read-only, no health data
  caddy:
    image: caddy:2-alpine
    restart: unless-stopped
    ports: [ "80:80", "443:443" ]
    volumes:
      - ./deploy/Caddyfile:/etc/caddy/Caddyfile:ro
      - caddy_data:/data                          # ACME certs only
      - caddy_config:/config
volumes: { caddy_data: {}, caddy_config: {} }
```

Notes that matter:
- **`read_only: true` on the bridge is load-bearing**, not decoration. Combined with D9 (in-memory cache only) it makes "no health data at rest" a property the container filesystem enforces rather than one the code promises.
- Secrets live in `/etc/libre-desk/bridge.env`, `0600`, root-owned, **outside the repo directory** so a stray `git add -A` cannot reach them.
- Multi-stage Dockerfile: build with the full toolchain, ship `node:22-alpine` with production deps only. Pin the base image by digest.
- **Check port 80/443 first.** If something already holds them, the agent must ask rather than fight over the binding.
- Log driver capped (`max-size: 10m`, `max-file: 3`) so a retry storm cannot fill the disk.
- **Survives reboot**: `restart: unless-stopped` plus a `docker compose up -d` systemd unit or `docker.service` enabled. Verified at M3 by actually rebooting the VPS — not by reading the config.
- `deploy/deploy.sh`: `git pull && docker compose build && docker compose up -d && ./scripts/smoke.sh`. One command, idempotent.

---

## 6. Firmware design

### 6.1 Hardware facts (verify, don't trust this table)
- **ESP32-C6FH4**: single RISC-V HP core plus an LP core, **4 MB in-package flash, no PSRAM**, ~512 KB HP SRAM.
- **1.3" IPS, 240x240, ST7789V2 over SPI**, 262K colour.
- Onboard RGB LED (WS2812-type), BOOT and RESET buttons, TF slot, 9-pin 2.54 mm header.

**Deriving the pin map is task #1 of M4, and it must be lifted from Waveshare's own demo source or schematic for the *1.3* board — not guessed, and not copied from the 1.47 sibling.** For plausibility-checking only, the 1.47 board uses SCLK=7, MOSI=6, MISO=5, CS=14, DC=15, RST=21, BL=22. Treat that as a sanity anchor, nothing more. Record the confirmed map in `firmware/main/board.h` with a comment citing the source.

### 6.2 Memory budget (the real constraint — no PSRAM)
| item | budget |
|---|---|
| LVGL draw buffers: 2 x (240 x 40 x 2 B), partial | ~38 KB |
| mbedTLS session (`MBEDTLS_SSL_IN_CONTENT_LEN=4096`, `OUT_CONTENT_LEN=2048`) | ~25-30 KB |
| Wi-Fi + LWIP | ~50-60 KB |
| LVGL objects + fonts (fonts live in flash) | small |
| **Alarm threshold** | log + telemetry if `esp_get_minimum_free_heap_size()` < 40 KB |

Non-negotiables: RGB565 only; **partial** draw buffers, never full-frame; one large digit font plus one small text font, both **subsetted** (digits, `.`, `-`, and the handful of characters actually used) via the LVGL font converter; trend arrow drawn as an LVGL polygon or line, not an image; no PNG/JPEG decode; no SD access; no on-device web server.

### 6.3 Partition table (`partitions.csv`, 4 MB)
```
nvs,       data, nvs,     0x9000,   24K
otadata,   data, ota,     0xf000,    8K
phy_init,  data, phy,     0x11000,   4K
ota_0,     app,  ota_0,   0x20000, 1600K
ota_1,     app,  ota_1,  0x1B0000, 1600K
coredump,  data, coredump,      ,   64K
```
Verify the app binary lands under about 1.4 MB with room to grow. If it does not, **cut LVGL features before cutting the OTA slot** — rollback is worth more than any UI feature, because it is what keeps the human out of the loop.

### 6.4 Tasks and data flow
```
ui_task   (prio 4, 4 KB)  : lv_timer_handler loop, owns the display, reads snapshot under mutex
net_task  (prio 5, 8 KB)  : SNTP -> HTTPS poll -> glucose_core_parse -> publish snapshot
diag_task (prio 2, 3 KB)  : heap watermark, telemetry POST, coredump upload, OTA check
esp_event loop            : wifi/ip events -> state transitions
```
- Shared state is one small struct behind a mutex; the UI never blocks on the network and vice versa.
- **TWDT** subscribes `ui_task` and `net_task`. A hung task reboots the device rather than freezing a number on screen forever — which is exactly the failure mode the §1 rule exists to prevent.
- Wi-Fi power save `WIFI_PS_MIN_MODEM` — mains powered, but it keeps the board cool in a small enclosure.

### 6.5 Boot sequence (order matters)
```
1. NVS init -> load config. Missing or invalid -> provisioning mode.
2. Display init -> immediately show the "starting" state. Never a number.   <-- the §1 rule
3. Wi-Fi connect (state: WIFI_CONNECTING, SSID shown small)
4. SNTP sync (state: TIME_SYNCING). TLS validation needs a sane clock; SNTP is
   UDP, so there is no chicken-and-egg. Timeout 30 s -> retry; do not proceed without it.
5. First HTTPS fetch (state: FETCHING)
6. On the first fully successful cycle (wifi + time + fetch + render):
   esp_ota_mark_app_valid_cancel_rollback()      <-- the rollback gate
7. Steady-state loop.
```
Step 6 is the entire safety net for OTA: firmware that cannot reach the bridge never marks itself valid, so the bootloader rolls back to the previous image on the next reset. **Do not call it earlier than a full successful cycle.**

### 6.6 Display states
Thresholds are configurable and stored in NVS. Defaults:

| state | condition | screen |
|---|---|---|
| `BOOT` | 0-2 s | product mark, no number |
| `WIFI_CONNECTING` | | `Wi-Fi...` + SSID (small) |
| `TIME_SYNCING` | | `Time...` |
| `FETCHING` | first fetch pending | `...` |
| `FRESH` | age <= 6 min | **big number**, arrow, unit, age, wifi glyph |
| `AGING` | 6 < age <= 15 min | same, value dimmed, age emphasised |
| `STALE` | 15 min < age <= 60 min | **`--`**, arrow hidden, age shown |
| `NO_DATA` | age > 60 min, or 503 | `--` + `no data` |
| `AUTH_ERROR` | 401 / 403 | `--` + `AUTH` |
| `SRV_ERROR` | 5xx / malformed | `--` + `SRV` |
| `NO_WIFI` | disconnected > 30 s | `--` + wifi-slash glyph |

Rules that are unit-tested, not merely documented:
- The large-number layout is reachable **only** from `FRESH` and `AGING`.
- On boot, no cached value is restored into the large-number layout under any circumstance.
- The age on screen increments continuously from the device's monotonic clock even when every poll is failing — a frozen age is itself a bug.
- Error strings are 8 characters or fewer. The screen is 1.3 inches.

### 6.7 Layout (240x240)
```
        +----------------------+
        |                      |   y=  0..30   quiet zone
        |    112      ->       |   y= 30..150  value ~100px + arrow ~44px
        |     mg/dL            |   y=150..180  unit, 20px, muted
        |                      |
        |  1m            ((o   |   y=205..235  age (left) | status glyph (right)
        +----------------------+
```
**Colour (D11).** Hue carries range, brightness carries freshness, and the two never collide because stale readings show no number at all:

| `range` | hue | `FRESH` | `AGING` |
|---|---|---|---|
| `urgent_low` | deep red | full brightness | dimmed ~55% |
| `low` | red | full | dimmed |
| `in_range` | white / near-white | full | dimmed |
| `high` | amber | full | dimmed |
| `unknown` | white | full | dimmed |
| — | `STALE`, `NO_DATA`, `AUTH_ERROR`, `SRV_ERROR`, `NO_WIFI` | **neutral grey, no hue, no number** | |

Pick hues that survive an IPS panel at an angle and stay distinguishable when dimmed — verify in `uisim` at both brightness levels before committing them. Test explicitly that no code path can produce a coloured number in a stale or error state.

Add a slow +/-2 px position jitter every ~5 minutes and scheduled night dimming — cheap insurance against image sticking and against a bright square in a dark room.

### 6.8 Configuration
**In NVS:** logical fields `wifi_ssid`, `wifi_pass`, `bridge_url`, `device_token`, `unit`, `brightness_day`, `bright_night`, `night_start`, `night_end`, `tz` (POSIX TZ string), `thresh_fresh_s`, `thresh_aging_s`, `thresh_stale_s`, `poll_s`, `ota_enabled`. `bright_night` is the logical stem for `brightness_night_percent` because ESP-IDF NVS keys are limited to 15 characters excluding NUL. The Task 7 transactional adapter stores two complete slots by appending `a` or `b` to each stem; the longest physical keys are exactly 15 characters, and an unsuffixed `active` selector is published only after the inactive slot is complete, checksummed, and valid. A private static mutex serializes full load/save/reset transactions. Fresh or invalid storage uses the validated compile-time private configuration as a recoverable fallback; a successful Wi-Fi-only provisioning submission copies that active configuration, changes only `wifi_ssid` and `wifi_password`, validates the complete candidate, and atomically publishes it without selecting a stale slot.

The validated model uses byte-counted, bounded C arrays: SSID 1–32 UTF-8 bytes; password empty, 8–63 UTF-8 bytes, or exactly 64 ASCII hexadecimal bytes; canonical lowercase HTTPS DNS origin up to 255 bytes with no path/query/fragment/userinfo; the bridge's canonical 43-character base64url device token encoding exactly 32 bytes; and a strict ASCII POSIX TZ subset up to 64 bytes. Day brightness is 1–100%, night brightness is 1–day, schedule minutes are 0–1439 with unequal endpoints, and persisted polling is 20–300 s. Freshness must satisfy `1 <= fresh < aging <= 900 < stale <= 86400`, so no numeric state can exceed 15 minutes. Validation returns field-level enums only and never values, lengths, or dynamic error text.

**Never in NVS:** Libre credentials, Abbott ticket, VPS admin anything.

Provisioning amendment (2026-08-25): the on-demand private HTTPS **Owner Portal** retains the WebSerial firmware installer and adds a guided Wi-Fi workflow. Wi-Fi credentials are never submitted to that remote origin: the page instructs the owner to hold BOOT for 5 seconds while the application is running, join a temporary WPA2 SoftAP using the random password shown on the device, and submit the replacement SSID/password directly to `http://192.168.4.1`. The local portal accepts **only** SSID/password; it never displays or accepts bridge URL or device token. Do not hold BOOT during power-on/reset because GPIO9 is a strapping pin and that gesture enters download mode. A candidate must obtain an IP before the complete configuration—with every non-Wi-Fi field unchanged—is transactionally saved; failure leaves the previous configuration available for retry.

### 6.9 TLS
Use `esp_crt_bundle` (Mozilla roots). **Do not pin the leaf certificate** — Let's Encrypt rotates roughly every 60 days, and pinning guarantees a dead display several times a year with no fix except a cable. If you want tighter binding than the full bundle, pin the **ISRG Root X1** CA only, and record its expiry in the runbook.

### 6.10 OTA
`esp_https_ota` against `GET /api/v1/ota/manifest` (token-protected), SHA-256 verified before commit, rollback armed as in §6.5. Check on boot and every 6 hours. A `min_prev_version` field lets the agent refuse dangerous skips.

**Gate:** the rollback path must be demonstrated once, deliberately, with a knowingly-broken image, before OTA is trusted for unattended updates (M6).

---

## 7. The no-hardware development environment

### 7.1 `tools/mock-upstream` — fake Abbott
A small HTTP server implementing `/llu/auth/login`, `/llu/connections`, and `/llu/connections/:id/graph`, driven by a scenario file. It must be able to emit, on demand:

`normal` · `flat_line` · `rising` · `falling` · `stale_5m` · `stale_20m` · `stale_2h` · `no_readings` · `future_timestamp` · `value_out_of_range` · `null_value` · `malformed_json` · `truncated_response` · `http_500` · `http_401_expired_ticket` · `region_redirect` · `terms_of_use_required` · `slow_10s` · `connection_reset` · `mmol_units` · `trend_notcomputable` · `range_low` · `range_urgent_low` · `range_high` · `no_target_range` · `target_range_changed`

The bridge points at this in dev via `UPSTREAM_BASE_URL`. Every one of those scenarios becomes a test case. This is how you get confidence in error handling **without waiting months for Abbott to actually break**.

### 7.2 `tools/uisim` — LVGL host simulator to PNG
Compile `components/ui_screens/` against LVGL on Linux with a memory framebuffer, render every state at 240x240, and write PNGs:
```
make screens   # -> docs/screens/*.png + docs/screens/index.html contact sheet
```
The agent iterates on layout, font sizes, and truncation without touching hardware; you review one HTML page. This alone removes most of the flash / look / tweak / reflash cycle. **Every change that touches the UI must regenerate the contact sheet** — the visual diff is the review.

### 7.3 `tools/scenario` — end-to-end driver
One CLI that sets a mock-upstream scenario, waits for the bridge to poll, curls `/api/v1/latest`, and asserts the expected response. Once hardware exists it also prints "expected screen: STALE, `--`, `22m`", so you can glance at the desk and confirm. Used for the M7 acceptance run.

### 7.4 Optional: Wokwi CI
`diagram.json` with ESP32-C6 + ST7789 plus `wokwi-cli` headless on the VPS. Catches boot loops, panics, and init-order regressions before the human is asked to flash. Worth about half a day if it works on this chip/panel combination; abandon it quickly if it fights back. **Not on the critical path.**

---

## 8. Human-in-the-loop protocol

**The agent can:** write all code, build the bridge, deploy it, build firmware binaries, run all host tests, render UI PNGs, symbolicate core dumps, publish OTA updates.

**Only the device owner can:** buy the board, plug it in, click Install in the browser once, look at the physical screen, and report what happened.

To keep that second list as short as possible:

1. **Flashing** — the agent publishes `https://<host>/flash` (ESP Web Tools + `manifest.json`). Windows plus Chrome or Edge; no drivers, no toolchain, no Python. Connect → Install → done. The same page has a serial console, so the boot log is copy-pasteable.
2. **After the first flash, use OTA.** The cable should come out exactly once.
3. **Logs come back automatically.** `POST /api/v1/telemetry` and `POST /api/v1/coredump` mean the agent sees crashes without asking you for anything.
4. **When the agent does need you**, it must ask in this exact form, in a single message:
   > **Action needed (est. 2 min):** power-cycle the device and tell me which of these appears first: (a) `Wi-Fi...`, (b) `Time...`, (c) `...`, (d) blank screen.

   Closed questions with enumerated answers. Never "can you check if it works?"
5. **Screens are reviewed as PNGs**, not described in prose.

---

## 9. Milestones

Effort is in *agent working sessions*, not calendar days.

**Sequencing note — the board is already in hand (§15 Q7), which changes the order.** Nothing is waiting on shipping, so the highest-uncertainty item should move to the front instead of sitting at M4. Risk R3 (ST7789V2 bring-up under IDF) is the one thing that could invalidate decision D1, and it is cheap to test. So **M1.5, a one-session display spike, runs immediately after M1**, before the bridge is built. If it fails, D1's escape hatch is invoked while it costs almost nothing; if it succeeds, R3 is retired on day one and the rest of the plan proceeds with confidence.

### M0 — Preflight (no code) · ~0.5 session · **no hardware needed**
- Create the dedicated LibreLinkUp **follower** account, in the **same region** as the primary account, and accept the sharing invitation from the primary account. Confirm the follower sees live data **in the LibreLinkUp app** before a line of code is written.
- Confirm region code and units.
- Pick the subdomain; point DNS at the VPS.
- Answer §16.
- **DoD:** follower app shows a live value; §16 answered; DNS resolves.

### M1 — Repo, contract, mock upstream · ~1 session · **no hardware**
- Monorepo scaffold; CI on the VPS (lint, typecheck, test, build both sides).
- `contracts/openapi.yaml` + `latest.schema.json` + all fixtures written **first**.
- `tools/mock-upstream` with the full scenario list from §7.1.
- gitleaks pre-commit hook.
- **DoD:** `make test` green; every §7.1 scenario reachable; contract frozen.

### M1.5 — Display spike (de-risking, board in hand) · 1 session · **complete 2026-08-23**
The single question this answers: *does the ST7789V2 on this exact board come up under ESP-IDF 5.5 + `esp_lcd` + `esp_lvgl_port`?* Nothing else.
- Derive the pin map from Waveshare's demo source or schematic for the **1.3** board (§6.1) and record it in `board.h` with its source cited.
- Minimal IDF app: init the panel, fill red / green / blue / white, draw one large digit, confirm orientation and colour order (RGB vs BGR, inverted vs not — both are classic ST7789 traps).
- Publish `tools/flasher` far enough to be usable.
- **Software evidence:** the exact-board app builds under pinned ESP-IDF v5.5.5 with `esp_lcd`, `esp_lvgl_port` 2.9.0, and LVGL 9.5.0. The reproducible accepted ESP32-C6 image is 439,760 bytes (`0x6b5d0`), leaving 58% of the 1 MiB spike partition. Two clean builds produced identical bootloader, partition-table, and application SHA-256 hashes; `esptool image_info` validates the application checksum and image hash. Three flash parts are generated from `flasher_args.json`; fifteen fail-closed packager tests pass; the self-hosted ESP Web Tools page loads with every referenced module and binary present.
- **Human evidence (2026-08-23):** browser flashing succeeded on the exact board. With USB-C exiting right, firmware `0.1.2-display-calibration` showed the expected solid sequence, exact RGBW corner card, and intact upright/non-mirrored asymmetric digit. The same sequence and card returned after a five-second power removal and cold boot.
- **DoD:** complete. Correct colors and orientation are encoded in `board.h` plus the tested LVGL software-rotation setting; ADR 0003 records the rejected and accepted paths. Risk R3 is retired.
- **Later product gates:** PWM sweep behavior, repeated-boot testing, and long soak remain in M7/M8; they are not prerequisites for accepting M1.5 first light.
- **Hard timebox: one session.** Do not debug into a second.

### M2 — Bridge against mock only · 1-2 sessions · **no hardware** — **complete 2026-08-22**
- Upstream client, poller, validator, normalizer, cache, Fastify routes, auth, rate limit, metrics.
- Vitest suite covering **every** §7.1 scenario mapped to its expected §4 response.
- **DoD:** 100% of scenarios produce the contractually correct response; a test asserts that logged output never contains a token, password, JWT, or glucose value; coverage on `domain/` above 90%.
- **Completion evidence:** all 26 scenarios pass through the authenticated runtime HTTP boundary; the complete suite is 137/137; domain coverage is 96.96% statements, 96.66% branches, 100% functions, and 98.86% lines. Typecheck, build, full/production dependency audits, whitespace checks, and privacy/security regressions are green. The bridge rejects ambiguous patient identity, timestamp regressions, implausible target ranges, unsafe OTA URLs, and unbounded diagnostic uploads.

### M3 — Bridge against real Abbott, then deploy · ~1 session · **no hardware**
- Point at the real follower account. Record sanitized fixtures from real responses.
- Handle whatever reality differs from the docs. This *will* happen — expect at least one surprise in headers or the terms-of-use flow.
- systemd + Caddy + HTTPS. **Reboot the VPS and confirm it comes back by itself.**
- **DoD:** `curl -H "Authorization: Bearer ..." https://host/api/v1/latest` returns your actual glucose; 24 h unattended with fewer than 2 upstream failures and no unexpected logins; survives a real reboot.

*At this point the project is already useful — you can curl it from your phone. Everything after this is the physical object.*

### M4 — Firmware skeleton · ~1 session · **hardware, no new human action**
Builds on the M1.5 spike, which already retired the display risk.
- Full IDF project structure, partition table (§6.3), Wi-Fi connect, SNTP, event loop, TWDT, LVGL app scaffold.
- **Measure the binary size against the 1600K OTA slot now**, before any UI features are added — this is the decision point for R4.
- **DoD:** device joins Wi-Fi and syncs time unattended across a power cycle; clean boot log; binary size recorded with headroom stated.

### M5 — `glucose_core` and the real UI · 1-2 sessions · **mostly no hardware**

**Software status (2026-08-22): core and Signal First UI renderer complete; physical review pending.** The pure-C component parses the frozen success/error contracts with explicit 2048/512-byte body caps, rejects malformed UTF-8 and time inconsistencies, converts units, maps trends without guessing, tracks age monotonically with saturation, enforces the no-number/no-hue stale invariant, and bounds formatting/backoff. Twenty-two native tests pass under strict C99, ASan/UBSan, and coverage instrumentation. The coverage gate reports 92.81% state-facade lines, 100% branch-site execution, 91.89% branch directions taken, and every freshness-machine branch direction exercised. The shared `ui_screens` component compiles natively and with pinned ESP-IDF v5.5.5, and the host simulator renders the real LVGL component twice with byte-identical output.

- Pure C parser, trend map, freshness state machine, formatter, backoff — with CTest unit tests covering every malformed input in `contracts/fixtures/`.
- `uisim` renders 37 exact 240×240 PNG scenarios covering every §6.6 state, all five range classes (four visual hues; `in_range` and `unknown` share white), every trend enum/family, 2-digit, symmetric/asymmetric 3-digit, and widest-decimal visible-ink centering with and without arrows, mg/dL and mmol/L, aging opacity, long-SSID ellipsis, OTA progress, night dimming, and ±2 px jitter. It also produces a 4-column contact sheet and self-contained HTML review page.
- **UI direction selected (2026-08-22): Signal First.** Maximum visible reading size, the enlarged clock, trend arrow, age, and Wi-Fi take priority; the glucose unit is deliberately hidden, and the denser Clinical Strip and rounded Bedside Calm treatments are not part of the first implementation.
- **Current evidence:** all 37 outputs pass dimensions, file hashes, exact 40-artifact-set and manifest-path confinement, byte-identical consecutive rendering on the pinned host, independent decoded-RGB hashes for every one of the 12 no-number states, exact RGB565 palette values, visible-ink centering and independent arrow placement across every layout fixture, an independent geometry matrix over all 749 accepted values plus 1,498 native LVGL renders exercising each value without and with an arrow, exact trend-arrow and 64×17 clock bounds, isolated widest-layout ±2 px clearance, nonempty/distinct/oriented trend masks with unknown suppression and deliberate slight/full/fast family equivalence, exact one-line SSID raster, dimming, exact jitter translation, and asset-reference checks. The manifest embeds both the managed-component hash and the canonical full LVGL source-tree digest. The complete run locks the validated repository-parent namespace, retains a repository identity descriptor, and uses a private build workspace; publication copies single-link files through no-follow descriptors and uses signal-masked rollback with atomic `renameat2(RENAME_NOREPLACE)` moves. It performs no file or directory deletion: the complete previous generation remains in ignored quarantine after success, and a copied failed candidate remains quarantined after rollback. Native model tests directly reject number or hue requests outside `FRESH`/`AGING`; agent pixel inspection found no blocking layout defect. **Remaining DoD:** exact-panel color, viewing-angle brightness, and night comfort are accepted on hardware.

### M6 — Integration, OTA, rollback proof · 1-2 sessions · **hardware, 2 human actions**
- `net_task` + HTTPS + ETag/304 + NVS config + SoftAP provisioning + TWDT + telemetry + coredump upload.
- OTA end to end.
- **Deliberately publish a broken image and prove the device rolls back on its own.** This is the gate that retires the USB cable.
- **DoD:** the device displays live glucose; the provisioning portal works from a phone; a broken OTA rolls back unattended; a forced panic produces a symbolicated backtrace on the VPS.

### M7 — Full state acceptance run · ~1 session · **hardware, ~20 min of your attention**
Walk the whole matrix with `tools/scenario`, confirming each screen against its PNG reference: unplug Wi-Fi · wrong token · bridge stopped · upstream 500 · stale 20 min · stale 2 h · future timestamp · out-of-range value · malformed JSON · phone offline for an hour · VPS reboot mid-poll · device power-cut mid-OTA.
- **DoD:** every row observed and matching; a signed-off checklist committed to `docs/`.

### M8 — Soak · 72 h unattended · **no human action**
Watch heap watermark, reconnect count, HTTP error rate, upstream login count, and resets. Target: **zero unexplained resets, stable minimum free heap, no downward memory trend.**
- **DoD:** 72 h of telemetry graphed and committed.

### M9 — Polish · ~1 session
Night dimming, jitter, optional BH1750 auto-brightness, RUNBOOK, backup/restore of bridge config, one-command redeploy. Enclosure work happens in parallel and is outside this plan.

---

## 10. Testing matrix

| layer | tool | runs where | gate |
|---|---|---|---|
| `bridge/src/domain` | vitest | VPS CI | every commit |
| bridge against mock upstream | vitest + mock-upstream | VPS CI | every commit |
| contract | schema validation of every fixture, both directions | VPS CI | every commit |
| secret leakage | test asserting logger output never contains token / password / JWT / glucose | VPS CI | every commit |
| `glucose_core` | CTest / Unity, native gcc | VPS CI | every commit |
| UI states | `uisim` PNG diff against committed references | VPS CI | every UI commit |
| boot smoke | Wokwi CI (optional) | VPS CI | pre-flash |
| on-device | M7 checklist | the desk | per release |
| longevity | M8 soak | the desk | per release |

**Property-based tests worth the effort** (fast-check on the bridge, hand-rolled on `glucose_core`):
- For any input reading, the published `timestamp` is either absent or equals `FactoryTimestamp` — never `now()`.
- For any sequence of poll results, `age_s` is monotonically non-decreasing between successful updates.
- For any state where `age_s > thresh_stale_s`, the render function never emits the large-number layout.

That third property is the §1 design rule expressed as executable code. It should be the single most-run test in the repo.

---

## 11. Security and privacy checklist

- [ ] Abbott credentials live only on the VPS, only in a `0600` file or a systemd credential; never in the repo, the unit file, the image, `ps`, or logs.
- [ ] Device tokens stored hashed; two-slot rotation; constant-time compare; independently revocable.
- [ ] Redacting logger: `Authorization`, `password`, `token`, JWT-shaped strings, and **glucose values** stripped by default. A `DIAG=1` mode may log values, is off by default, and self-disables after 1 hour.
- [ ] No health data written to disk on the VPS (D9). No database. Bridge container runs `read_only: true` with only a tmpfs `/tmp`, so this is enforced by the runtime, not just by the code. Confirm nothing lands in the Docker log driver either.
- [ ] Container hardening: non-root user, `cap_drop: ALL`, `no-new-privileges`, memory limit, base image pinned by digest, log rotation capped.
- [ ] `bridge.env` lives at `/etc/libre-desk/`, `0600`, root-owned, **outside the repo tree**.
- [ ] Rate limits on `/api/v1/latest`, `/telemetry`, `/coredump`, `/ota`.
- [ ] Caddy: HTTPS only, HSTS, no directory listing, server header stripped.
- [ ] Device: TLS certificate verification **on** (`esp_crt_bundle`); no `skip_cert_common_name_check`; no plaintext HTTP fallback anywhere in the code, not even behind a flag.
- [ ] No inbound ports on the home router.
- [ ] Firmware binaries served over HTTPS with SHA-256 verification. Consider IDF secure boot only if the device ever leaves the house.
- [ ] gitleaks in CI and as a pre-commit hook.
- [ ] Telemetry contains no glucose values, ever.

---

## 12. Observability and runbook

**Bridge metrics:** `upstream_poll_total{result}`, `upstream_login_total`, `reading_rejected_total{reason}`, `reading_age_seconds` (gauge), `device_requests_total{status}`, `cache_hit_ratio`.

**Device telemetry:** firmware version, uptime, min free heap, RSSI, Wi-Fi reconnects, HTTP errors by class, last reset reason.

The single most useful alert: **`reading_age_seconds > 3600` for 15 minutes.** That one alert covers the whole chain — phone offline, sensor ended, Abbott changed the API, bridge down.

`docs/RUNBOOK.md` must answer each of these in under a minute:
- Display says `AUTH` → rotate the device token.
- Display says `SRV` → check `/healthz`, then `docker compose logs --tail=200 bridge`.
- `/healthz` reports `upstream_ok: false` → check for a terms-of-use prompt in the LibreLinkUp app; check whether `LLU_APP_VERSION` needs bumping; check the detected region in the startup log.
- **The number lost its colour but is still there** → `range: "unknown"`; the bridge could not read `targetLow`/`targetHigh`. Check the connection object; the display is still correct, just uncoloured.
- Everything looks fine but the data is old → the **phone** is the usual culprit. The chain can never be fresher than the phone's last upload.
- Device appears bricked → hold BOOT for 5 s to factory reset; if that fails, re-flash via `/flash`.
- Exact commands to rotate the device token, rotate the Libre follower password, and redeploy the bridge.

---

## 13. Risk register

| # | risk | likelihood | impact | mitigation |
|---|---|---|---|---|
| R1 | Abbott changes headers or bumps the required app version | **high** (roughly annual) | display dies | `version` / `product` as env vars; one isolated file; `/healthz` alert; Appendix B references to diff against |
| R2 | Follower account in the wrong region | medium | nothing works at M3 | verified at M0 in the app, before any code |
| R3 | ST7789V2 bring-up fights back under IDF | medium | invalidates D1 | software path VPS-built at M1.5; **physical retirement pending** browser-flash, colour, orientation, and cold-boot acceptance on the exact board |
| R4 | 4 MB flash too tight for dual OTA plus LVGL | medium | no OTA, so the human returns to the loop | measure binary size at M4 before committing to features; cut UI before cutting OTA |
| R5 | Heap exhaustion (TLS + LVGL + Wi-Fi, no PSRAM) | medium | random reboots | tuned mbedTLS buffers, partial draw buffers, heap watermark telemetry, 72 h soak |
| R6 | Phone offline, so stale data looks like a bug | **high** | confusion | the age display *is* the mitigation; RUNBOOK entry; `/healthz` distinguishes bridge-healthy-but-upstream-stale |
| R7 | A stale value renders as current | low | **the one unacceptable failure** | server-computed age, monotonic ageing on device, executable property test, no boot-time cache restore |
| R8 | Let's Encrypt renewal breaks device TLS | low | display dies silently | root bundle rather than leaf pinning; a renewal falls inside the M8 soak |
| R9 | Agent commits a secret | medium | credential leak | gitleaks pre-commit and CI; secrets never in the repo by construction |
| R10 | A bad OTA bricks the device | low | the cable comes back out | rollback proven deliberately at M6 before OTA is trusted |
| R11 | The single board dies, or its flash wears out during heavy OTA testing | low-medium | **project stops entirely** | now that hardware is on the critical path from M1.5, order a **second board** (~$11). It is the cheapest insurance in the project and also gives you an A/B target for OTA testing |
| R12 | Range hue mistaken for an alarm | medium | false reassurance | stated in the README and in D11; no sound, no flashing; the number simply disappears when stale, which is the opposite of an alarm's behaviour |

---

## 14. Rules of engagement for the VPS agent

1. **Never invent a glucose value.** Not in production code, not in a fallback branch, not in a default parameter, not in a fixture that a production path could read. If you cannot produce a validated reading, produce an error.
2. **Never change the §4 contract after M1 without asking.** Both sides and all fixtures depend on it.
3. **Never commit a secret.** gitleaks runs pre-commit and in CI; do not disable it.
4. **Never push an OTA update unattended until the M6 rollback proof has passed.** After that, OTA is allowed for non-breaking changes; changes to provisioning, partitions, or the boot sequence still need a human on standby.
5. **Never widen scope into the non-goals of §1** (alarms, charts, history, direct BLE) without an explicit request. Range colouring **is** in scope as of D11, but only sourced from the account's own thresholds — never from thresholds you choose.
6. **Every milestone ends with a written DoD checklist plus artifacts**: test output, curl transcripts, PNG contact sheet, telemetry graphs. "It works" is not a report.
7. **Ask closed questions** in the form given in §8.4, batched, with an effort estimate.
8. **Keep `main` deployable.** Feature branches; the bridge on `main` is what is running.
9. **Record every non-obvious decision** as a short ADR in `docs/decisions/`, especially anything that contradicts this plan. This plan is a starting position, not scripture — but deviations must be written down.
10. **When Abbott's API misbehaves, diff against the reference implementations in Appendix B before theorising.** Someone has usually already hit it.

---

## 15. Questions — answered 2026-08-22

| # | question | answer | consequence |
|---|---|---|---|
| Q1 | LibreLinkUp **region** | **auto-detect** | Bridge follows the login redirect and caches the resolved region; logged once at startup (§5.2). No configuration needed, but confirm the detected region appears in the M3 startup log. |
| Q2 | **Units** | **auto** from the account's `GlucoseUnits`, env-overridable | No assumption baked in. mmol/L formatting (one decimal, narrower digits) must be exercised in `uisim` regardless of which one you turn out to use. |
| Q3 | Deployment | **Docker Compose** | §5.5 rewritten: two containers, read-only bridge rootfs, secrets in `/etc/libre-desk/bridge.env` outside the repo, Caddy owns 80/443. Agent must check those ports are free before claiming them. |
| Q4 | Hostname / existing services on 80/443 | *still open* | Low risk. Agent asks at M1 and proceeds with `libre.<yourdomain>` as a placeholder in config. |
| Q5 | Derived `rising_fast` / `falling_fast` | **no** | Enum values reserved and unused (D5). |
| Q6 | Glucose-**range** colouring | **yes** | D11 added. Thresholds read from your Libre app's own `targetLow`/`targetHigh`, not invented. Hue = range, brightness = freshness, stale = no number at all. |
| Q7 | Board in hand | **yes** | M1.5 display spike moved to the front to retire risk R3 immediately. A **second board** is now worth ordering (R11). |
| Q8 | Repo hosting | *still open* | Agent defaults to a private GitHub repo with CI on the VPS; say the word if you want local-only git. |
| Q9 | Second consumer later | assume yes | Contract stays generic; two token slots exist anyway for rotation. Costs nothing. |
| Q10 | Show sensor remaining life | **no** in v1 | Reconsider after you have lived with the screen for a week. |

**Confirmed before M2:** `URGENT_LOW` is **70 mg/dL / 3.9 mmol/L**. Every other range threshold comes from your Libre app; this one does not exist there, so it is the single clinical-ish configured value in the system.

---

## Appendix A — handoff prompt for the VPS agent

> You are implementing the project specified in `PLAN.md` in this repository. Read it fully before writing code.
>
> Key facts about your situation: you are headless. You cannot see the display, press buttons, or connect a cable. the device owner is your only physical actuator, and his time is the scarcest resource in the project. Structure everything so that you can prove correctness on this VPS and so that his involvement is limited to: one browser flash, occasional glances at the screen, and answering closed questions.
>
> Work milestone by milestone (§9). At the end of each, produce the DoD checklist with artifacts and stop for review. Follow the rules of engagement in §14 exactly; item 1 (never invent a glucose value) and item 2 (never change the contract unilaterally) are absolute.
>
> §15 is answered; the answers are already folded into the body of the plan. Two items remain open and both have safe defaults: the hostname (Q4) and repo hosting (Q8). The confirmed `URGENT_LOW` threshold is 70 mg/dL / 3.9 mmol/L.
>
> The board is already in hand, so the order is **M0 → M1 → M1.5 (display spike) → M2 → M3 → M4 …**. Run the spike early and on its timebox; it is the only thing that can invalidate a foundational decision, and it costs one session to find out.
>
> Begin with M0 verification and M1.

---

## Appendix B — reference implementations to read (not to depend on)

| project | language | what it is good for |
|---|---|---|
| `timoschlueter/nightscout-librelink-up` | TypeScript | the most actively maintained region list, the `version` header value, and the `account-id` requirement; check its commit log first whenever the API breaks |
| `robberwick/pylibrelinkup` | Python | cleanest model of the region-redirect flow and the `sha256(user.id)` account-id derivation |
| `DiaKEM/libre-link-up-api-client` | TypeScript | compact reference for the login and graph shapes |
| `khskekec` LibreLinkUp HTTP dump (gist) | — | raw request/response captures, useful when a field's meaning is unclear |
| `InventivetalentDev/LibreViewApi` | — | documents the older `lsl/api/...` endpoints; historical context only, superseded by `/llu/...` |
| `espressif/esp-bsp` + `esp_lvgl_port` | C | the pattern to copy for ST7789 + LVGL 9 on IDF 5.x |
| ESP-IDF `examples/peripherals/lcd/spi_lcd_touch` | C | the canonical `esp_lcd` bring-up sequence |

If the upstream API breaks, the diagnostic order is: (1) check `nightscout-librelink-up` commits and issues, (2) bump `LLU_APP_VERSION`, (3) capture the failing request/response into a new fixture, (4) fix `src/upstream/`, (5) add the scenario to `mock-upstream` so it can never silently regress.

---

## Appendix C — model selection for the implementing agent

Prices are per million tokens; coding/agentic figures are the Artificial Analysis indices as listed on OpenRouter on 2026-08-22.

| model | in / out | cache read | coding | agentic | verdict |
|---|---|---|---|---|---|
| **`gpt-5.6-sol`** | $2 / $10 | $0.20 | **77.4** | **57.8** | **primary driver** |
| `gpt-5.6-sol-pro` | $2 / $10 | $0.20 | 77.4 | 57.8 | same price, `reasoning.mode=pro`; escalation only |
| `gpt-5.6-terra` | $2 / $12 | $0.20 | 76.7 | 50.2 | dominated by sol — costs more, lower agentic |
| **`gpt-5.6-luna`** | $0.20 / $1.20 | $0.02 | 71.4 | 46.9 | **bulk worker: 1/10 the price for ~6 points of coding** |
| `gpt-5.5` | $5 / $30 | $0.50 | 74.9 | 47.4 | strictly dominated by sol on price *and* score |
| `gpt-5.4` | $2.50 / $15 | $0.25 | 71.1 | 44.2 | strictly dominated |
| `gpt-5.4-mini` | $0.75 / $4.50 | $0.075 | 56.1 | 31.5 | too weak for embedded C; also dominated by luna |
| `gpt-5.4-nano` | $0.20 / $1.25 | — | — | — | dominated by luna |

**Recommendation — a two-model split:**

- **`gpt-5.6-sol` at reasoning effort `high`** for the work that actually decides whether the project succeeds: the bridge's upstream client and validator, the freshness state machine, the firmware task architecture, display bring-up, and any debugging session. It is the best coding *and* best agentic score in the lineup, at a fifth of GPT-5.5's output price, and OpenAI describes it as tuned for command-line and multi-step coding work — which is exactly the shape of this project.
- **`gpt-5.6-luna` at effort `medium`** for the high-volume, low-judgement mass: writing fixtures, scenario definitions, tests from a spec that already exists, docs, RUNBOOK entries, commit messages, log triage, and mechanical refactors. At $0.20/$1.20 it is roughly a tenth of sol's cost for six points of coding index, and none of those tasks live near the six points.
- **`gpt-5.6-sol-pro`** only for genuinely stuck moments — ST7789V2 not initialising, heap corruption, a TLS handshake that fails only on the device. Same per-token price as sol, so the extra cost is purely the additional reasoning tokens it spends. Escalate to it; do not sit on it.

**Do not use** `gpt-5.5` or `gpt-5.4` for this: sol beats both on the coding and agentic indices while costing less. They are on the frontier of nothing here.

**Operational notes that matter more than the model choice:**
- Turn on **prompt caching** and keep the repo context stable across turns. Sol's cache read is $0.20/M — 10% of the input price. On a long agentic session, this is the difference between a $10 day and a $60 day.
- Set effort per task rather than globally. `low` for mechanical edits even on sol; `high` only where the reasoning earns it.
- Give the agent a **cheap inner loop**: `make test` on the VPS must run in seconds. A fast test loop saves more tokens than any model choice, because it replaces reasoning with evidence.

**Rough lifetime budget:** on the order of $50–150 in API cost for the whole project if run entirely on sol, roughly 40% less with the sol/luna split. That assumes ~2M output tokens and ~12M input tokens with a high cache-hit rate — a guess with wide error bars, but the right order of magnitude, and small enough that it should not drive any design decision here.
