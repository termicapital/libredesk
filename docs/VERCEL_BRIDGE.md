# Stable Vercel bridge

Deploy your own Vercel project and use its HTTPS origin. The clock-aware device route is:

```text
https://bridge.example.invalid/api/v2/latest
```

`bridge.example.invalid` is a deliberately unusable placeholder; replace it everywhere with your own deployment hostname. `/api/v1/latest` remains a frozen 12-field schema-1 compatibility endpoint for already-flashed firmware. In this deployment model the Vercel project does **not** authenticate directly to LibreLinkUp: your trusted host remains the upstream client.

## Architecture

```text
LibreLinkUp follower API
        |
        | authenticated polling from the trusted VPS only
        v
local Libre Desk bridge `/api/v2/latest` (loopback HTTP)
        |
        | authenticated HTTPS push once per minute
        v
/api/v1/ingest on Vercel
        |
        | strict validation + atomic monotonic write
        v
your private Upstash Redis (24-hour TTL)
        |
        | strict validation + server-derived freshness
        v
/api/v2/latest on the stable vercel.app alias
        |
        v
ESP32-C6 display
```

Keep a verified local or Cloudflare path available while validating your Vercel installation.

## Trust boundaries

Use two unrelated canonical 256-bit base64url tokens:

- the **device token** can read `/api/v1/latest` and `/api/v2/latest` but cannot ingest;
- the **ingestion token** can write `/api/v1/ingest` but cannot read.

Vercel stores only SHA-256 token digests. Raw tokens remain in the device provisioning data and an owner-only pusher configuration on the trusted VPS. LibreLinkUp credentials must not be configured in Vercel.

The ingestion handler authenticates before body validation or Redis work. The read handler authenticates before fetching Redis. Both boundaries strictly validate the complete normalized schema. Ingestion accepts schema 2 only. During migration, the read handler also accepts an exact legacy schema-1 Redis record for `/api/v1/latest`; `/api/v2/latest` fails closed for that record because local wall time must never be invented.

## Vercel configuration

Required production variables:

```text
DEVICE_TOKEN_HASHES=<one-to-four SHA-256 hex digests>
INGEST_TOKEN_HASHES=<one-to-four distinct SHA-256 hex digests>
KV_REST_API_URL=<Upstash HTTPS REST URL>
KV_REST_API_TOKEN=<Upstash REST token>
MAX_AGE_S=3600
POLL_AFTER_S=60
```

The production functions run in `fra1`. The project publishes only `public/` plus the four functions described by `vercel.json`.

Security controls:

- `/api/v1/latest` and `/api/v2/latest`: Vercel Firewall fixed-window limit of 120 requests/hour per complete authorization header;
- `/api/v1/ingest`: Redis-backed atomic fixed-window limit of 120 requests/hour per authenticated ingestion token, plus an in-process burst limit as defense in depth;
- Redis latest-reading and identity keys expire after 24 hours;
- responses use `Cache-Control: no-store`;
- malformed, absent, stale, storage-failed, and limiter-failed states fail closed without a partial reading.

## Atomic latest-reading behavior

One Lua transaction updates three Redis keys:

1. normalized payload;
2. source timestamp in epoch milliseconds;
3. SHA-256 stable measurement identity.

The stable identity includes glucose, unit, trend, range, targets, source timestamp, LibreLinkUp local measurement wall time, source, and schema. It excludes volatile `age_s`, `server_time`, and poll metadata.

Outcomes:

- newer source timestamp: `stored`;
- equal timestamp and equal stable identity: `duplicate`, with all three keys rewritten to repair a missing payload and refresh TTLs;
- older timestamp: rejected;
- equal timestamp and different or missing stable identity: rejected as conflict;
- missing timestamp with any residual payload or identity key: rejected as conflict;
- legacy two-key record: an equal timestamp conflicts because identity is unknown; only a genuinely newer source timestamp upgrades it into the three-key representation.

## Trusted-host sender

Build before running the compiled sender:

```sh
npm ci
npm run build
```

Create an external JSON file with exactly these keys:

```json
{
  "device_token": "REPLACE_WITH_LOCAL_READ_TOKEN",
  "ingest_token": "REPLACE_WITH_DISTINCT_INGEST_TOKEN",
  "ingest_url": "https://bridge.example.invalid/api/v1/ingest"
}
```

The file must be a regular file owned by the running user, mode `0600`, no symlink, and at most 4096 bytes. The sender opens it with `O_NOFOLLOW`, validates both tokens and the exact ingestion URL, reads schema 2 from local loopback `/api/v2/latest`, and validates both the reading and Vercel acknowledgement.

## Compatibility-safe schema-2 rollout

1. Pause the scheduled VPS pusher.
2. Deploy the dual-read Vercel bridge. Existing schema-1 Redis data remains available through `/api/v1/latest`; v2 remains unavailable rather than guessing local time.
3. Upgrade the local bridge and pusher so the sender reads local `/api/v2/latest` and writes strict schema 2 to `/api/v1/ingest`.
4. Wait for a genuinely newer Libre source timestamp, then push it. An equal timestamp with a different schema/local-time identity is intentionally rejected as a conflict.
5. Verify both remote routes, scheduled propagation, and the dual-route firewall rule before publishing schema-2 firmware.

This order preserves already-flashed schema-1 firmware throughout the migration.

Run one push:

```sh
LIBRE_DESK_VERCEL_PUSH_CONFIG=/absolute/private/vercel-push.json \
  tools/push-latest-vercel/run.sh
```

A successful run is silent and exits zero. A failure exits nonzero and prints only a generic category; it never prints tokens, responses, or glucose values.

Schedule that one-shot command once per minute using the host's supervised scheduler. Immediately execute the scheduler-managed job once and verify its recorded exit status rather than relying only on a manual shell run.

## Remote verification

Verify in this order:

1. root and `/healthz` return HTTP 200;
2. missing device token returns 401;
3. canonical but inactive device token returns 403;
4. valid device token returns frozen schema 1 without `libre_local_time` on v1 and strict schema 2 with `libre_local_time` on v2;
5. missing/wrong ingestion credentials return 401/403 without a write;
6. first real push returns `stored` and replay returns `duplicate`;
7. changed age/server time at the same source timestamp remains `duplicate`;
8. at least two genuinely newer source timestamps propagate through scheduled runs;
9. local v2 and remote v2 stable measurement fields match immediately after scheduled pushes;
10. a real v2 `If-None-Match` request still returns HTTP 200 with a complete body.

Vercel injects a response ETag. Because `server_time` is regenerated, that provider ETag changes on each successful read. The application intentionally does not emit a fixed ETag: Vercel strips custom freshness headers from 304 responses, while the current firmware requires a full response body.

## Rollback

Do not delete the trusted local bridge or the known-good Cloudflare firmware configuration during initial physical acceptance. If the Vercel endpoint, scheduler, store, or display fails, reflash the previously accepted Cloudflare-origin package while diagnosing the Vercel path.
