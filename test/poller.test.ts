import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { LatestCache } from "../bridge/src/cache.js";
import { LibreLinkUpClient } from "../bridge/src/upstream/client.js";
import { Poller } from "../bridge/src/poller.js";
import { createMockUpstream } from "../tools/mock-upstream/server.js";

describe("single-flight bridge poller", () => {
  const mock = createMockUpstream();
  const client = new LibreLinkUpClient({
    baseUrl: "pending",
    email: "follower@example.invalid",
    password: "not-a-secret",
    product: "llu.android",
    version: "4.16.0",
  });
  const cache = new LatestCache();
  const poller = new Poller(client, cache, { source: "mock", urgentLowMgDl: 70 });
  const now = new Date("2026-08-22T09:15:47.000Z");

  beforeAll(async () => {
    await mock.ready;
    client.setBaseUrl(mock.url);
  });

  afterAll(async () => {
    await mock.close();
  });

  it("publishes only a fully validated normal reading", async () => {
    await mock.setScenario("normal");
    const result = await poller.pollOnce(now);

    expect(result).toMatchObject({ status: "success", reading: { glucose: 112, age_s: 47 } });
    expect(cache.get(now, 3_600)).toMatchObject({ glucose: 112 });
  });

  it("keeps the last valid cache when a new upstream value is rejected", async () => {
    await mock.setScenario("value_out_of_range");
    const result = await poller.pollOnce(now);

    expect(result).toMatchObject({ status: "rejected", reason: "value_out_of_range" });
    expect(cache.get(now, 3_600)).toMatchObject({ glucose: 112 });
  });

  it("returns no-data for a connection with no measurement", async () => {
    await mock.setScenario("no_readings");
    const result = await poller.pollOnce(now);

    expect(result).toMatchObject({ status: "rejected", reason: "missing_measurement" });
  });

  it("rejects an ambiguous follower account with more than one patient", async () => {
    const source = {
      async getConnections() {
        return [
          { patientId: "patient-a", glucoseMeasurement: {} },
          { patientId: "patient-b", glucoseMeasurement: {} },
        ];
      },
      async getGraph() {
        throw new Error("graph must not be requested for an ambiguous account");
      },
    };
    const ambiguousCache = new LatestCache();
    const result = await new Poller(source, ambiguousCache).pollOnce(now);
    expect(result).toMatchObject({ status: "rejected", reason: "invalid_connection" });
    expect(ambiguousCache.get(now, 3_600)).toBeUndefined();
  });

  it("rejects a graph whose patient identity differs from the requested connection", async () => {
    const source = {
      async getConnections() {
        return [{ patientId: "patient-a", glucoseMeasurement: {} }];
      },
      async getGraph() {
        return {
          data: {
            connection: {
              patientId: "patient-b",
              targetLow: 70,
              targetHigh: 180,
              glucoseMeasurement: {
                ValueInMgPerDl: 112,
                Value: 112,
                GlucoseUnits: "mg/dL",
                FactoryTimestamp: "2026-08-22T09:15:00.000Z",
                TrendArrow: 3,
              },
            },
          },
        };
      },
    };
    const mismatchedCache = new LatestCache();
    const result = await new Poller(source, mismatchedCache).pollOnce(now);
    expect(result).toMatchObject({ status: "error", code: "upstream_protocol_error" });
    expect(mismatchedCache.get(now, 3_600)).toBeUndefined();
  });

  it("shares one upstream flight between concurrent poll requests", async () => {
    let calls = 0;
    let release: (() => void) | undefined;
    const source = {
      async getConnections() {
        calls += 1;
        await new Promise<void>((resolve) => { release = resolve; });
        return [{ patientId: "patient", glucoseMeasurement: {} }];
      },
      async getGraph() {
        return { data: { connection: {} } };
      },
    };
    const concurrentPoller = new Poller(source, new LatestCache());
    const first = concurrentPoller.pollOnce(now);
    const second = concurrentPoller.pollOnce(now);
    expect(first).toBe(second);
    release?.();
    await Promise.all([first, second]);
    expect(calls).toBe(1);
  });
});
