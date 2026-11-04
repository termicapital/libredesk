import { createHash } from "node:crypto";
import { describe, expect, it } from "vitest";
import {
  VercelStoredConfigError,
  createStoredBridgeFromEnvironment,
} from "../bridge/src/vercel/default.js";
import type { ReadingStore, SharedRateLimiter } from "../bridge/src/vercel/stored.js";

const DEVICE_TOKEN = Buffer.alloc(32, 9).toString("base64url");
const INGEST_TOKEN = Buffer.alloc(32, 10).toString("base64url");
const DEVICE_TOKEN_HASH = createHash("sha256").update(DEVICE_TOKEN, "utf8").digest("hex");
const INGEST_TOKEN_HASH = createHash("sha256").update(INGEST_TOKEN, "utf8").digest("hex");
const NOW = new Date("2026-01-01T00:01:00.000Z");

const ENV = {
  DEVICE_TOKEN_HASHES: DEVICE_TOKEN_HASH,
  INGEST_TOKEN_HASHES: INGEST_TOKEN_HASH,
  KV_REST_API_URL: "https://redis.example.test",
  KV_REST_API_TOKEN: "redis-token",
  MAX_AGE_S: "3600",
  POLL_AFTER_S: "60",
} as const;

class FakeStore implements ReadingStore {
  value: unknown = {
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
  };

  async get(): Promise<unknown> {
    return this.value;
  }

  async putIfNewer(): Promise<"stored"> {
    return "stored";
  }
}

class FakeSharedLimiter implements SharedRateLimiter {
  checks = 0;

  async check() {
    this.checks += 1;
    return { allowed: true, retryAfterS: 0 };
  }
}

describe("stored Vercel bridge environment factory", () => {
  it("fails closed when required configuration is absent or malformed", () => {
    const invalid = [
      {},
      { ...ENV, DEVICE_TOKEN_HASHES: "bad" },
      { ...ENV, INGEST_TOKEN_HASHES: "bad" },
      { ...ENV, MAX_AGE_S: "0" },
      { ...ENV, POLL_AFTER_S: "301" },
      { ...ENV, INGEST_TOKEN_HASHES: DEVICE_TOKEN_HASH },
    ];

    for (const env of invalid) {
      expect(() => createStoredBridgeFromEnvironment(env, { storeFactory: () => new FakeStore() }))
        .toThrow(VercelStoredConfigError);
    }
  });

  it("constructs the store from environment and fixes the display hint at 60 seconds", async () => {
    let captured: Readonly<Record<string, string | undefined>> | undefined;
    let limiterCaptured: Readonly<Record<string, string | undefined>> | undefined;
    const sharedLimiter = new FakeSharedLimiter();
    const bridge = createStoredBridgeFromEnvironment(ENV, {
      storeFactory: (env) => {
        captured = env;
        return new FakeStore();
      },
      sharedRateLimiterFactory: (env) => {
        limiterCaptured = env;
        return sharedLimiter;
      },
      clock: () => new Date(NOW),
      requestId: () => "request-1",
    });

    const latest = await bridge.latest({
      method: "GET",
      headers: { authorization: `Bearer ${DEVICE_TOKEN}` },
    });
    const ingest = await bridge.ingest({
      method: "POST",
      headers: { authorization: `Bearer ${INGEST_TOKEN}` },
      body: new FakeStore().value,
    });

    expect(captured).toBe(ENV);
    expect(limiterCaptured).toBe(ENV);
    expect(sharedLimiter.checks).toBe(1);
    expect(latest.statusCode).toBe(200);
    expect(latest.body).toEqual(expect.objectContaining({
      age_s: 60,
      poll_after_s: 60,
      server_time: NOW.toISOString(),
      schema: 1,
    }));
    expect(ingest.statusCode).toBe(202);
  });
});
