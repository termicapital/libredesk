import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { LatestCache } from "../bridge/src/cache.js";
import { Poller, type PollResult } from "../bridge/src/poller.js";
import { LibreLinkUpClient } from "../bridge/src/upstream/client.js";
import {
  createMockUpstream,
  type MockScenario,
} from "../tools/mock-upstream/server.js";

const now = new Date("2026-08-22T09:15:47.000Z");

describe("bridge behavior across every mock-upstream scenario", () => {
  const mock = createMockUpstream({ slowDelayMs: 75 });

  beforeAll(async () => {
    await mock.ready;
  });

  afterAll(async () => {
    await mock.close();
  });

  async function poll(scenario: MockScenario, timeoutMs = 500): Promise<PollResult> {
    await mock.setScenario(scenario);
    const client = new LibreLinkUpClient({
      baseUrl: mock.url,
      email: "follower@example.invalid",
      password: "test-password",
      product: "llu.android",
      version: "4.16.0",
      timeoutMs,
      regionBaseUrl: () => mock.url,
    });
    return new Poller(client, new LatestCache(), {
      source: "mock",
      urgentLowMgDl: 70,
    }).pollOnce(now);
  }

  it.each([
    ["normal", { glucose: 112, trend: "stable", range: "in_range", age_s: 47 }],
    ["flat_line", { glucose: 112, trend: "stable", range: "in_range" }],
    ["rising", { glucose: 125, trend: "rising", range: "in_range" }],
    ["falling", { glucose: 100, trend: "falling", range: "in_range" }],
    ["stale_5m", { glucose: 112, age_s: 347 }],
    ["stale_20m", { glucose: 112, age_s: 1_247 }],
    ["mmol_units", { glucose: 6.2, unit: "mmol/L", target_low: 3.9, target_high: 10 }],
    ["trend_notcomputable", { trend: "unknown" }],
    ["range_low", { glucose: 75, range: "low", target_low: 80 }],
    ["range_urgent_low", { glucose: 50, range: "urgent_low" }],
    ["range_high", { glucose: 220, range: "high" }],
    ["no_target_range", { range: "unknown", target_low: null, target_high: null }],
    ["target_range_changed", { target_low: 80, target_high: 160, range: "in_range" }],
    ["region_redirect", { glucose: 112, range: "in_range" }],
  ] satisfies Array<[MockScenario, Record<string, unknown>]>) (
    "%s publishes only the expected normalized reading",
    async (scenario, expected) => {
      await expect(poll(scenario)).resolves.toMatchObject({
        status: "success",
        reading: expected,
      });
    },
  );

  it.each([
    ["stale_2h", "too_old"],
    ["no_readings", "missing_measurement"],
    ["future_timestamp", "future_timestamp"],
    ["value_out_of_range", "value_out_of_range"],
    ["null_value", "missing_value"],
  ] satisfies Array<[MockScenario, string]>) (
    "%s is rejected without publishing a partial reading",
    async (scenario, reason) => {
      const result = await poll(scenario);
      expect(result).toMatchObject({ status: "rejected", reason });
      expect(result).not.toHaveProperty("reading");
    },
  );

  it.each([
    ["malformed_json", "upstream_protocol_error", 500],
    ["truncated_response", "upstream_transport_error", 500],
    ["http_500", "upstream_http_error", 500],
    ["http_401_expired_ticket", "upstream_auth_failed", 500],
    ["terms_of_use_required", "terms_of_use_required", 500],
    ["slow_10s", "upstream_transport_error", 10],
    ["connection_reset", "upstream_transport_error", 500],
  ] satisfies Array<[MockScenario, string, number]>) (
    "%s becomes an explicit upstream error and never a reading",
    async (scenario, code, timeoutMs) => {
      const result = await poll(scenario, timeoutMs);
      expect(result).toMatchObject({ status: "error", code });
      expect(result).not.toHaveProperty("reading");
    },
  );
});
