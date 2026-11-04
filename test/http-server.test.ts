import { afterEach, describe, expect, it } from "vitest";
import { LatestCache } from "../bridge/src/cache.js";
import { createBridgeServer } from "../bridge/src/http/server.js";
import { hashDeviceToken } from "../bridge/src/http/auth.js";

const token = Buffer.alloc(32, 0).toString("base64url");
const revokedToken = Buffer.alloc(32, 1).toString("base64url");
const nonCanonicalToken = "_".repeat(43);
const now = new Date("2026-08-22T09:16:00.000Z");
const reading = {
  glucose: 112,
  unit: "mg/dL" as const,
  trend: "stable" as const,
  range: "in_range" as const,
  target_low: 70,
  target_high: 180,
  timestamp: "2026-08-22T09:15:00.000Z",
  libre_local_time: "11:15:00",
  age_s: 47,
  server_time: "2026-08-22T09:15:47.000Z",
  source: "mock" as const,
  poll_after_s: 60,
  schema: 2 as const,
};

let server: ReturnType<typeof createBridgeServer> | undefined;
afterEach(async () => {
  await server?.app.close();
  server = undefined;
});

describe("authenticated device HTTP API", () => {
  it("returns a validated latest reading with an ETag", async () => {
    const cache = new LatestCache();
    cache.set(reading);
    server = createBridgeServer({
      cache,
      activeTokenHashes: [hashDeviceToken(token)],
      clock: () => now,
      version: "test",
    });

    const response = await server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${token}` },
    });

    expect(response.statusCode).toBe(200);
    expect(response.json()).toMatchObject({ glucose: 112, age_s: 60, server_time: now.toISOString() });
    expect(response.headers.etag).toMatch(/^"[a-f0-9]{64}"$/);
    expect(response.headers["cache-control"]).toBe("no-store");
  });

  it("returns 304 with current age when the ETag is unchanged", async () => {
    const cache = new LatestCache();
    cache.set(reading);
    server = createBridgeServer({ cache, activeTokenHashes: [hashDeviceToken(token)], clock: () => now });

    const first = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${token}` },
    });
    const second = await server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${token}`, "if-none-match": first.headers.etag as string },
    });

    expect(second.statusCode).toBe(304);
    expect(second.body).toBe("");
    expect(second.headers["x-reading-age-s"]).toBe("60");
  });

  it("distinguishes malformed credentials from a well-shaped revoked token", async () => {
    server = createBridgeServer({ cache: new LatestCache(), activeTokenHashes: [hashDeviceToken(token)] });

    const missing = await server.app.inject({ method: "GET", url: "/api/v1/latest" });
    const malformed = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer too-short` },
    });
    const nonCanonical = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${nonCanonicalToken}` },
    });
    const revoked = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${revokedToken}` },
    });

    expect(missing.statusCode).toBe(401);
    expect(malformed.statusCode).toBe(401);
    expect(nonCanonical.statusCode).toBe(401);
    expect(revoked.statusCode).toBe(403);
  });

  it("fails closed with 503 when cache is empty or too old", async () => {
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      clock: () => now,
    });

    const response = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${token}` },
    });

    expect(response.statusCode).toBe(503);
    expect(response.json()).toMatchObject({ error: "upstream_unavailable" });
    expect(response.json()).not.toHaveProperty("glucose");
  });

  it("accepts operational telemetry but rejects glucose fields", async () => {
    server = createBridgeServer({ cache: new LatestCache(), activeTokenHashes: [hashDeviceToken(token)] });
    const headers = { authorization: `Bearer ${token}` };
    const valid = await server.app.inject({
      method: "POST", url: "/api/v1/telemetry", headers,
      payload: {
        firmware_version: "0.1.0", uptime_s: 10, min_free_heap: 50_000, wifi_rssi: -40,
        reconnect_count: 1, http_errors: { timeout: 2 },
      },
    });
    const invalid = await server.app.inject({
      method: "POST", url: "/api/v1/telemetry", headers,
      payload: {
        firmware_version: "0.1.0", uptime_s: 10, min_free_heap: 50_000, wifi_rssi: -40,
        reconnect_count: 1, http_errors: {}, glucose: 112,
      },
    });

    expect(valid.statusCode).toBe(202);
    expect(invalid.statusCode).toBe(400);
  });

  it("limits accepted telemetry to one report per token every five minutes", async () => {
    let clock = new Date("2026-08-22T09:16:00.000Z");
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      clock: () => clock,
    });
    const request = () => server!.app.inject({
      method: "POST",
      url: "/api/v1/telemetry",
      headers: { authorization: `Bearer ${token}` },
      payload: {
        firmware_version: "0.1.0", uptime_s: 10, min_free_heap: 50_000, wifi_rssi: -40,
        reconnect_count: 1, http_errors: {},
      },
    });

    expect((await request()).statusCode).toBe(202);
    const limited = await request();
    expect(limited.statusCode).toBe(429);
    expect(limited.headers["retry-after"]).toBe("300");
    clock = new Date("2026-08-22T09:21:00.000Z");
    expect((await request()).statusCode).toBe(202);
  });

  it("atomically reserves the telemetry interval across concurrent requests", async () => {
    let enteredFirst: (() => void) | undefined;
    let releaseFirst: (() => void) | undefined;
    const entered = new Promise<void>((resolve) => { enteredFirst = resolve; });
    const blocked = new Promise<void>((resolve) => { releaseFirst = resolve; });
    let callbacks = 0;
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      clock: () => new Date("2026-08-22T09:16:00.000Z"),
      onTelemetry: async () => {
        callbacks += 1;
        if (callbacks === 1) {
          enteredFirst?.();
          await blocked;
        }
      },
    });
    const request = {
      method: "POST" as const,
      url: "/api/v1/telemetry",
      headers: { authorization: `Bearer ${token}` },
      payload: {
        firmware_version: "0.1.0", uptime_s: 10, min_free_heap: 50_000,
        wifi_rssi: -45, reconnect_count: 1, http_errors: {},
      },
    };
    const firstPromise = server.app.inject(request);
    await entered;
    const second = await server.app.inject(request);
    releaseFirst?.();
    const first = await firstPromise;
    expect([first.statusCode, second.statusCode].sort()).toEqual([202, 429]);
    expect(callbacks).toBe(1);
  });

  it("limits coredump uploads independently to one per token per hour", async () => {
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      clock: () => new Date("2026-08-22T09:16:00.000Z"),
    });
    const request = {
      method: "POST" as const,
      url: "/api/v1/coredump",
      headers: { authorization: `Bearer ${token}` },
      payload: {
        firmware_version: "0.1.0",
        reset_reason: "panic",
        data_base64: Buffer.from("bounded-dump").toString("base64"),
      },
    };
    const first = await server.app.inject(request);
    const second = await server.app.inject(request);
    expect(first.statusCode).toBe(202);
    expect(second.statusCode).toBe(429);
    expect(second.headers["retry-after"]).toBe("3600");
  });

  it("returns 413 for an oversized coredump", async () => {
    server = createBridgeServer({ cache: new LatestCache(), activeTokenHashes: [hashDeviceToken(token)] });
    const response = await server.app.inject({
      method: "POST",
      url: "/api/v1/coredump",
      headers: { authorization: `Bearer ${token}` },
      payload: {
        firmware_version: "0.1.0",
        reset_reason: "panic",
        data_base64: "A".repeat(524_289),
      },
    });
    expect(response.statusCode).toBe(413);
  });

  it("applies per-token limits before touching the cache", async () => {
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      rateLimit: { maxPerHour: 1, burst: 1 },
    });
    const headers = { authorization: `Bearer ${token}` };

    const first = await server.app.inject({ method: "GET", url: "/api/v1/latest", headers });
    const second = await server.app.inject({ method: "GET", url: "/api/v1/latest", headers });

    expect(first.statusCode).toBe(503);
    expect(second.statusCode).toBe(429);
    expect(second.headers["retry-after"]).toBeDefined();
  });
});
