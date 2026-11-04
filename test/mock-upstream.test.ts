import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { request } from "node:http";
import { createMockUpstream, MOCK_SCENARIOS, type MockScenario } from "../tools/mock-upstream/server.js";

describe("scenario-driven mock upstream", () => {
  const mock = createMockUpstream();

  beforeAll(async () => {
    await mock.ready;
  });

  afterAll(async () => {
    await mock.close();
  });

  it("exposes every scenario named by the implementation plan", () => {
    expect(MOCK_SCENARIOS).toEqual([
      "normal", "flat_line", "rising", "falling", "stale_5m", "stale_20m", "stale_2h",
      "no_readings", "future_timestamp", "value_out_of_range", "null_value", "malformed_json",
      "truncated_response", "http_500", "http_401_expired_ticket", "region_redirect",
      "terms_of_use_required", "slow_10s", "connection_reset", "mmol_units", "trend_notcomputable",
      "range_low", "range_urgent_low", "range_high", "no_target_range", "target_range_changed",
    ] satisfies MockScenario[]);
  });

  it("reports and changes the active scenario without restarting", async () => {
    await mock.setScenario("rising");
    expect(await getJson(`${mock.url}/__scenario`)).toEqual({ scenario: "rising" });

    await mock.setScenario("range_low");
    expect(await getJson(`${mock.url}/__scenario`)).toEqual({ scenario: "range_low" });
  });

  it("can select every planned scenario at runtime", async () => {
    for (const scenario of MOCK_SCENARIOS) {
      await mock.setScenario(scenario);
      expect(await getJson(`${mock.url}/__scenario`)).toEqual({ scenario });
    }
  });

  it("serves a deterministic normal LibreLinkUp-shaped response", async () => {
    await mock.setScenario("normal");
    const response = await getJson(`${mock.url}/llu/connections`);

    expect(response).toMatchObject({
      data: [{
        patientId: "mock-patient-1",
        targetLow: expect.any(Number),
        targetHigh: expect.any(Number),
        glucoseMeasurement: {
          ValueInMgPerDl: expect.any(Number),
          FactoryTimestamp: expect.any(String),
          TrendArrow: 3,
        },
      }],
    });
  });

  it("emits the requested failure shape for malformed upstream JSON", async () => {
    await mock.setScenario("malformed_json");
    await expect(getJson(`${mock.url}/llu/connections/mock-patient-1/graph`)).rejects.toThrow(/invalid json/i);
  });

  it("redirects login to the configured region", async () => {
    await mock.setScenario("region_redirect");
    const response = await getJson(`${mock.url}/llu/auth/login`, {
      method: "POST",
      body: JSON.stringify({ email: "follower@example.invalid", password: "not-a-secret" }),
    });

    expect(response).toEqual(expect.objectContaining({
      data: expect.objectContaining({ redirect: true, region: "eu" }),
    }));
  });
});

function getJson(url: string, options: { method?: string; body?: string } = {}): Promise<any> {
  return new Promise((resolve, reject) => {
    const parsed = new URL(url);
    const req = request({
      hostname: parsed.hostname,
      port: parsed.port,
      path: `${parsed.pathname}${parsed.search}`,
      method: options.method ?? "GET",
      headers: options.body ? { "content-type": "application/json", "content-length": Buffer.byteLength(options.body) } : undefined,
    }, (response) => {
      const chunks: Buffer[] = [];
      response.on("data", (chunk: Buffer) => chunks.push(chunk));
      response.on("end", () => {
        const body = Buffer.concat(chunks).toString("utf8");
        if ((response.statusCode ?? 500) >= 400) {
          reject(new Error(`HTTP ${response.statusCode}: ${body}`));
          return;
        }
        try {
          resolve(JSON.parse(body));
        } catch {
          reject(new Error(`Invalid JSON: ${body}`));
        }
      });
    });
    req.on("error", reject);
    if (options.body) req.write(options.body);
    req.end();
  });
}
