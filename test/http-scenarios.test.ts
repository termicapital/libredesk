import { createHash } from "node:crypto";
import { afterEach, describe, expect, it } from "vitest";
import { loadBridgeConfig } from "../bridge/src/config.js";
import { createRedactingLogger } from "../bridge/src/obs/logger.js";
import { createBridgeRuntime, type BridgeRuntime } from "../bridge/src/runtime.js";
import { createMockUpstream, type MockScenario, type MockUpstream } from "../tools/mock-upstream/server.js";

const now = new Date("2026-08-22T09:15:00.000Z");
const deviceToken = Buffer.alloc(32, 17).toString("base64url");
const tokenHash = createHash("sha256").update(deviceToken).digest("hex");
const successScenarios: MockScenario[] = [
  "normal",
  "flat_line",
  "rising",
  "falling",
  "stale_5m",
  "stale_20m",
  "region_redirect",
  "mmol_units",
  "trend_notcomputable",
  "range_low",
  "range_urgent_low",
  "range_high",
  "no_target_range",
  "target_range_changed",
];
const failureScenarios: ReadonlyArray<[MockScenario, string]> = [
  ["stale_2h", "upstream_unavailable"],
  ["no_readings", "upstream_unavailable"],
  ["future_timestamp", "malformed_upstream"],
  ["value_out_of_range", "malformed_upstream"],
  ["null_value", "malformed_upstream"],
  ["malformed_json", "malformed_upstream"],
  ["truncated_response", "upstream_unavailable"],
  ["http_500", "upstream_unavailable"],
  ["http_401_expired_ticket", "upstream_auth_failed"],
  ["terms_of_use_required", "terms_of_use_required"],
  ["slow_10s", "upstream_unavailable"],
  ["connection_reset", "upstream_unavailable"],
];

function configFor(mock: MockUpstream) {
  return loadBridgeConfig({
    LIBRE_EMAIL: "follower@example.invalid",
    LIBRE_PASSWORD: "follower-password",
    LLU_APP_VERSION: "4.16.0",
    DEVICE_TOKEN_HASHES: tokenHash,
    UPSTREAM_BASE_URL: mock.url,
    UPSTREAM_SOURCE: "mock",
    UPSTREAM_TIMEOUT_MS: "200",
    MAX_AGE_S: "3600",
    POLL_JITTER_S: "0",
  });
}

describe("mock catalogue through the authenticated runtime HTTP boundary", () => {
  let runtime: BridgeRuntime | undefined;
  let mock: MockUpstream | undefined;

  afterEach(async () => {
    await runtime?.stop();
    await mock?.close();
    runtime = undefined;
    mock = undefined;
  });

  it.each(successScenarios)("serves validated scenario %s as the device contract", async (scenario) => {
    mock = createMockUpstream({ scenario, now, slowDelayMs: 300 });
    await mock.ready;
    runtime = createBridgeRuntime(configFor(mock), {
      clock: () => now,
      logger: createRedactingLogger(() => undefined),
    });

    await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "success" });
    const response = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: "Bear" + "er " + deviceToken },
    });
    const v2 = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v2/latest",
      headers: { authorization: "Bear" + "er " + deviceToken },
    });
    expect(response.statusCode).toBe(200);
    expect(response.headers["cache-control"]).toBe("no-store");
    expect(response.headers.etag).toBeTruthy();
    expect(response.json()).toMatchObject({ schema: 1, source: "mock" });
    expect(response.json()).not.toHaveProperty("libre_local_time");
    expect(v2.statusCode).toBe(200);
    expect(v2.json()).toMatchObject({
      schema: 2,
      source: "mock",
      libre_local_time: expect.stringMatching(/^\d{2}:\d{2}:\d{2}$/),
    });
  });

  it.each(failureScenarios)("fails closed for scenario %s with %s", async (scenario, expectedError) => {
    mock = createMockUpstream({ scenario, now, slowDelayMs: 300 });
    await mock.ready;
    runtime = createBridgeRuntime(configFor(mock), {
      clock: () => now,
      logger: createRedactingLogger(() => undefined),
    });

    await expect(runtime.pollOnce()).resolves.not.toMatchObject({ status: "success" });
    const response = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${deviceToken}` },
    });
    expect(response.statusCode).toBe(503);
    expect(response.headers["cache-control"]).toBe("no-store");
    expect(response.json()).toMatchObject({ error: expectedError });
  });

  it("keeps the last valid response after a rejected poll, then expires it by age", async () => {
    mock = createMockUpstream({ scenario: "normal", now });
    await mock.ready;
    let clock = new Date(now);
    runtime = createBridgeRuntime(configFor(mock), {
      clock: () => clock,
      logger: createRedactingLogger(() => undefined),
    });

    await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "success" });
    const first = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${deviceToken}` },
    });
    expect(first.statusCode).toBe(200);

    await mock.setScenario("future_timestamp");
    await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "rejected" });
    const preserved = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${deviceToken}` },
    });
    expect(preserved.statusCode).toBe(200);
    expect(preserved.headers.etag).toBe(first.headers.etag);

    clock = new Date(now.getTime() + 3_700_000);
    const expired = await runtime.server.app.inject({
      method: "GET",
      url: "/api/v1/latest",
      headers: { authorization: `Bearer ${deviceToken}` },
    });
    expect(expired.statusCode).toBe(503);
    expect(expired.json()).toMatchObject({ error: "malformed_upstream" });
  });
});
