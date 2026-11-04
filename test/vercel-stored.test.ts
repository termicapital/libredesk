import { createHash } from "node:crypto";
import { describe, expect, it } from "vitest";
import {
  createStoredBridge,
  type ReadingStore,
  type SharedRateLimiter,
  type StoreWriteResult,
} from "../bridge/src/vercel/stored.js";

const DEVICE_TOKEN = Buffer.alloc(32, 3).toString("base64url");
const INGEST_TOKEN = Buffer.alloc(32, 4).toString("base64url");
const DEVICE_HASH = createHash("sha256").update(DEVICE_TOKEN).digest("hex");
const INGEST_HASH = createHash("sha256").update(INGEST_TOKEN).digest("hex");
const NOW = new Date("2026-01-01T00:01:00.000Z");

function reading(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    glucose: 120,
    unit: "mg/dL",
    trend: "stable",
    range: "in_range",
    target_low: 70,
    target_high: 180,
    timestamp: "2026-01-01T00:00:00.000Z",
    libre_local_time: "00:00:00",
    age_s: 0,
    server_time: "2026-01-01T00:00:00.000Z",
    source: "librelinkup",
    poll_after_s: 20,
    schema: 2,
    ...overrides,
  };
}

function legacyReading(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  const { libre_local_time: _omitted, ...legacy } = reading();
  return { ...legacy, schema: 1, ...overrides };
}

class FakeStore implements ReadingStore {
  reads = 0;
  writes = 0;
  current: unknown;
  nextWrite: StoreWriteResult = "stored";
  throwOnRead = false;
  throwOnWrite = false;
  written: unknown;

  async get(): Promise<unknown | undefined> {
    this.reads += 1;
    if (this.throwOnRead) throw new Error("private storage detail");
    return this.current;
  }

  async putIfNewer(value: unknown): Promise<StoreWriteResult> {
    this.writes += 1;
    if (this.throwOnWrite) throw new Error("private storage detail");
    this.written = value;
    return this.nextWrite;
  }
}

class FakeSharedLimiter implements SharedRateLimiter {
  checks: Array<{ tokenHash: string; nowMs: number }> = [];
  decision = { allowed: true, retryAfterS: 0 };
  throwOnCheck = false;

  async check(tokenHash: string, nowMs: number) {
    this.checks.push({ tokenHash, nowMs });
    if (this.throwOnCheck) throw new Error("private rate store detail");
    return this.decision;
  }
}

function bridge(store: FakeStore, ingestSharedRateLimiter?: SharedRateLimiter) {
  return createStoredBridge({
    store,
    deviceTokenHashes: [DEVICE_HASH],
    ingestTokenHashes: [INGEST_HASH],
    clock: () => NOW,
    requestId: () => "request-1",
    maxAgeS: 3_600,
    pollAfterS: 60,
    version: "0.3.0-vercel",
    ...(ingestSharedRateLimiter === undefined ? {} : { ingestSharedRateLimiter }),
  });
}

