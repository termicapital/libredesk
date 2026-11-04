import { describe, expect, it } from "vitest";
import {
  RedisConfigError,
  createRedisReadingStoreFromEnvironment,
  type FetchLike,
} from "../bridge/src/vercel/redis-store.js";

function reading(timestamp = "2026-01-01T00:00:00.000Z") {
  return {
    glucose: 120,
    unit: "mg/dL" as const,
    trend: "stable" as const,
    range: "in_range" as const,
    target_low: 70,
    target_high: 180,
    timestamp,
    libre_local_time: "00:00:00",
    age_s: 60,
    server_time: "2026-01-01T00:01:00.000Z",
    source: "librelinkup" as const,
    poll_after_s: 60,
    schema: 2 as const,
  };
}

const ENV = {
  KV_REST_API_URL: "https://redis.example.test",
  KV_REST_API_TOKEN: "private-test-token",
};

function response(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

describe("Upstash REST reading store", () => {
  it("fails closed for missing, insecure, or malformed configuration", () => {
    for (const env of [
      {},
      { KV_REST_API_URL: "https://redis.example.test" },
      { KV_REST_API_URL: "http://redis.example.test", KV_REST_API_TOKEN: "token" },
      { KV_REST_API_URL: "not-a-url", KV_REST_API_TOKEN: "token" },
    ]) {
      expect(() => createRedisReadingStoreFromEnvironment(env)).toThrow(RedisConfigError);
    }
  });

  it("reads and parses the single latest-reading key", async () => {
    const calls: Array<{ input: RequestInfo | URL; init?: RequestInit }> = [];
    const fetcher: FetchLike = async (input, init) => {
      calls.push({ input, ...(init === undefined ? {} : { init }) });
      return response({ result: JSON.stringify(reading()) });
    };
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await expect(store.get()).resolves.toEqual(reading());
    expect(calls).toHaveLength(1);
    expect(String(calls[0]?.input)).toBe(ENV.KV_REST_API_URL);
    expect(calls[0]?.init).toEqual(expect.objectContaining({
      method: "POST",
      headers: {
        authorization: "Bearer private-test-token",
        "content-type": "application/json",
      },
      body: JSON.stringify(["GET", "libre-desk:latest"]),
      signal: expect.any(AbortSignal),
    }));
  });

  it("maps a missing key to undefined", async () => {
    const fetcher: FetchLike = async () => response({ result: null });
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await expect(store.get()).resolves.toBeUndefined();
  });

  it("uses an atomic three-key Lua write and maps all monotonic outcomes", async () => {
    for (const [rawResult, expected] of [
      [1, "stored"],
      [0, "duplicate"],
      [-1, "older"],
      [-2, "conflict"],
    ] as const) {
      let command: unknown[] | undefined;
      const fetcher: FetchLike = async (_input, init) => {
        command = JSON.parse(String(init?.body)) as unknown[];
        return response({ result: rawResult });
      };
      const store = createRedisReadingStoreFromEnvironment(ENV, {
        fetcher,
        ttlS: 86_400,
      });

      await expect(store.putIfNewer(reading())).resolves.toBe(expected);
      expect(command?.[0]).toBe("EVAL");
      expect(command?.[2]).toBe("3");
      expect(command?.[3]).toBe("libre-desk:latest");
      expect(command?.[4]).toBe("libre-desk:latest:timestamp-ms");
      expect(command?.[5]).toBe("libre-desk:latest:identity-sha256");
      expect(JSON.parse(String(command?.[6]))).toEqual(reading());
      expect(command?.[7]).toBe(String(Date.parse(reading().timestamp)));
      expect(command?.[8]).toMatch(/^[a-f0-9]{64}$/);
      expect(command?.[9]).toBe("86400");
      expect(String(command?.[1])).toContain("redis.call('SET', KEYS[1]");
      expect(String(command?.[1])).toContain("redis.call('SET', KEYS[2]");
      expect(String(command?.[1])).toContain("redis.call('SET', KEYS[3]");
      expect(String(command?.[1])).toContain("if not currentIdentity then");
    }
  });

  it("rewrites every key on an idempotent duplicate to repair partial eviction", async () => {
    let script = "";
    const fetcher: FetchLike = async (_input, init) => {
      const command = JSON.parse(String(init?.body)) as unknown[];
      script = String(command[1]);
      return response({ result: 0 });
    };
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await expect(store.putIfNewer(reading())).resolves.toBe("duplicate");
    const duplicateStart = script.indexOf("if currentIdentity == ARGV[3] then");
    const duplicateEnd = script.indexOf("return 0", duplicateStart);
    expect(duplicateStart).toBeGreaterThanOrEqual(0);
    expect(duplicateEnd).toBeGreaterThan(duplicateStart);
    const duplicateBranch = script.slice(duplicateStart, duplicateEnd);
    expect(duplicateBranch).toContain("redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[4])");
    expect(duplicateBranch).toContain("redis.call('SET', KEYS[2], ARGV[2], 'EX', ARGV[4])");
    expect(duplicateBranch).toContain("redis.call('SET', KEYS[3], ARGV[3], 'EX', ARGV[4])");
  });

  it("fails closed for incomplete monotonic metadata and initializes only an empty store", async () => {
    let script = "";
    const fetcher: FetchLike = async (_input, init) => {
      const command = JSON.parse(String(init?.body)) as unknown[];
      script = String(command[1]);
      return response({ result: -2 });
    };
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await expect(store.putIfNewer(reading())).resolves.toBe("conflict");

    expect(script).toContain([
      "if not currentTimestamp then",
      "  if currentPayload or currentIdentity then return -2 end",
      "else",
    ].join("\n"));
    expect(script).toContain("if not currentIdentity then return -2 end");
    expect(script).not.toContain([
      "if not currentIdentity then",
      "  redis.call('SET'",
    ].join("\n"));
  });

  it("uses stable measurement identity independent of age and server time", async () => {
    const identities: string[] = [];
    const fetcher: FetchLike = async (_input, init) => {
      const command = JSON.parse(String(init?.body)) as unknown[];
      identities.push(String(command[8]));
      return response({ result: 0 });
    };
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await store.putIfNewer(reading());
    await store.putIfNewer({
      ...reading(),
      age_s: 120,
      server_time: "2026-01-01T00:02:00.000Z",
    });
    const stableMutations = [
      { ...reading(), glucose: 121 },
      { ...reading(), unit: "mmol/L" as const },
      { ...reading(), trend: "rising" as const },
      { ...reading(), range: "high" as const },
      { ...reading(), target_low: 71 },
      { ...reading(), target_high: 181 },
      { ...reading(), timestamp: "2026-01-01T00:00:01.000Z" },
      { ...reading(), libre_local_time: "01:00:00" },
      { ...reading(), source: "mock" as const },
    ];
    for (const mutation of stableMutations) await store.putIfNewer(mutation);

    expect(identities[0]).toBe(identities[1]);
    for (const identity of identities.slice(2)) expect(identity).not.toBe(identities[0]);
  });

  it("rejects invalid timestamps before touching Redis", async () => {
    let called = false;
    const fetcher: FetchLike = async () => {
      called = true;
      return response({ result: 1 });
    };
    const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });

    await expect(store.putIfNewer(reading("not-a-time"))).rejects.toThrow("invalid reading timestamp");
    expect(called).toBe(false);
  });

  it("rejects HTTP, Upstash, and response-shape errors without leaking the token", async () => {
    const fetchers: FetchLike[] = [
      async () => response({ result: null }, 500),
      async () => response({ error: "private provider detail" }),
      async () => response({ result: { unexpected: true } }),
      async () => new Response("not json", { status: 200 }),
    ];

    for (const fetcher of fetchers) {
      const store = createRedisReadingStoreFromEnvironment(ENV, { fetcher });
      let message = "";
      try {
        await store.get();
      } catch (error) {
        message = error instanceof Error ? error.message : String(error);
      }
      expect(message).toBeTruthy();
      expect(message).not.toContain("private-test-token");
      expect(message).not.toContain("private provider detail");
    }
  });
});
