import { chmod, mkdtemp, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { describe, expect, it } from "vitest";
import {
  PusherConfigError,
  PusherError,
  loadSecurePusherConfig,
  parsePusherConfig,
  pushLatestReading,
  type FetchLike,
} from "../bridge/src/vercel/pusher.js";

const DEVICE_TOKEN = Buffer.alloc(32, 11).toString("base64url");
const INGEST_TOKEN = Buffer.alloc(32, 12).toString("base64url");
const INGEST_URL = "https://bridge.example.invalid/api/v1/ingest";
const LOCAL_URL = "http://127.0.0.1:18080/api/v2/latest";

function reading() {
  return {
    glucose: 120,
    unit: "mg/dL",
    trend: "stable",
    range: "in_range",
    target_low: 70,
    target_high: 180,
    timestamp: "2026-01-01T00:00:00.000Z",
    libre_local_time: "00:00:00",
    age_s: 60,
    server_time: "2026-01-01T00:01:00.000Z",
    source: "librelinkup",
    poll_after_s: 60,
    schema: 2,
  };
}

function config() {
  return {
    localUrl: LOCAL_URL,
    ingestUrl: INGEST_URL,
    deviceToken: DEVICE_TOKEN,
    ingestToken: INGEST_TOKEN,
    timeoutMs: 5_000,
    maxBodyBytes: 4_096,
  };
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

describe("VPS-to-Vercel reading pusher", () => {
  it("strictly parses separate secure tokens and fixed endpoint shapes", () => {
    expect(parsePusherConfig({
      device_token: DEVICE_TOKEN,
      ingest_token: INGEST_TOKEN,
      ingest_url: INGEST_URL,
    })).toEqual(config());

    for (const invalid of [
      {},
      { device_token: DEVICE_TOKEN, ingest_token: DEVICE_TOKEN, ingest_url: INGEST_URL },
      { device_token: "short", ingest_token: INGEST_TOKEN, ingest_url: INGEST_URL },
      { device_token: DEVICE_TOKEN, ingest_token: INGEST_TOKEN, ingest_url: INGEST_URL, extra: true },
      { device_token: DEVICE_TOKEN, ingest_token: INGEST_TOKEN, ingest_url: "http://example.test/api/v1/ingest" },
      { device_token: DEVICE_TOKEN, ingest_token: INGEST_TOKEN, ingest_url: "https://example.test/wrong" },
      { device_token: DEVICE_TOKEN, ingest_token: INGEST_TOKEN, ingest_url: `${INGEST_URL}?secret=x` },
    ]) {
      expect(() => parsePusherConfig(invalid)).toThrow(PusherConfigError);
    }
  });

  it("loads only a regular owner-only configuration file", async () => {
    const dir = await mkdtemp(path.join(tmpdir(), "libre-pusher-test-"));
    const securePath = path.join(dir, "secure.json");
    const linkPath = path.join(dir, "link.json");
    const contents = JSON.stringify({
      device_token: DEVICE_TOKEN,
      ingest_token: INGEST_TOKEN,
      ingest_url: INGEST_URL,
    });
    try {
      await writeFile(securePath, contents, { mode: 0o600 });
      await expect(loadSecurePusherConfig(securePath)).resolves.toEqual(config());

      await chmod(securePath, 0o644);
      await expect(loadSecurePusherConfig(securePath)).rejects.toThrow(PusherConfigError);

      await chmod(securePath, 0o600);
      await symlink(securePath, linkPath);
      await expect(loadSecurePusherConfig(linkPath)).rejects.toThrow(PusherConfigError);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("fetches the loopback bridge then posts the bounded reading to Vercel", async () => {
    const calls: Array<{ url: string; init: RequestInit | undefined }> = [];
    const fetcher: FetchLike = async (input, init) => {
      const url = String(input);
      calls.push({ url, init });
      if (url === LOCAL_URL) return jsonResponse(reading());
      return jsonResponse({ accepted: true, result: "stored" }, 202);
    };

    await expect(pushLatestReading(config(), fetcher)).resolves.toEqual({ result: "stored" });
    expect(calls).toHaveLength(2);
    expect(calls[0]).toEqual(expect.objectContaining({
      url: LOCAL_URL,
      init: expect.objectContaining({
        method: "GET",
        redirect: "error",
        headers: { authorization: `Bearer ${DEVICE_TOKEN}` },
        signal: expect.any(AbortSignal),
      }),
    }));
    expect(calls[1]).toEqual(expect.objectContaining({
      url: INGEST_URL,
      init: expect.objectContaining({
        method: "POST",
        redirect: "error",
        headers: {
          authorization: `Bearer ${INGEST_TOKEN}`,
          "content-type": "application/json",
        },
        body: JSON.stringify(reading()),
        signal: expect.any(AbortSignal),
      }),
    }));
  });

  it("accepts an idempotent duplicate response", async () => {
    const fetcher: FetchLike = async (input) => String(input) === LOCAL_URL
      ? jsonResponse(reading())
      : jsonResponse({ accepted: true, result: "duplicate" }, 202);

    await expect(pushLatestReading(config(), fetcher)).resolves.toEqual({ result: "duplicate" });
  });

  it("does not post after a bad, oversized, or malformed local response", async () => {
    const badResponses = [
      new Response("unavailable", { status: 503 }),
      new Response("x".repeat(4_097), { status: 200 }),
      jsonResponse({ ...reading(), source: "mock" }),
      jsonResponse({ ...reading(), schema: 1 }),
    ];

    for (const localResponse of badResponses) {
      let calls = 0;
      const fetcher: FetchLike = async () => {
        calls += 1;
        return localResponse.clone();
      };
      await expect(pushLatestReading(config(), fetcher)).rejects.toThrow(PusherError);
      expect(calls).toBe(1);
    }
  });

  it("rejects Vercel errors and malformed acknowledgements without leaking tokens", async () => {
    const remoteResponses = [
      jsonResponse({ error: "storage_unavailable" }, 503),
      jsonResponse({ accepted: true, result: "unexpected" }, 202),
      new Response("not-json", { status: 202 }),
    ];

    for (const remoteResponse of remoteResponses) {
      const fetcher: FetchLike = async (input) => String(input) === LOCAL_URL
        ? jsonResponse(reading())
        : remoteResponse.clone();
      let message = "";
      try {
        await pushLatestReading(config(), fetcher);
      } catch (error) {
        message = error instanceof Error ? error.message : String(error);
      }
      expect(message).toBeTruthy();
      expect(message).not.toContain(DEVICE_TOKEN);
      expect(message).not.toContain(INGEST_TOKEN);
    }
  });
});
