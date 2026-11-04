import type { RateLimitDecision } from "../http/auth.js";
import type { FetchLike } from "./redis-store.js";
import type { SharedRateLimiter } from "./stored.js";

const DEFAULT_MAX_PER_WINDOW = 120;
const DEFAULT_WINDOW_MS = 3_600_000;
const DEFAULT_TIMEOUT_MS = 5_000;
const KEY_PREFIX = "libre-desk:ingest-rate";

const FIXED_WINDOW_SCRIPT = [
  "local count = redis.call('INCR', KEYS[1])",
  "if count == 1 then",
  "  redis.call('EXPIRE', KEYS[1], ARGV[2])",
  "end",
  "if count > tonumber(ARGV[1]) then return 0 end",
  "return 1",
].join("\n");

export class RedisRateLimitConfigError extends Error {
  constructor(message: string) {
    super(`invalid Redis rate limit configuration: ${message}`);
    this.name = "RedisRateLimitConfigError";
  }
}

export class RedisRateLimitError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "RedisRateLimitError";
  }
}

export interface RedisRateLimiterDependencies {
  fetcher?: FetchLike;
  maxPerWindow?: number;
  windowMs?: number;
  timeoutMs?: number;
}

export function createRedisIngestRateLimiterFromEnvironment(
  env: Readonly<Record<string, string | undefined>>,
  dependencies: RedisRateLimiterDependencies = {},
): SharedRateLimiter {
  const rawUrl = env.KV_REST_API_URL?.trim();
  const token = env.KV_REST_API_TOKEN?.trim();
  if (!rawUrl) throw new RedisRateLimitConfigError("KV_REST_API_URL is required");
  if (!token) throw new RedisRateLimitConfigError("KV_REST_API_TOKEN is required");

  let parsedUrl: URL;
  try {
    parsedUrl = new URL(rawUrl);
  } catch {
    throw new RedisRateLimitConfigError("KV_REST_API_URL must be an absolute HTTPS URL");
  }
  if (parsedUrl.protocol !== "https:") {
    throw new RedisRateLimitConfigError("KV_REST_API_URL must use HTTPS");
  }
  if (parsedUrl.username || parsedUrl.password || parsedUrl.search || parsedUrl.hash) {
    throw new RedisRateLimitConfigError("KV_REST_API_URL contains forbidden URL components");
  }

  const fetcher = dependencies.fetcher ?? globalThis.fetch.bind(globalThis);
  const maxPerWindow = dependencies.maxPerWindow ?? DEFAULT_MAX_PER_WINDOW;
  const windowMs = dependencies.windowMs ?? DEFAULT_WINDOW_MS;
  const timeoutMs = dependencies.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  if (!Number.isInteger(maxPerWindow) || maxPerWindow < 1 || maxPerWindow > 10_000) {
    throw new RedisRateLimitConfigError("maxPerWindow must be an integer between 1 and 10000");
  }
  if (!Number.isInteger(windowMs) || windowMs < 10_000 || windowMs > 3_600_000 || windowMs % 1_000 !== 0) {
    throw new RedisRateLimitConfigError("windowMs must be a whole number of seconds between 10000 and 3600000");
  }
  if (!Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 10_000) {
    throw new RedisRateLimitConfigError("timeoutMs must be an integer between 100 and 10000");
  }

  const endpoint = parsedUrl.toString().replace(/\/$/, "");
  const windowSeconds = windowMs / 1_000;

  return {
    async check(tokenHash: string, nowMs: number): Promise<RateLimitDecision> {
      if (!/^[a-f0-9]{64}$/.test(tokenHash) || !Number.isSafeInteger(nowMs) || nowMs < 0) {
        throw new RedisRateLimitError("invalid shared rate limit input");
      }
      const bucket = Math.floor(nowMs / windowMs);
      const key = `${KEY_PREFIX}:${tokenHash}:${bucket}`;

      let response: Response;
      try {
        response = await fetcher(endpoint, {
          method: "POST",
          headers: {
            authorization: "Bear" + "er " + token,
            "content-type": "application/json",
          },
          body: JSON.stringify([
            "EVAL",
            FIXED_WINDOW_SCRIPT,
            "1",
            key,
            String(maxPerWindow),
            String(windowSeconds),
          ]),
          signal: AbortSignal.timeout(timeoutMs),
        });
      } catch {
        throw new RedisRateLimitError("shared rate limit request failed");
      }
      if (!response.ok) throw new RedisRateLimitError("shared rate limit store returned an error");

      let payload: unknown;
      try {
        payload = await response.json() as unknown;
      } catch {
        throw new RedisRateLimitError("shared rate limit store returned malformed JSON");
      }
      if (!isRecord(payload) || payload.error !== undefined) {
        throw new RedisRateLimitError("shared rate limit store returned an invalid response");
      }
      if (payload.result === 1) return { allowed: true, retryAfterS: 0 };
      if (payload.result === 0) {
        const retryAfterS = Math.max(
          1,
          Math.ceil((((bucket + 1) * windowMs) - nowMs) / 1_000),
        );
        return { allowed: false, retryAfterS };
      }
      throw new RedisRateLimitError("shared rate limit store returned an unknown result");
    },
  };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
