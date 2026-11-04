import { createHash } from "node:crypto";
import type { LatestReading } from "../domain/types.js";
import type { ReadingStore, StoreWriteResult } from "./stored.js";

const PAYLOAD_KEY = "libre-desk:latest";
const TIMESTAMP_KEY = "libre-desk:latest:timestamp-ms";
const IDENTITY_KEY = "libre-desk:latest:identity-sha256";
const DEFAULT_TTL_S = 86_400;
const DEFAULT_TIMEOUT_MS = 5_000;

const MONOTONIC_WRITE_SCRIPT = [
  "local currentPayload = redis.call('GET', KEYS[1])",
  "local currentTimestamp = redis.call('GET', KEYS[2])",
  "local currentIdentity = redis.call('GET', KEYS[3])",
  "local incomingNumber = tonumber(ARGV[2])",
  "if not incomingNumber then return -2 end",
  "if not currentTimestamp then",
  "  if currentPayload or currentIdentity then return -2 end",
  "else",
  "  local currentNumber = tonumber(currentTimestamp)",
  "  if not currentNumber then return -2 end",
  "  if currentNumber > incomingNumber then return -1 end",
  "  if currentNumber == incomingNumber then",
  "    if not currentIdentity then return -2 end",
  "    if currentIdentity == ARGV[3] then",
  "      redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[4])",
  "      redis.call('SET', KEYS[2], ARGV[2], 'EX', ARGV[4])",
  "      redis.call('SET', KEYS[3], ARGV[3], 'EX', ARGV[4])",
  "      return 0",
  "    end",
  "    return -2",
  "  end",
  "end",
  "redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[4])",
  "redis.call('SET', KEYS[2], ARGV[2], 'EX', ARGV[4])",
  "redis.call('SET', KEYS[3], ARGV[3], 'EX', ARGV[4])",
  "return 1",
].join("\n");

export type FetchLike = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export class RedisConfigError extends Error {
  constructor(message: string) {
    super(`invalid Redis configuration: ${message}`);
    this.name = "RedisConfigError";
  }
}

export class RedisStoreError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "RedisStoreError";
  }
}

export interface RedisStoreDependencies {
  fetcher?: FetchLike;
  ttlS?: number;
  timeoutMs?: number;
}

export function createRedisReadingStoreFromEnvironment(
  env: Readonly<Record<string, string | undefined>>,
  dependencies: RedisStoreDependencies = {},
): ReadingStore {
  const rawUrl = env.KV_REST_API_URL?.trim();
  const token = env.KV_REST_API_TOKEN?.trim();
  if (!rawUrl) throw new RedisConfigError("KV_REST_API_URL is required");
  if (!token) throw new RedisConfigError("KV_REST_API_TOKEN is required");

  let parsedUrl: URL;
  try {
    parsedUrl = new URL(rawUrl);
  } catch {
    throw new RedisConfigError("KV_REST_API_URL must be an absolute HTTPS URL");
  }
  if (parsedUrl.protocol !== "https:") {
    throw new RedisConfigError("KV_REST_API_URL must use HTTPS");
  }
  if (parsedUrl.username || parsedUrl.password || parsedUrl.search || parsedUrl.hash) {
    throw new RedisConfigError("KV_REST_API_URL contains forbidden URL components");
  }

  const fetcher = dependencies.fetcher ?? globalThis.fetch.bind(globalThis);
  const ttlS = dependencies.ttlS ?? DEFAULT_TTL_S;
  const timeoutMs = dependencies.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  if (!Number.isInteger(ttlS) || ttlS < 3_600 || ttlS > 604_800) {
    throw new RedisConfigError("ttlS must be an integer between 3600 and 604800");
  }
  if (!Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 10_000) {
    throw new RedisConfigError("timeoutMs must be an integer between 100 and 10000");
  }
  const endpoint = parsedUrl.toString().replace(/\/$/, "");

  async function command(args: ReadonlyArray<unknown>): Promise<unknown> {
    let response: Response;
    try {
      response = await fetcher(endpoint, {
        method: "POST",
        headers: {
          authorization: `Bearer ${token}`,
          "content-type": "application/json",
        },
        body: JSON.stringify(args),
        signal: AbortSignal.timeout(timeoutMs),
      });
    } catch {
      throw new RedisStoreError("persistent store request failed");
    }
    if (!response.ok) throw new RedisStoreError("persistent store returned an error");

    let payload: unknown;
    try {
      payload = await response.json() as unknown;
    } catch {
      throw new RedisStoreError("persistent store returned malformed JSON");
    }
    if (!isRecord(payload) || !("result" in payload) || payload.error !== undefined) {
      throw new RedisStoreError("persistent store returned an invalid response");
    }
    return payload.result;
  }

  return {
    async get() {
      const result = await command(["GET", PAYLOAD_KEY]);
      if (result === null) return undefined;
      if (typeof result !== "string") {
        throw new RedisStoreError("persistent store returned an invalid value");
      }
      try {
        const parsed = JSON.parse(result) as unknown;
        if (!isRecord(parsed)) throw new Error("not an object");
        return parsed;
      } catch {
        throw new RedisStoreError("persistent store returned malformed data");
      }
    },

    async putIfNewer(reading: LatestReading): Promise<StoreWriteResult> {
      const timestampMs = Date.parse(reading.timestamp);
      if (!Number.isFinite(timestampMs)) throw new RedisStoreError("invalid reading timestamp");
      const identity = stableReadingIdentity(reading);
      const result = await command([
        "EVAL",
        MONOTONIC_WRITE_SCRIPT,
        "3",
        PAYLOAD_KEY,
        TIMESTAMP_KEY,
        IDENTITY_KEY,
        JSON.stringify(reading),
        String(timestampMs),
        identity,
        String(ttlS),
      ]);
      if (result === 1) return "stored";
      if (result === 0) return "duplicate";
      if (result === -1) return "older";
      if (result === -2) return "conflict";
      throw new RedisStoreError("persistent store returned an unknown write result");
    },
  };
}

function stableReadingIdentity(reading: LatestReading): string {
  const canonical = JSON.stringify({
    glucose: reading.glucose,
    unit: reading.unit,
    trend: reading.trend,
    range: reading.range,
    target_low: reading.target_low,
    target_high: reading.target_high,
    timestamp: reading.timestamp,
    libre_local_time: reading.libre_local_time,
    source: reading.source,
    schema: reading.schema,
  });
  return createHash("sha256").update(canonical, "utf8").digest("hex");
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
