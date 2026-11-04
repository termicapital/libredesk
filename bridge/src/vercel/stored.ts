import { randomUUID } from "node:crypto";
import { z } from "zod";
import type { LatestReading } from "../domain/types.js";
import {
  toLatestReadingV1,
  type DisplayContractVersion,
  type LatestReadingV1,
} from "../domain/version.js";
import {
  authorizeBearer,
  TokenRateLimiter,
  type RateLimitDecision,
  type RateLimitOptions,
} from "../http/auth.js";

const ISO_UTC_PATTERN = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?Z$/;
const FUTURE_SKEW_MS = 120_000;

const CommonReadingFields = {
  glucose: z.number().finite(),
  unit: z.enum(["mg/dL", "mmol/L"]),
  trend: z.enum([
    "rising",
    "rising_slightly",
    "stable",
    "falling_slightly",
    "falling",
    "rising_fast",
    "falling_fast",
    "unknown",
  ]),
  range: z.enum(["urgent_low", "low", "in_range", "high", "unknown"]),
  target_low: z.number().finite().nonnegative().nullable(),
  target_high: z.number().finite().nonnegative().nullable(),
  timestamp: z.string().regex(ISO_UTC_PATTERN),
  age_s: z.number().int().nonnegative(),
  server_time: z.string().regex(ISO_UTC_PATTERN),
  source: z.enum(["librelinkup", "mock"]),
  poll_after_s: z.number().int().min(20).max(300),
} as const;

const ReadingSchema = z.object({
  ...CommonReadingFields,
  libre_local_time: z.string().regex(/^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$/),
  schema: z.literal(2),
}).strict();

const LegacyReadingSchema = z.object({
  ...CommonReadingFields,
  schema: z.literal(1),
}).strict();

export type StoreWriteResult = "stored" | "duplicate" | "older" | "conflict";

export interface ReadingStore {
  get(): Promise<unknown | undefined>;
  putIfNewer(reading: LatestReading): Promise<StoreWriteResult>;
}

export interface SharedRateLimiter {
  check(tokenHash: string, nowMs: number): Promise<RateLimitDecision>;
}

export interface StoredRequest {
  method: string | undefined;
  headers: Readonly<Record<string, string | undefined>>;
  body?: unknown;
}

export interface StoredResponse {
  statusCode: number;
  headers: Record<string, string>;
  body?: unknown;
}

export interface StoredBridge {
  latest(request: StoredRequest, version?: DisplayContractVersion): Promise<StoredResponse>;
  ingest(request: StoredRequest): Promise<StoredResponse>;
  health(): Promise<StoredResponse>;
}

export interface StoredBridgeOptions {
  store: ReadingStore;
  deviceTokenHashes: ReadonlyArray<string>;
  ingestTokenHashes: ReadonlyArray<string>;
  maxAgeS?: number;
  pollAfterS?: number;
  version?: string;
  clock?: () => Date;
  requestId?: () => string;
  deviceRateLimit?: RateLimitOptions;
  ingestRateLimit?: RateLimitOptions;
  ingestSharedRateLimiter?: SharedRateLimiter;
}

export function createStoredBridge(options: StoredBridgeOptions): StoredBridge {
  const maxAgeS = options.maxAgeS ?? 3_600;
  const pollAfterS = options.pollAfterS ?? 60;
  const version = options.version ?? "0.3.0-vercel";
  const clock = options.clock ?? (() => new Date());
  const requestId = options.requestId ?? randomUUID;
  const deviceLimiter = new TokenRateLimiter(options.deviceRateLimit);
  const ingestLimiter = new TokenRateLimiter(options.ingestRateLimit);

  return {
    async latest(request, contractVersion = 1) {
      if (request.method?.toUpperCase() !== "GET") {
        return errorResponse(405, "method_not_allowed", "GET required", 0, { allow: "GET" });
      }
      const authenticated = authenticate(
        request.headers.authorization,
        options.deviceTokenHashes,
        deviceLimiter,
        clock,
      );
      if ("response" in authenticated) return authenticated.response;

      let raw: unknown;
      try {
        raw = await options.store.get();
      } catch {
        return errorResponse(503, "upstream_unavailable", "no validated reading within max_age", 30);
      }
      if (raw === undefined) {
        return errorResponse(503, "upstream_unavailable", "no validated reading within max_age", 30);
      }
      const parsed = validateStoredReading(raw, clock(), maxAgeS, pollAfterS);
      if (!parsed.ok) {
        return parsed.reason === "stale"
          ? errorResponse(503, "upstream_unavailable", "no validated reading within max_age", 30)
          : errorResponse(503, "malformed_upstream", "stored reading failed strict validation", 60);
      }

      const headers: Record<string, string> = {
        "cache-control": "no-store",
        "x-request-id": requestId(),
      };
      if (contractVersion === 2 && parsed.reading.schema === 1) {
        return errorResponse(503, "malformed_upstream", "stored reading failed strict validation", 60);
      }
      const body = contractVersion === 1 && parsed.reading.schema === 2
        ? toLatestReadingV1(parsed.reading)
        : parsed.reading;
      return { statusCode: 200, headers, body };
    },

    async ingest(request) {
      if (request.method?.toUpperCase() !== "POST") {
        return errorResponse(405, "method_not_allowed", "POST required", 0, { allow: "POST" });
      }
      const authenticated = authenticate(
        request.headers.authorization,
        options.ingestTokenHashes,
        ingestLimiter,
        clock,
      );
      if ("response" in authenticated) return authenticated.response;

      if (options.ingestSharedRateLimiter) {
        let rate: RateLimitDecision;
        try {
          rate = await options.ingestSharedRateLimiter.check(
            authenticated.tokenHash,
            clock().getTime(),
          );
        } catch {
          return errorResponse(
            503,
            "rate_limit_unavailable",
            "shared rate limit unavailable",
            30,
          );
        }
        if (!rate.allowed) {
          return errorResponse(
            429,
            "rate_limited",
            "request rate limit exceeded",
            rate.retryAfterS,
            { "retry-after": String(rate.retryAfterS) },
          );
        }
      }

      const parsed = validateReading(request.body, clock(), maxAgeS, pollAfterS);
      if (!parsed.ok || parsed.reading.source !== "librelinkup") {
        return errorResponse(400, "invalid_payload", "reading payload rejected", 0);
      }

      let result: StoreWriteResult;
      try {
        result = await options.store.putIfNewer(parsed.reading);
      } catch {
        return errorResponse(503, "storage_unavailable", "persistent store unavailable", 30);
      }
      if (result === "older" || result === "conflict") {
        return errorResponse(409, "non_monotonic_timestamp", "stored reading is newer or conflicts", 0);
      }
      return {
        statusCode: 202,
        headers: { "cache-control": "no-store" },
        body: { accepted: true, result },
      };
    },

    async health() {
      let raw: unknown;
      try {
        raw = await options.store.get();
      } catch {
        raw = undefined;
      }
      const parsed = raw === undefined
        ? { ok: false as const }
        : validateStoredReading(raw, clock(), maxAgeS, pollAfterS);
      return {
        statusCode: 200,
        headers: { "cache-control": "no-store" },
        body: {
          ok: true,
          upstream_ok: parsed.ok,
          last_success_age_s: parsed.ok ? parsed.reading.age_s : null,
          version,
          mode: "stored_push",
        },
      };
    },
  };
}

