import { describe, expect, it } from "vitest";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { parse as parseYaml } from "yaml";

const root = path.resolve(import.meta.dirname, "..");

const plannedScenarios = [
  "normal", "flat_line", "rising", "falling", "stale_5m", "stale_20m", "stale_2h",
  "no_readings", "future_timestamp", "value_out_of_range", "null_value", "malformed_json",
  "truncated_response", "http_500", "http_401_expired_ticket", "region_redirect",
  "terms_of_use_required", "slow_10s", "connection_reset", "mmol_units", "trend_notcomputable",
  "range_low", "range_urgent_low", "range_high", "no_target_range", "target_range_changed",
] as const;

describe("M1 repository integrity", () => {
  it("keeps the scenario catalogue machine-readable", async () => {
    const source = await readFile(path.join(root, "contracts/scenarios.yaml"), "utf8");
    const document = parseYaml(source) as { scenarios?: Array<{ name?: string }> };
    expect(document.scenarios?.map((scenario) => scenario.name)).toEqual(plannedScenarios);
  });

  it("keeps all golden fixtures free of credentials and private upstream fields", async () => {
    const source = await readFile(path.join(root, "contracts/fixtures/index.json"), "utf8");
    const index = JSON.parse(source) as { fixtures?: string[] };
    expect(index.fixtures).toEqual(expect.arrayContaining([
      "fresh.json", "aging.json", "stale.json", "no-data.json", "auth-error.json", "server-error.json",
    ]));
    expect(source).not.toMatch(/password|Bearer|authTicket|jwt/i);
  });
});
