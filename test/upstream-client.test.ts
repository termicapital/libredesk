import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { createMockUpstream } from "../tools/mock-upstream/server.js";
import {
  LibreLinkUpClient,
  resolveRegionBaseUrl,
  resolveTicketExpiryMs,
  UpstreamHttpError,
  UpstreamProtocolError,
} from "../bridge/src/upstream/client.js";

describe("in-house LibreLinkUp client", () => {
  const mock = createMockUpstream();
  const client = new LibreLinkUpClient({
    baseUrl: "pending",
    email: "follower@example.invalid",
    password: "not-a-secret",
    product: "llu.android",
    version: "4.16.0",
    timeoutMs: 500,
  });

  beforeAll(async () => {
    await mock.ready;
    client.setBaseUrl(mock.url);
  });

  afterAll(async () => {
    await mock.close();
  });

  it("rejects malicious or malformed region redirect values", () => {
    expect(resolveRegionBaseUrl("eu")).toBe("https://api-eu.libreview.io");
    expect(() => resolveRegionBaseUrl("foo@evil.example/")).toThrowError(/region/i);
    expect(() => resolveRegionBaseUrl("eu.example.com")).toThrowError(/region/i);
  });

  it("interprets Abbott ticket expiry as either epoch seconds or a test duration", () => {
    const issuedAtMs = Date.parse("2026-08-22T09:00:00Z");
    const absoluteExpirySeconds = Math.floor(issuedAtMs / 1000) + 3_600;
    expect(resolveTicketExpiryMs(absoluteExpirySeconds, issuedAtMs)).toBe(issuedAtMs + 3_600_000);
    expect(resolveTicketExpiryMs(3_600, issuedAtMs)).toBe(issuedAtMs + 3_600_000);
    expect(() => resolveTicketExpiryMs(Math.floor(issuedAtMs / 1000) - 1, issuedAtMs)).toThrowError(/expired/i);
  });

  it("logs in and fetches a connection plus graph using the mock upstream", async () => {
    await mock.setScenario("normal");
    await client.login();
    const connections = await client.getConnections();
    const graph = await client.getGraph("mock-patient-1");

    expect(connections).toHaveLength(1);
    expect(connections[0]).toMatchObject({ patientId: "mock-patient-1" });
    expect(graph).toMatchObject({ data: { connection: { patientId: "mock-patient-1" } } });
    expect(client.accountId).toMatch(/^[a-f0-9]{64}$/);
  });

  it("sends the current Abbott-compatible identity headers without requesting compressed JSON", async () => {
    await mock.setScenario("normal");
    client.clearSession();
    await client.login();
    await client.getConnections();

    const loginHeaders = mock.lastHeaders("/llu/auth/login");
    expect(loginHeaders?.["user-agent"]).toBe("LibreDesk/0.1");
    expect(loginHeaders?.["account-id"]).toBe("");
    expect(loginHeaders?.["accept-encoding"]).toBeUndefined();

    const authenticatedHeaders = mock.lastHeaders("/llu/connections");
    expect(authenticatedHeaders?.["user-agent"]).toBe("LibreDesk/0.1");
    expect(authenticatedHeaders?.["account-id"]).toMatch(/^[a-f0-9]{64}$/);
    expect(authenticatedHeaders?.["accept-encoding"]).toBeUndefined();
  });

  it("exposes upstream HTTP status without returning a partial reading", async () => {
    await mock.setScenario("http_401_expired_ticket");
    client.clearSession();

    await expect(client.getGraph("mock-patient-1")).rejects.toEqual(
      expect.objectContaining({
        name: "UpstreamHttpError",
        statusCode: 401,
      }),
    );
  });

  it("re-authenticates once after an expired ticket before surfacing the persistent 401", async () => {
    await mock.setScenario("http_401_expired_ticket");
    client.clearSession();
    const loginCountBefore = mock.requestCount("/llu/auth/login");

    await expect(client.getConnections()).rejects.toEqual(
      expect.objectContaining({ name: "UpstreamHttpError", statusCode: 401 }),
    );

    expect(mock.requestCount("/llu/auth/login") - loginCountBefore).toBe(2);
  });

  it("turns invalid JSON into a protocol error", async () => {
    await mock.setScenario("normal");
    await client.login();
    await mock.setScenario("malformed_json");

    await expect(client.getGraph("mock-patient-1")).rejects.toBeInstanceOf(UpstreamProtocolError);
  });

  it("surfaces terms-of-use interstitials as an actionable error", async () => {
    await mock.setScenario("terms_of_use_required");
    client.clearSession();

    await expect(client.login()).rejects.toEqual(
      expect.objectContaining({ code: "terms_of_use_required" }),
    );
  });
});
