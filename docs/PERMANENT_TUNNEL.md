# Permanent bridge and Cloudflare tunnel

The current MVP endpoint uses an account-less `trycloudflare.com` Quick Tunnel. It cannot be kept alive indefinitely:

- the hostname is temporary;
- Cloudflare explicitly provides no uptime guarantee for Quick Tunnels;
- the connector URL may change after a restart; and
- the firmware currently embeds that hostname, so a changed URL requires reflashing.

For ongoing use, replace it with a **named Cloudflare Tunnel** attached to a hostname in a Cloudflare-managed domain, and run both the bridge and `cloudflared` as restart-on-boot services.

No Internet service is literally immortal. A named tunnel gives stable identity and automatic reconnection; systemd restarts local processes. VPS, Cloudflare, DNS, power, and the LibreLinkUp API remain external dependencies. For higher availability, add a second connector on another host after the single-host system is stable.

Official Cloudflare references:

- [Create a locally managed tunnel](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/local-management/create-local-tunnel/)
- [Run cloudflared as a Linux service](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/local-management/as-a-service/linux/)

## Target topology

```text
ESP32-C6
   |
   | HTTPS https://REPLACE_WITH_STABLE_HOSTNAME/api/v2/latest
   v
Cloudflare edge + stable DNS
   |
   | named outbound tunnel
   v
cloudflared system service on VPS
   |
   | http://127.0.0.1:18080
   v
Libre Desk bridge system service
   |
   v
LibreLinkUp follower API
```

The bridge remains bound to loopback. No inbound VPS port needs to be opened for the API.

## Prerequisites

- a domain managed in the user's Cloudflare account;
- permission to create DNS records and Cloudflare Tunnels;
- a Linux host with systemd and outbound HTTPS access;
- `cloudflared` installed;
- Node.js 22 and a built Libre Desk checkout;
- `/etc/libre-desk/bridge.env` containing the validated production environment; and
- a chosen real hostname in a Cloudflare-managed DNS zone.

`REPLACE_WITH_STABLE_HOSTNAME` is an intentionally unusable sentinel. Replace every occurrence with that real hostname before running a command or installing a configuration file.

The Cloudflare login/domain selection is an interactive account action. Do not send `cert.pem`, tunnel credential JSON, Libre credentials, or raw device tokens through chat or commit them to Git.

## 1. Install the bridge checkout

Example layout:

```sh
sudo useradd --system --home /nonexistent --shell /usr/sbin/nologin libre-desk 2>/dev/null || true
sudo install -d -m 0755 -o root -g root /opt/libre-desk
sudo install -d -m 0700 -o root -g root /etc/libre-desk
sudo install -d -m 0755 -o root -g root /srv/firmware
```

Clone or copy this repository into `/opt/libre-desk`, then build it as an administrative deployment user:

```sh
cd /opt/libre-desk
npm ci
npm run typecheck
npm test
npm run build
```

Do not run `npm install` from the restricted service account. Keep source and `dist/` owned by root or a deployment account and non-writable by `libre-desk`.

Install the private environment file:

```sh
sudo install -m 0600 -o root -g root /absolute/private/path/bridge.env /etc/libre-desk/bridge.env
```

At minimum, set the values documented in `.env.example`. For the physically accepted low-latency MVP, the bridge used `POLL_INTERVAL_S=20`; this performs three upstream cycles per minute and approximately 8,640 Abbott HTTP requests per day because each cycle calls both connections and graph endpoints. Use `60` for lower traffic if the extra cache latency is acceptable. Failure backoff remains enabled either way.

## 2. Install the bridge systemd service

Review `deploy/systemd/libre-desk-bridge.service`. Adjust `WorkingDirectory`, `ExecStart`, or the Node path if your checkout differs, then install it:

```sh
sudo install -m 0644 deploy/systemd/libre-desk-bridge.service \
  /etc/systemd/system/libre-desk-bridge.service
sudo systemctl daemon-reload
sudo systemctl enable --now libre-desk-bridge.service
sudo systemctl status libre-desk-bridge.service
```

Verify loopback liveness:

```sh
curl --fail --silent --show-error http://127.0.0.1:18080/healthz
```

The service uses `Restart=always`. A process crash or clean exit is restarted after five seconds. This does not replace application-level monitoring; inspect aggregate metrics and systemd status without exposing readings or credentials.

## 3. Authenticate cloudflared

Run as the administrator who will create the tunnel:

```sh
cloudflared tunnel login
```

Cloudflare opens a browser authorization flow and asks you to select the domain. The command creates an account certificate in the administrator's default `.cloudflared` directory. Keep it private.

## 4. Create a named tunnel and DNS route

```sh
cloudflared tunnel create libre-desk
cloudflared tunnel list
cloudflared tunnel route dns libre-desk REPLACE_WITH_STABLE_HOSTNAME
```

Record the generated tunnel UUID locally. The create command also writes a `<UUID>.json` tunnel credential. This credential authorizes that tunnel and must never be committed.

## 5. Install tunnel credentials and configuration

```sh
sudo install -d -m 0700 -o root -g root /etc/cloudflared
sudo install -m 0600 -o root -g root \
  "$HOME/.cloudflared/<TUNNEL-UUID>.json" \
  "/etc/cloudflared/<TUNNEL-UUID>.json"
sudo cp deploy/cloudflared/config.yml.example /etc/cloudflared/config.yml
sudo chmod 0600 /etc/cloudflared/config.yml
sudoedit /etc/cloudflared/config.yml
```

Replace every placeholder in `/etc/cloudflared/config.yml`:

