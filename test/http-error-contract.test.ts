import { readFileSync } from "node:fs";
import { Ajv2020 } from "ajv/dist/2020.js";
import { afterEach, describe, expect, it } from "vitest";
import { LatestCache } from "../bridge/src/cache.js";
import { hashDeviceToken } from "../bridge/src/http/auth.js";
import { createBridgeServer } from "../bridge/src/http/server.js";

const errorSchema = JSON.parse(readFileSync(new URL("../contracts/error.schema.json", import.meta.url), "utf8")) as object;
const validateError = new Ajv2020({ strict: true }).compile(errorSchema);
const token = Buffer.alloc(32, 3).toString("base64url");
const revoked = Buffer.alloc(32, 4).toString("base64url");
let server: ReturnType<typeof createBridgeServer> | undefined;

afterEach(async () => {
  await server?.app.close();
  server = undefined;
});

function assertContractError(response: { statusCode: number; json(): unknown }, status: number, code: string): void {
  expect(response.statusCode).toBe(status);
  const body = response.json();
  expect(validateError(body), JSON.stringify(validateError.errors)).toBe(true);
  expect(body).toMatchObject({ error: code });
}

describe("all emitted HTTP errors obey the frozen error schema", () => {
  it("validates authentication and no-data errors", async () => {
    server = createBridgeServer({ cache: new LatestCache(), activeTokenHashes: [hashDeviceToken(token)] });
    const missing = await server.app.inject({ method: "GET", url: "/api/v1/latest" });
    const inactive = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${revoked}` },
    });
    const noData = await server.app.inject({
      method: "GET", url: "/api/v1/latest", headers: { authorization: `Bearer ${token}` },
    });
    assertContractError(missing, 401, "invalid_token");
    assertContractError(inactive, 403, "revoked_token");
    assertContractError(noData, 503, "upstream_unavailable");
  });

  it("validates malformed telemetry and coredump errors", async () => {
    server = createBridgeServer({ cache: new LatestCache(), activeTokenHashes: [hashDeviceToken(token)] });
    const headers = { authorization: `Bearer ${token}` };
    const telemetry = await server.app.inject({ method: "POST", url: "/api/v1/telemetry", headers, payload: {} });
    const coredump = await server.app.inject({
      method: "POST",
      url: "/api/v1/coredump",
      headers,
      payload: { firmware_version: "0.1.0", reset_reason: "panic", data_base64: "A".repeat(524_289) },
    });
    assertContractError(telemetry, 400, "invalid_payload");
    assertContractError(coredump, 413, "invalid_payload");
  });

  it("validates OTA-unavailable and rate-limit errors", async () => {
    server = createBridgeServer({
      cache: new LatestCache(),
      activeTokenHashes: [hashDeviceToken(token)],
      rateLimit: { maxPerHour: 1, burst: 1 },
    });
    const headers = { authorization: `Bearer ${token}` };
    const ota = await server.app.inject({ method: "GET", url: "/api/v1/ota/manifest", headers });
    const limited = await server.app.inject({ method: "GET", url: "/api/v1/latest", headers });
    assertContractError(ota, 503, "ota_unavailable");
    assertContractError(limited, 429, "rate_limited");
  });
});