function authenticate(
  authorization: string | undefined,
  hashes: ReadonlyArray<string>,
  limiter: TokenRateLimiter,
  clock: () => Date,
): { tokenHash: string } | { response: StoredResponse } {
  const decision = authorizeBearer(authorization, hashes);
  if (!decision.authenticated || !decision.tokenHash) {
    return {
      response: errorResponse(
        decision.statusCode,
        decision.statusCode === 403 ? "revoked_token" : "invalid_token",
        "device token rejected",
        0,
      ),
    };
  }
  const rate = limiter.check(decision.tokenHash, clock().getTime());
  if (!rate.allowed) {
    return {
      response: errorResponse(
        429,
        "rate_limited",
        "request rate limit exceeded",
        rate.retryAfterS,
        { "retry-after": String(rate.retryAfterS) },
      ),
    };
  }
  return { tokenHash: decision.tokenHash };
}

type StoredReading = LatestReading | LatestReadingV1;

type StoredValidationResult<T extends StoredReading = StoredReading> =
  | { ok: true; reading: T }
  | { ok: false; reason: "malformed" | "stale" };

function validateStoredReading(
  raw: unknown,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult {
  const current = validateReading(raw, now, maxAgeS, pollAfterS);
  if (current.ok || current.reason === "stale") return current;
  return validateLegacyReading(raw, now, maxAgeS, pollAfterS);
}

function validateReading(
  raw: unknown,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult<LatestReading> {
  const parsed = ReadingSchema.safeParse(raw);
  if (!parsed.success) return { ok: false, reason: "malformed" };
  const value: LatestReading = parsed.data;
  return validateParsedReading(value, now, maxAgeS, pollAfterS);
}

function validateLegacyReading(
  raw: unknown,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult<LatestReadingV1> {
  const parsed = LegacyReadingSchema.safeParse(raw);
  if (!parsed.success) return { ok: false, reason: "malformed" };
  const value: LatestReadingV1 = parsed.data;
  return validateParsedReading(value, now, maxAgeS, pollAfterS);
}

function validateParsedReading(
  value: LatestReading,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult<LatestReading>;
function validateParsedReading(
  value: LatestReadingV1,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult<LatestReadingV1>;
function validateParsedReading(
  value: StoredReading,
  now: Date,
  maxAgeS: number,
  pollAfterS: number,
): StoredValidationResult {
  const timestampMs = Date.parse(value.timestamp);
  const serverTimeMs = Date.parse(value.server_time);
  const nowMs = now.getTime();
  if (![timestampMs, serverTimeMs, nowMs].every(Number.isFinite)) {
    return { ok: false, reason: "malformed" };
  }
  if (timestampMs > nowMs + FUTURE_SKEW_MS) return { ok: false, reason: "malformed" };

  const upper = value.unit === "mg/dL" ? 500 : 28;
  const lower = value.unit === "mg/dL" ? 20 : 1;
  if (value.glucose < lower || value.glucose > upper) return { ok: false, reason: "malformed" };
  for (const target of [value.target_low, value.target_high]) {
    if (target !== null && target > upper) return { ok: false, reason: "malformed" };
  }
  if (
    value.target_low !== null
    && value.target_high !== null
    && value.target_low > value.target_high
  ) {
    return { ok: false, reason: "malformed" };
  }

  const ageS = Math.max(0, Math.floor((nowMs - timestampMs) / 1_000));
  if (ageS > maxAgeS) return { ok: false, reason: "stale" };
  return {
    ok: true,
    reading: {
      ...value,
      age_s: ageS,
      server_time: now.toISOString(),
      poll_after_s: pollAfterS,
    },
  };
}

function errorResponse(
  statusCode: number,
  error: string,
  detail: string,
  retryAfterS: number,
  extraHeaders: Record<string, string> = {},
): StoredResponse {
  return {
    statusCode,
    headers: { "cache-control": "no-store", ...extraHeaders },
    body: { error, detail, retry_after_s: retryAfterS },
  };
}
