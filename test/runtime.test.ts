import { mkdtemp, readFile, readdir, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { loadBridgeConfig } from "../bridge/src/config.js";
import { hashDeviceToken } from "../bridge/src/http/auth.js";
import { createRedactingLogger } from "../bridge/src/obs/logger.js";
import { createBridgeRuntime, type BridgeRuntime } from "../bridge/src/runtime.js";
import { createMockUpstream } from "../tools/mock-upstream/server.js";

const deviceToken = Buffer.alloc(32, 2).toString("base64url");
const now = new Date("2026-08-22T09:15:47.000Z");
let runtime: BridgeRuntime | undefined;
let coredumpDir: string | undefined;

afterEach(async () => {
  await runtime?.stop();
  runtime = undefined;
  if (coredumpDir) await rm(coredumpDir, { recursive: true, force: true });
  coredumpDir = undefined;
});

describe("complete bridge runtime wiring", () => {
  it("polls mock upstream, serves the device API, records metrics, and accepts tmpfs diagnostics", async () => {
    const mock = createMockUpstream();
    await mock.ready;
    coredumpDir = await mkdtemp(path.join(tmpdir(), "libre-desk-coredumps-"));
    const lines: string[] = [];
    const config = loadBridgeConfig({
      LIBRE_EMAIL: "follower@example.invalid",
      LIBRE_PASSWORD: "follower-password",
      LLU_APP_VERSION: "4.16.0",
      DEVICE_TOKEN_HASHES: hashDeviceToken(deviceToken),
      UPSTREAM_BASE_URL: mock.url,
      UPSTREAM_SOURCE: "mock",
      UPSTREAM_TIMEOUT_MS: "500",
    });
    runtime = createBridgeRuntime(config, {
      clock: () => now,
      logger: createRedactingLogger((line) => lines.push(line)),
      coredumpDir,
      random: () => 0.5,
    });

    try {
      await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "success" });
      const auth = { authorization: `Bearer ${deviceToken}` };
      const latest = await runtime.server.app.inject({ method: "GET", url: "/api/v1/latest", headers: auth });
      const health = await runtime.server.app.inject({ method: "GET", url: "/healthz" });
      const telemetry = await runtime.server.app.inject({
        method: "POST",
        url: "/api/v1/telemetry",
        headers: auth,
        payload: {
          firmware_version: "0.1.0",
          uptime_s: 10,
          min_free_heap: 50_000,
          wifi_rssi: -40,
          reconnect_count: 1,
          http_errors: {},
        },
      });
      const coredump = await runtime.server.app.inject({
        method: "POST",
        url: "/api/v1/coredump",
        headers: auth,
        payload: {
          firmware_version: "0.1.0",
          reset_reason: "panic",
          data_base64: Buffer.from("dump-data").toString("base64"),
        },
      });
      const metrics = await runtime.server.app.inject({ method: "GET", url: "/metrics", headers: auth });

      expect(latest.statusCode).toBe(200);
      expect(latest.json()).toMatchObject({ glucose: 112, source: "mock" });
      expect(health.json()).toMatchObject({ upstream_ok: true, last_success_age_s: 0 });
      expect(telemetry.statusCode).toBe(202);
      expect(coredump.statusCode).toBe(202);
      expect(metrics.body).toContain("upstream_login_total 1");
      expect(metrics.body).toContain('upstream_poll_total{result="success"} 1');
      expect(metrics.body).not.toContain("glucose");

      const files = await readdir(coredumpDir);
      expect(files).toHaveLength(1);
      expect(await readFile(path.join(coredumpDir, files[0] as string), "utf8")).toBe("dump-data");
      expect(lines.join("\n")).not.toContain("follower-password");
      expect(lines.join("\n")).not.toContain('"glucose":112');
    } finally {
      await mock.close();
    }
  });

  it("maps a persistent upstream 401 to the documented authentication failure", async () => {
    const mock = createMockUpstream({ scenario: "http_401_expired_ticket" });
    await mock.ready;
    const config = loadBridgeConfig({
      LIBRE_EMAIL: "follower@example.invalid",
      LIBRE_PASSWORD: "follower-password",
      LLU_APP_VERSION: "4.16.0",
      DEVICE_TOKEN_HASHES: hashDeviceToken(deviceToken),
      UPSTREAM_BASE_URL: mock.url,
      UPSTREAM_SOURCE: "mock",
      UPSTREAM_TIMEOUT_MS: "500",
    });
    runtime = createBridgeRuntime(config, { clock: () => now });
    try {
      await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "error", code: "upstream_auth_failed" });
      const response = await runtime.server.app.inject({
        method: "GET",
        url: "/api/v1/latest",
        headers: { authorization: `Bearer ${deviceToken}` },
      });
      expect(response.statusCode).toBe(503);
      expect(response.json()).toMatchObject({ error: "upstream_auth_failed" });
    } finally {
      await mock.close();
    }
  });

  it("continues aging the reading metric through an outage beyond MAX_AGE_S", async () => {
    const mock = createMockUpstream();
    await mock.ready;
    let clock = new Date(now);
    const config = loadBridgeConfig({
      LIBRE_EMAIL: "follower@example.invalid",
      LIBRE_PASSWORD: "follower-password",
      LLU_APP_VERSION: "4.16.0",
      DEVICE_TOKEN_HASHES: hashDeviceToken(deviceToken),
      UPSTREAM_BASE_URL: mock.url,
      UPSTREAM_SOURCE: "mock",
      UPSTREAM_TIMEOUT_MS: "500",
      MAX_AGE_S: "3600",
    });
    runtime = createBridgeRuntime(config, { clock: () => clock });
    try {
      await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "success" });
      clock = new Date(now.getTime() + 3_700_000);
      await mock.setScenario("http_500");
      await expect(runtime.pollOnce()).resolves.toMatchObject({ status: "error" });
      const response = await runtime.server.app.inject({
        method: "GET",
        url: "/metrics",
        headers: { authorization: `Bearer ${deviceToken}` },
      });
      expect(response.statusCode).toBe(200);
      const ageMatch = /reading_age_seconds (\d+)\b/.exec(response.body);
      expect(Number(ageMatch?.[1])).toBeGreaterThan(3_600);
    } finally {
      await mock.close();
    }
  });
});