describe("stored Vercel bridge", () => {
  it("authenticates the display before reading storage", async () => {
    const store = new FakeStore();
    store.current = reading();

    const missing = await bridge(store).latest({ method: "GET", headers: {} });
    const wrong = await bridge(store).latest({
      method: "GET",
      headers: { authorization: `Bearer ${Buffer.alloc(32, 8).toString("base64url")}` },
    });

    expect(missing.statusCode).toBe(401);
    expect(wrong.statusCode).toBe(403);
    expect(store.reads).toBe(0);
  });

  it("returns a schema-stable reading with server-derived age", async () => {
    const store = new FakeStore();
    store.current = reading();

    const response = await bridge(store).latest({
      method: "GET",
      headers: { authorization: `Bearer ${DEVICE_TOKEN}` },
    });

    expect(response.statusCode).toBe(200);
    expect(response.headers).toEqual(expect.objectContaining({
      "cache-control": "no-store",
      "x-request-id": "request-1",
    }));
    expect(response.headers.etag).toBeUndefined();
    const v2 = await bridge(store).latest({
      method: "GET",
      headers: { authorization: "Bear" + "er " + DEVICE_TOKEN },
    }, 2);
    expect(response.body).toEqual(expect.objectContaining({
      age_s: 60,
      server_time: NOW.toISOString(),
      poll_after_s: 60,
      schema: 1,
    }));
    expect(response.body).not.toEqual(expect.objectContaining({ libre_local_time: expect.anything() }));
    expect(v2.statusCode).toBe(200);
    expect(v2.body).toEqual(expect.objectContaining({
      libre_local_time: "00:00:00",
      age_s: 60,
      server_time: NOW.toISOString(),
      poll_after_s: 60,
      schema: 2,
    }));
  });

  it("keeps v1 available for a legacy stored record while v2 fails closed", async () => {
    const store = new FakeStore();
    store.current = legacyReading();
    const instance = bridge(store);

    const v1 = await instance.latest({
      method: "GET",
      headers: { authorization: "Bear" + "er " + DEVICE_TOKEN },
    });
    const v2 = await instance.latest({
      method: "GET",
      headers: { authorization: "Bear" + "er " + DEVICE_TOKEN },
    }, 2);
    const health = await instance.health();

    expect(v1.statusCode).toBe(200);
    expect(v1.body).toEqual(expect.objectContaining({
      age_s: 60,
      server_time: NOW.toISOString(),
      poll_after_s: 60,
      schema: 1,
    }));
    expect(v1.body).not.toEqual(expect.objectContaining({ libre_local_time: expect.anything() }));
    expect(v2.statusCode).toBe(503);
    expect(v2.body).toEqual(expect.objectContaining({ error: "malformed_upstream" }));
    expect(v2.body).not.toEqual(expect.objectContaining({ glucose: expect.anything() }));
    expect(health.body).toEqual(expect.objectContaining({
      upstream_ok: true,
      last_success_age_s: 60,
    }));
  });

  it("ignores conditional headers and always returns a fresh 200 body", async () => {
    const store = new FakeStore();
    store.current = reading();
    const instance = bridge(store);
    const first = await instance.latest({
      method: "GET",
      headers: { authorization: `Bearer ${DEVICE_TOKEN}` },
    });
    const second = await instance.latest({
      method: "GET",
      headers: {
        authorization: `Bearer ${DEVICE_TOKEN}`,
        "if-none-match": "\"obsolete-client-etag\"",
      },
    });

    expect(second.statusCode).toBe(200);
    expect(second.body).toEqual(expect.objectContaining({
      age_s: 60,
      server_time: NOW.toISOString(),
      poll_after_s: 60,
    }));
    expect(first.headers.etag).toBeUndefined();
    expect(second.headers.etag).toBeUndefined();
  });

  it("fails closed for stale, malformed, absent, and unavailable storage", async () => {
    const stale = new FakeStore();
    stale.current = reading({ timestamp: "2025-12-31T22:00:00.000Z" });
    const malformed = new FakeStore();
    malformed.current = { glucose: "120" };
    const absent = new FakeStore();
    const unavailable = new FakeStore();
    unavailable.throwOnRead = true;

    for (const [store, error] of [
      [stale, "upstream_unavailable"],
      [malformed, "malformed_upstream"],
      [absent, "upstream_unavailable"],
      [unavailable, "upstream_unavailable"],
    ] as const) {
      const response = await bridge(store).latest({
        method: "GET",
        headers: { authorization: `Bearer ${DEVICE_TOKEN}` },
      });
      expect(response.statusCode).toBe(503);
      expect(response.body).toEqual(expect.objectContaining({ error }));
      expect(response.body).not.toEqual(expect.objectContaining({ glucose: expect.anything() }));
      expect(JSON.stringify(response.body)).not.toContain("private storage detail");
    }
  });

  it("authenticates ingestion before validating or writing", async () => {
    const store = new FakeStore();
    const limiter = new FakeSharedLimiter();

    const missing = await bridge(store, limiter).ingest({ method: "POST", headers: {}, body: reading() });
    const deviceCredential = await bridge(store, limiter).ingest({
      method: "POST",
      headers: { authorization: "Bear" + "er " + DEVICE_TOKEN },
      body: reading(),
    });

    expect(missing.statusCode).toBe(401);
    expect(deviceCredential.statusCode).toBe(403);
    expect(store.writes).toBe(0);
    expect(limiter.checks).toHaveLength(0);
  });

  it("enforces the shared ingestion limit before payload validation or storage", async () => {
    const store = new FakeStore();
    const limiter = new FakeSharedLimiter();
    limiter.decision = { allowed: false, retryAfterS: 37 };

    const response = await bridge(store, limiter).ingest({
      method: "POST",
      headers: { authorization: "Bear" + "er " + INGEST_TOKEN },
      body: { invalid: true },
    });

    expect(response.statusCode).toBe(429);
    expect(response.headers["retry-after"]).toBe("37");
    expect(response.body).toEqual({
      error: "rate_limited",
      detail: "request rate limit exceeded",
      retry_after_s: 37,
    });
    expect(limiter.checks).toEqual([{ tokenHash: INGEST_HASH, nowMs: NOW.getTime() }]);
    expect(store.writes).toBe(0);
  });

  it("fails closed when the shared ingestion limiter is unavailable", async () => {
    const store = new FakeStore();
    const limiter = new FakeSharedLimiter();
    limiter.throwOnCheck = true;

    const response = await bridge(store, limiter).ingest({
      method: "POST",
      headers: { authorization: "Bear" + "er " + INGEST_TOKEN },
      body: reading(),
    });

    expect(response.statusCode).toBe(503);
    expect(response.body).toEqual({
      error: "rate_limit_unavailable",
      detail: "shared rate limit unavailable",
      retry_after_s: 30,
    });
    expect(JSON.stringify(response.body)).not.toContain("private rate store detail");
    expect(store.writes).toBe(0);
  });

  it("strictly validates and normalizes ingestion before storing", async () => {
    const store = new FakeStore();
    const response = await bridge(store).ingest({
      method: "POST",
      headers: { authorization: `Bearer ${INGEST_TOKEN}` },
      body: reading(),
    });

    expect(response.statusCode).toBe(202);
    expect(response.body).toEqual({ accepted: true, result: "stored" });
    expect(store.writes).toBe(1);
    expect(store.written).toEqual({
      ...reading(),
      age_s: 60,
      server_time: NOW.toISOString(),
      poll_after_s: 60,
    });
  });

  it("rejects invalid bodies without a storage write", async () => {
    const invalidBodies = [
      reading({ glucose: "120" }),
      reading({ source: "mock" }),
      reading({ extra: true }),
      reading({ target_low: 190, target_high: 180 }),
      reading({ timestamp: "2026-01-01T00:10:00.000Z" }),
    ];

    for (const body of invalidBodies) {
      const store = new FakeStore();
      const response = await bridge(store).ingest({
        method: "POST",
        headers: { authorization: `Bearer ${INGEST_TOKEN}` },
        body,
      });
      expect(response.statusCode).toBe(400);
      expect(response.body).toEqual({ error: "invalid_payload", detail: "reading payload rejected", retry_after_s: 0 });
      expect(store.writes).toBe(0);
    }
  });

  it("maps idempotent and monotonic write outcomes without overwriting newer data", async () => {
    for (const [result, statusCode] of [
      ["stored", 202],
      ["duplicate", 202],
      ["older", 409],
      ["conflict", 409],
    ] as const) {
      const store = new FakeStore();
      store.nextWrite = result;
      const response = await bridge(store).ingest({
        method: "POST",
        headers: { authorization: `Bearer ${INGEST_TOKEN}` },
        body: reading(),
      });
      expect(response.statusCode).toBe(statusCode);
      expect(response.body).toEqual(statusCode === 202
        ? { accepted: true, result }
        : { error: "non_monotonic_timestamp", detail: "stored reading is newer or conflicts", retry_after_s: 0 });
    }
  });

  it("contains storage write failures", async () => {
    const store = new FakeStore();
    store.throwOnWrite = true;
    const response = await bridge(store).ingest({
      method: "POST",
      headers: { authorization: `Bearer ${INGEST_TOKEN}` },
      body: reading(),
    });

    expect(response.statusCode).toBe(503);
    expect(response.body).toEqual({ error: "storage_unavailable", detail: "persistent store unavailable", retry_after_s: 30 });
    expect(JSON.stringify(response.body)).not.toContain("private storage detail");
  });

  it("reports store freshness without exposing the reading", async () => {
    const fresh = new FakeStore();
    fresh.current = reading();
    const empty = new FakeStore();

    expect(await bridge(fresh).health()).toEqual(expect.objectContaining({
      statusCode: 200,
      body: {
        ok: true,
        upstream_ok: true,
        last_success_age_s: 60,
        version: "0.3.0-vercel",
        mode: "stored_push",
      },
    }));
    expect(await bridge(empty).health()).toEqual(expect.objectContaining({
      statusCode: 200,
      body: {
        ok: true,
        upstream_ok: false,
        last_success_age_s: null,
        version: "0.3.0-vercel",
        mode: "stored_push",
      },
    }));
  });
});
