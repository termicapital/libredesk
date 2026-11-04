# Provision a second Libre Desk device

This guide reproduces the physically accepted MVP on another **Waveshare ESP32-C6-LCD-1.3**. It deliberately gives every board a different revocable bearer token. Do not clone an existing device's private header or firmware binary.

The official Libre app/reader remains authoritative. This display has no alarm or dosing function.

## 1. Decide the deployment first

The firmware embeds its HTTPS bridge origin. Establish a stable hostname before building a device intended for ongoing use.

Recommended order:

1. deploy the bridge as a restart-on-boot service;
2. create a named Cloudflare tunnel and stable hostname;
3. verify that hostname from an independent client;
4. provision and build the device against that hostname.

See `docs/PERMANENT_TUNNEL.md`. A `trycloudflare.com` Quick Tunnel is suitable only for temporary testing because its URL and availability are not guaranteed.

## 2. Prepare the follower account

Use a dedicated LibreLinkUp follower account, not the primary Libre account. In the official LibreLinkUp app:

1. accept the patient's sharing invitation;
2. accept any current terms or privacy prompt;
3. verify that exactly one patient connection is present; and
4. verify that current readings are visible.

The current bridge intentionally rejects zero or multiple connections rather than guessing which patient to display.

## 3. Clone and install

```sh
git clone https://github.com/termicapital/libredesk.git
cd libredesk
npm ci
npm run typecheck
npm test
npm run build
```

Install the pinned firmware toolchain once:

```sh
./scripts/setup-idf.sh
```

## 4. Generate unique private material

Choose an external private directory and run the helper below from the repository root. It prompts without echoing passwords, creates mode-0600 files, generates a unique device token and installer capability, and writes the ignored firmware header. It prints paths only—never credential values.

```sh
export LIBRE_DESK_SECURE_DIR="$HOME/.config/libre-desk/device-02"
umask 077
python3 - <<'PY'
import getpass
import hashlib
import json
import os
import secrets
from pathlib import Path

secure = Path(os.environ["LIBRE_DESK_SECURE_DIR"]).expanduser()
secure.mkdir(parents=True, exist_ok=True)
email = input("Dedicated LibreLinkUp follower email: ").strip()
libre_password = getpass.getpass("LibreLinkUp password: ")
ssid = input("2.4 GHz Wi-Fi SSID: ")
wifi_password = getpass.getpass("Wi-Fi password: ")
origin = input("Stable HTTPS bridge origin (https://REPLACE_WITH_STABLE_HOSTNAME): ").strip().rstrip("/")
if not origin.startswith("https://"):
    raise SystemExit("bridge origin must start with https://")

device_token = secrets.token_urlsafe(32)
device_hash = hashlib.sha256(device_token.encode("ascii")).hexdigest()
installer_token = secrets.token_urlsafe(32)

def write_private(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    path.chmod(0o600)

def env_quote(value: str) -> str:
    if "\n" in value or "\r" in value:
        raise SystemExit("environment values must not contain newlines")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

write_private(secure / "credentials.json", json.dumps({
    "libre_email": email,
    "libre_password": libre_password,
}, indent=2) + "\n")
write_private(secure / "device.json", json.dumps({
    "device_token": device_token,
    "device_token_sha256": device_hash,
    "bridge_origin": origin,
    "bridge_port": 18080,
    "installer_path_token": installer_token,
}, indent=2) + "\n")
write_private(secure / "bridge.env", "\n".join([
    f"LIBRE_EMAIL={env_quote(email)}",
    f"LIBRE_PASSWORD={env_quote(libre_password)}",
    "LLU_APP_VERSION=5.1.1",
    "LLU_PRODUCT=llu.android",
    "UPSTREAM_BASE_URL=https://api.libreview.io",
    "UPSTREAM_SOURCE=librelinkup",
    f"DEVICE_TOKEN_HASHES={device_hash}",
    "HOST=127.0.0.1",
    "PORT=18080",
    "POLL_INTERVAL_S=20",
    "POLL_JITTER_S=0",
    "MAX_BACKOFF_S=900",
    "MAX_AGE_S=3600",
    "RANGE_URGENT_LOW=70",
    "FIRMWARE_DIR=/srv/firmware",
    "",
]))
header = Path("firmware/main/mvp_secrets.h")
write_private(header, "\n".join([
    "/* Local generated MVP provisioning. Never commit this file. */",
    f"#define MVP_WIFI_SSID {json.dumps(ssid)}",
    f"#define MVP_WIFI_PASSWORD {json.dumps(wifi_password)}",
    f"#define MVP_BRIDGE_ORIGIN {json.dumps(origin)}",
    f"#define MVP_DEVICE_TOKEN {json.dumps(device_token)}",
    "",
]))
print(f"Created private configuration under {secure}")
print(f"Created ignored firmware header at {header}")
PY
```

