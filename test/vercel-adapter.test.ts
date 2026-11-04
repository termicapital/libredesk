import { describe, expect, it } from "vitest";
import {
  createVercelHealthHandler,
  createVercelIngestHandler,
  createVercelLatestHandler,
  type VercelRequestLike,
  type VercelResponseLike,
} from "../bridge/src/vercel/adapter.js";
import type { StoredBridge } from "../bridge/src/vercel/stored.js";

class FakeResponse implements VercelResponseLike {
  statusCode = 200;
  headers: Record<string, string> = {};
  jsonBody: unknown;
  ended = false;

  status(code: number): this {
    this.statusCode = code;
    return this;
  }

  setHeader(name: string, value: string): this {
    this.headers[name.toLowerCase()] = value;
    return this;
  }

  json(body: unknown): this {
    this.jsonBody = body;
    return this;
  }

  end(): this {
    this.ended = true;
    return this;
  }
}

function request(overrides: Partial<VercelRequestLike> = {}): VercelRequestLike {
  return { method: "GET", headers: {}, ...overrides };
}

function fakeBridge(overrides: Partial<StoredBridge> = {}): StoredBridge {
  return {
    latest: async () => ({
      statusCode: 200,
      headers: { "cache-control": "no-store" },
      body: { schema: 1 },
    }),
    ingest: async () => ({
      statusCode: 202,
      headers: { "cache-control": "no-store" },
      body: { accepted: true },
    }),
    health: async () => ({
      statusCode: 200,
      headers: { "cache-control": "no-store" },
      body: { ok: true },
    }),
    ...overrides,
  };
}

describe("Vercel function adapter", () => {
  it("normalizes scalar headers and sends the core response", async () => {
    let observedAuthorization: string | undefined;
    const bridge = fakeBridge({
      latest: async (input) => {
        observedAuthorization = input.headers.authorization;
        return {
          statusCode: 200,
          headers: { "cache-control": "no-store", "x-request-id": "request-1" },
          body: { schema: 1 },
        };
      },
    });
    const response = new FakeResponse();

    await createVercelLatestHandler(() => bridge)(
      request({ headers: { authorization: "Bearer token" } }),
      response,
    );

    expect(observedAuthorization).toBe("Bearer token");
    expect(response.statusCode).toBe(200);
    expect(response.headers).toEqual({ "cache-control": "no-store", "x-request-id": "request-1" });
    expect(response.jsonBody).toEqual({ schema: 1 });
    expect(response.ended).toBe(false);
  });

  it("forwards the explicitly selected display contract version", async () => {
    let observedVersion: 1 | 2 | undefined;
    const bridge = fakeBridge({
      latest: async (_input, version) => {
        observedVersion = version;
        return {
          statusCode: 200,
          headers: { "cache-control": "no-store" },
          body: { schema: version },
        };
      },
    });
    const response = new FakeResponse();

    await createVercelLatestHandler(() => bridge, 2)(request(), response);

    expect(observedVersion).toBe(2);
    expect(response.jsonBody).toEqual({ schema: 2 });
  });

  it("rejects ambiguous repeated authorization headers", async () => {
    let observedAuthorization: string | undefined = "not-called";
    const bridge = fakeBridge({
      latest: async (input) => {
        observedAuthorization = input.headers.authorization;
        return { statusCode: 401, headers: { "cache-control": "no-store" }, body: { error: "invalid_token" } };
      },
    });
    const response = new FakeResponse();

    await createVercelLatestHandler(() => bridge)(
      request({ headers: { authorization: ["Bearer first", "Bearer second"] } }),
      response,
    );

    expect(observedAuthorization).toBeUndefined();
    expect(response.statusCode).toBe(401);
  });

  it("ends a 304 response without serializing a body", async () => {
    const bridge = fakeBridge({
      latest: async () => ({
        statusCode: 304,
        headers: { "cache-control": "no-store", etag: "\"abc\"" },
      }),
    });
    const response = new FakeResponse();

    await createVercelLatestHandler(() => bridge)(request(), response);

    expect(response.statusCode).toBe(304);
    expect(response.ended).toBe(true);
    expect(response.jsonBody).toBeUndefined();
  });

  it("forwards the parsed ingestion body and scalar authorization", async () => {
    const body = { schema: 1 };
    let observedBody: unknown;
    let observedAuthorization: string | undefined;
    const bridge = fakeBridge({
      ingest: async (input) => {
        observedBody = input.body;
        observedAuthorization = input.headers.authorization;
        return {
          statusCode: 202,
          headers: { "cache-control": "no-store" },
          body: { accepted: true },
        };
      },
    });
    const response = new FakeResponse();

    await createVercelIngestHandler(() => bridge)(request({
      method: "POST",
      headers: { authorization: "Bearer push-token" },
      body,
    }), response);

    expect(observedAuthorization).toBe("Bearer push-token");
    expect(observedBody).toBe(body);
    expect(response.statusCode).toBe(202);
    expect(response.jsonBody).toEqual({ accepted: true });
  });

  it("awaits asynchronous storage-backed health", async () => {
    const response = new FakeResponse();
    const bridge = fakeBridge({
      health: async () => ({
        statusCode: 200,
        headers: { "cache-control": "no-store" },
        body: { ok: true, mode: "stored_push" },
      }),
    });

    await createVercelHealthHandler(() => bridge)(request(), response);

    expect(response.statusCode).toBe(200);
    expect(response.jsonBody).toEqual({ ok: true, mode: "stored_push" });
  });

  it("contains unexpected exceptions without leaking details", async () => {
    const response = new FakeResponse();
    const handler = createVercelLatestHandler(() => {
      throw new Error("secret internal detail");
    });

    await handler(request(), response);

    expect(response.statusCode).toBe(500);
    expect(response.headers).toEqual({ "cache-control": "no-store" });
    expect(response.jsonBody).toEqual({ error: "internal", detail: "bridge internal error", retry_after_s: 30 });
    expect(JSON.stringify(response.jsonBody)).not.toContain("secret internal detail");
  });

  it("reports an unconfigured deployment without leaking configuration errors", async () => {
    const response = new FakeResponse();
    const handler = createVercelHealthHandler(() => {
      throw new Error("KV_REST_API_TOKEN is missing");
    });

    await handler(request(), response);

    expect(response.statusCode).toBe(503);
    expect(response.headers).toEqual({ "cache-control": "no-store" });
    expect(response.jsonBody).toEqual({
      ok: false,
      configured: false,
      version: "0.3.0-vercel",
      mode: "stored_push",
    });
    expect(JSON.stringify(response.jsonBody)).not.toContain("KV_REST_API_TOKEN");
  });
});
