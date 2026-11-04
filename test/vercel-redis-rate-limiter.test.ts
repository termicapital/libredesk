import { describe, expect, it } from "vitest";
import {
  RedisRateLimitConfigError,
  RedisRateLimitError,
  createRedisIngestRateLimiterFromEnvironment,
} from "../bridge/src/vercel/redis-rate-limiter.js";
import type { FetchLike } from "../bridge/src/vercel/redis-store.js";

const ENV = {
  KV_REST_API_URL: "https://redis.example.test",
  KV_REST_API_TOKEN: "redis-token",
} as const;
const TOKEN_HASH = "a".repeat(64);
const NOW_MS = Date.parse("2026-01-01T00:30:00.000Z");

function response(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

describe("Upstash REST ingestion rate limiter", () => {
  it("rejects absent, insecure, or malformed configuration", () => {
    for (const env of [
      {},
      { KV_REST_API_URL: "https://redis.example.test" },
      { KV_REST_API_URL: "http://redis.example.test", KV_REST_API_TOKEN: "token" },
      { KV_REST_API_URL: "not-a-url", KV_REST_API_TOKEN: "token" },
    ]) {
      expect(() => createRedisIngestRateLimiterFromEnvironment(env))
        .toThrow(RedisRateLimitConfigError);
    }
  });

  it("uses one atomic fixed-window command keyed by token hash and time bucket", async () => {
    let command: unknown[] | undefined;
    const fetcher: FetchLike = async (_input, init) => {
      command = JSON.parse(String(init?.body)) as unknown[];
      return response({ result: 1 });
    };
    const limiter = createRedisIngestRateLimiterFromEnvironment(ENV, { fetcher });

    await expect(limiter.check(TOKEN_HASH, NOW_MS)).resolves.toEqual({
      allowed: true,
      retryAfterS: 0,
    });
    expect(command?.[0]).toBe("EVAL");
    expect(command?.[2]).toBe("1");
    expect(command?.[3]).toBe(`libre-desk:ingest-rate:${TOKEN_HASH}:${Math.floor(NOW_MS / 3_600_000)}`);
    expect(command?.[4]).toBe("120");
    expect(command?.[5]).toBe("3600");
    expect(String(command?.[1])).toContain("redis.call('INCR', KEYS[1])");
    expect(String(command?.[1])).toContain("redis.call('EXPIRE', KEYS[1], ARGV[2])");
  });

  it("returns deterministic retry time when the shared window is exhausted", async () => {
    const fetcher: FetchLike = async () => response({ result: 0 });
    const limiter = createRedisIngestRateLimiterFromEnvironment(ENV, { fetcher });

    await expect(limiter.check(TOKEN_HASH, NOW_MS)).resolves.toEqual({
      allowed: false,
      retryAfterS: 1_800,
    });
  });

  it("fails closed for invalid token hashes and store failures", async () => {
    let calls = 0;
    const fetcher: FetchLike = async () => {
      calls += 1;
      return response({ result: 7 });
    };
    const limiter = createRedisIngestRateLimiterFromEnvironment(ENV, { fetcher });

    await expect(limiter.check("bad", NOW_MS)).rejects.toThrow(RedisRateLimitError);
    expect(calls).toBe(0);
    await expect(limiter.check(TOKEN_HASH, NOW_MS)).rejects.toThrow(RedisRateLimitError);
    expect(calls).toBe(1);
  });
});