```yaml
tunnel: <TUNNEL-UUID>
credentials-file: /etc/cloudflared/<TUNNEL-UUID>.json

ingress:
  - hostname: REPLACE_WITH_STABLE_HOSTNAME
    service: http://127.0.0.1:18080
  - service: http_status:404
```

The final catch-all rule prevents accidental publication of another local service.

Validate before installation:

```sh
cloudflared tunnel --config /etc/cloudflared/config.yml ingress validate
cloudflared tunnel --config /etc/cloudflared/config.yml run libre-desk
```

Stop the foreground check after the public hostname works.

## 6. Install cloudflared as a service

Cloudflare's current Linux instructions support installing from an explicit config path:

```sh
sudo cloudflared --config /etc/cloudflared/config.yml service install
sudo systemctl enable --now cloudflared.service
sudo systemctl status cloudflared.service
```

Useful checks:

```sh
cloudflared tunnel info libre-desk
systemctl is-enabled libre-desk-bridge.service cloudflared.service
systemctl is-active libre-desk-bridge.service cloudflared.service
```

Do not run a second manual `cloudflared tunnel run` while the service owns the connector unless intentionally adding another high-availability connector.

### Alternative: Docker Compose when systemd is unavailable

Use this only on a host whose Docker daemon itself starts at boot. The standalone `deploy/docker-compose.tunnel.yml` stack runs the bridge and named connector with `restart: unless-stopped`, exposes no host ports, and pins the multi-architecture Cloudflare image by digest.

Create a Docker-specific bridge environment from the private file generated in `docs/SECOND_DEVICE.md`. It must use the container network rather than loopback:

```text
HOST=0.0.0.0
PORT=8080
```

Install it owner-only:

```sh
sudo install -m 0600 -o root -g root \
  /absolute/private/path/bridge.docker.env \
  /etc/libre-desk/bridge.docker.env
```

Create a named-tunnel token file without printing the token:

```sh
umask 077
TUNNEL_TOKEN="$(cloudflared tunnel token libre-desk)"
printf 'TUNNEL_TOKEN=%s\nTUNNEL_URL=http://bridge:8080\nTUNNEL_LOGLEVEL=info\nNO_AUTOUPDATE=true\n' \
  "$TUNNEL_TOKEN" > "$HOME/cloudflared.env"
unset TUNNEL_TOKEN
sudo install -m 0600 -o root -g root "$HOME/cloudflared.env" \
  /etc/libre-desk/cloudflared.env
rm -f "$HOME/cloudflared.env"
```

Start and inspect the stack:

```sh
cd /opt/libre-desk
sudo docker compose -f deploy/docker-compose.tunnel.yml up -d --build
sudo docker compose -f deploy/docker-compose.tunnel.yml ps
```

The connector reads its token from the owner-only env file, not from process arguments. Anyone with root or Docker-socket access can still inspect container environment values; protect that access as equivalent to root. Do not run this stack and the systemd services simultaneously unless intentionally operating separate high-availability connectors. Verify the stable public hostname exactly as below.

## 7. Verify the stable public path

Liveness is intentionally unauthenticated:

```sh
curl --fail --silent --show-error https://REPLACE_WITH_STABLE_HOSTNAME/healthz
```

Run the repository smoke test with a raw device token supplied only to that process:

```sh
BASE_URL=https://REPLACE_WITH_STABLE_HOSTNAME \
DEVICE_TOKEN="$DEVICE_TOKEN" \
/opt/libre-desk/scripts/smoke.sh
```

The smoke script must pass without printing the token or glucose value. Also verify:

- an absent token receives `401`;
- an inactive canonical token receives `403`;
- the real device receives `200` and continues replacing readings; and
- system logs contain aggregate events only.

## 8. Migrate the existing ESP32

Do not stop the Quick Tunnel until the board has been migrated:

1. keep the old bridge endpoint running;
2. prove the named hostname locally and from an independent network;
3. change only `MVP_BRIDGE_ORIGIN` in ignored `firmware/main/mvp_secrets.h`;
4. run the focused firmware tests and clean ESP-IDF build;
5. flash the new private package;
6. observe two successive readings through the named hostname; and
7. only then stop and remove the Quick Tunnel.

Because the current MVP has no finished OTA/provisioning flow, this hostname migration requires one physical browser reflash.

## 9. Reboot and recovery acceptance

Reboot the VPS during a controlled window:

```sh
sudo reboot
```

After it returns, verify:

```sh
systemctl is-active libre-desk-bridge.service cloudflared.service
curl --fail --silent --show-error https://REPLACE_WITH_STABLE_HOSTNAME/healthz
```

Then confirm the physical display automatically recovers without reflashing. A one-cycle `SRV` state can still occur during an origin restart because the current firmware fails closed on a single transport failure; retaining and aging the last valid reading through isolated failures is deferred hardening.

## 10. Operations and upgrades

For a bridge update:

```sh
cd /opt/libre-desk
git fetch --all --prune
git checkout <reviewed-commit-or-tag>
npm ci
npm run typecheck
npm test
npm run build
sudo systemctl restart libre-desk-bridge.service
```

Verify health immediately after restart. Use reviewed commits or signed tags rather than deploying an arbitrary moving branch.

For additional resilience:

- create a second named-tunnel connector on another host using the same tunnel token or managed-tunnel installation flow;
- keep bridge secrets independent per host;
- monitor systemd service state, public `/healthz`, and aggregate failure counters;
- configure host security updates and disk/process monitoring; and
- replace build-time device credentials with the planned secure provisioning flow.