Verify the publication boundary:

```sh
git check-ignore -v firmware/main/mvp_secrets.h
stat -c '%a %n' "$LIBRE_DESK_SECURE_DIR"/*.json "$LIBRE_DESK_SECURE_DIR/bridge.env
```

Every listed private file must be mode `600`; the header must be ignored. Do not paste any of their contents into chat, issues, CI variables, or logs.

### More than one device

Generate a separate raw token for each device. The bridge environment accepts one or two comma-separated token hashes during MVP rotation:

```text
DEVICE_TOKEN_HASHES=<device-01-sha256>,<device-02-sha256>
```

Never put raw tokens in the bridge environment. The MVP parser rejects more than two active hashes; expand that contract deliberately before operating a larger fleet.

## 5. Start and verify the bridge

For a foreground check using the generated JSON files:

```sh
LIBRE_DESK_SECURE_DIR="$LIBRE_DESK_SECURE_DIR" \
./node_modules/.bin/tsx tools/live-bridge/start.ts
```

`tools/live-bridge/start.ts` is an MVP convenience launcher; it is not the permanent service entry point. For continuous operation, use the generated `bridge.env` and systemd deployment in `docs/PERMANENT_TUNNEL.md` rather than leaving this terminal open.

From another terminal, verify liveness without credentials:

```sh
curl --fail --silent --show-error http://127.0.0.1:18080/healthz
```

Use `scripts/smoke.sh` with the raw device token only in the current process environment. The script verifies the authenticated contract without printing the token or glucose value:

```sh
BASE_URL=http://127.0.0.1:18080 \
DEVICE_TOKEN="$(python3 -c 'import json,os; print(json.load(open(os.environ["LIBRE_DESK_SECURE_DIR"]+"/device.json"))["device_token"])')" \
./scripts/smoke.sh
```

## 6. Build the device-specific firmware

```sh
./scripts/test-mvp-config.sh
./scripts/test-wifi-startup-flow.sh
./scripts/test-mvp-http-flow.sh
./scripts/build-display-spike.sh
```

The build is device-specific because `mvp_secrets.h` is compiled into the application. Generated `.bin` files under `firmware/build/` and `tools/flasher/firmware/` are ignored and must remain private.

Before flashing, verify that Git still contains no private changes:

```sh
git status --short
./scripts/pre-commit-gitleaks.sh
```

The staged scan does not scan ignored binaries; never add them with `git add -f`.

## 7. Flash

If the board and browser are on the same development machine, localhost is accepted as a trustworthy WebSerial origin by Chromium browsers:

```sh
python3 tools/flasher/serve_private.py \
  --root tools/flasher \
  --device-config "$LIBRE_DESK_SECURE_DIR/device.json" \
  --port 18081
```

Read the private path token locally and open:

```text
http://127.0.0.1:18081/install/<installer_path_token>/
```

Use desktop Chrome or Edge, click **Connect and install**, select the ESP32-C6 USB serial device, and wait for installation to finish.

If the builder and browser are on different machines, expose this private installer only through certificate-verified HTTPS and a capability path. Remove it after flashing. Do not reuse the bridge's device token as the installer path token.

## 8. Physical acceptance

Keep the board powered and check:

1. `starting` appears;
2. `Wi-Fi` appears while joining the configured 2.4 GHz network;
3. `Time` appears during SNTP synchronization;
4. `...` appears during the first HTTPS request;
5. a numeric reading, unit, trend, age, and signal indicator appear; and
6. a later real reading replaces the earlier one without reflashing.

Compare privately with the official LibreLinkUp app. Do not publish the values. A number must never be accepted from mock data as physical live-data proof.

## 9. Rotation or decommissioning

To revoke one device:

1. remove its hash from `DEVICE_TOKEN_HASHES` and restart the bridge;
2. delete its private installer and generated binaries;
3. erase or reflash the board before transferring it; and
4. delete the device's external private directory when no longer needed.

Changing Wi-Fi, the raw device token, or the stable bridge hostname currently requires rebuilding and reflashing the MVP firmware. Secure runtime provisioning and OTA are deferred hardening work.
