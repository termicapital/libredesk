import { createHash, randomUUID } from "node:crypto";
import Fastify, {
  LogController,
  type FastifyInstance,
  type FastifyReply,
  type FastifyRequest,
} from "fastify";
import { z } from "zod";
import type { LatestCache } from "../cache.js";
import type { LatestReading } from "../domain/types.js";
import { selectLatestReading, type DisplayContractVersion } from "../domain/version.js";
import {
  authorizeBearer,
  TokenRateLimiter,
  type RateLimitOptions,
} from "./auth.js";

const TelemetrySchema = z.object({
  firmware_version: z.string().min(1).max(64),
  uptime_s: z.number().int().nonnegative(),
  min_free_heap: z.number().int().nonnegative(),
  wifi_rssi: z.number().int().max(0),
  reconnect_count: z.number().int().nonnegative(),
  http_errors: z.record(z.string().regex(/^[a-z0-9_]{1,32}$/), z.number().int().nonnegative()),
}).strict();
const MAX_COREDUMP_BASE64_CHARS = 524_288;
const DEFAULT_TELEMETRY_INTERVAL_S = 300;
const DEFAULT_COREDUMP_INTERVAL_S = 3_600;

const CoredumpSchema = z.object({
  firmware_version: z.string().min(1).max(64),
  reset_reason: z.string().min(1).max(64),
  data_base64: z.string().regex(/^[A-Za-z0-9+/]*={0,2}$/).max(MAX_COREDUMP_BASE64_CHARS),
}).strict();

export type DeviceTelemetry = z.infer<typeof TelemetrySchema>;
export type DeviceCoredump = z.infer<typeof CoredumpSchema>;

export interface HealthSnapshot {
  upstreamOk: boolean;
  lastSuccessAt?: Date;
}

export type UnavailableErrorCode =
  | "upstream_unavailable"
  | "upstream_auth_failed"
  | "malformed_upstream"
  | "terms_of_use_required";

export interface UnavailableError {
  code: UnavailableErrorCode;
  detail: string;
  retryAfterS: number;
}

export interface OtaManifest {
  version: string;
  url: string;
  sha256: string;
  min_prev_version: string;
}

export interface BridgeServerOptions {
  cache: LatestCache;
  activeTokenHashes: ReadonlyArray<string>;
  maxAgeS?: number;
  version?: string;
  clock?: () => Date;
  health?: () => HealthSnapshot;
  unavailableError?: () => UnavailableError;
  metrics?: () => string;
  otaManifest?: () => OtaManifest | undefined;
  rateLimit?: RateLimitOptions;
  telemetryMinIntervalS?: number;
  coredumpMinIntervalS?: number;
  onTelemetry?: (telemetry: DeviceTelemetry) => void | Promise<void>;
  onCoredump?: (coredump: DeviceCoredump) => void | Promise<void>;
  onDeviceResponse?: (statusCode: number) => void;
  onCacheLookup?: (hit: boolean) => void;
}

export interface BridgeServer {
  app: FastifyInstance;
}

export function createBridgeServer(options: BridgeServerOptions): BridgeServer {
  const app = Fastify({ logger: false, logController: new LogController({ disableRequestLogging: true }) });
  const cacheMaxAgeS = options.maxAgeS ?? 3_600;
  const clock = options.clock ?? (() => new Date());
  const limiter = new TokenRateLimiter(options.rateLimit);
  const telemetryLastAcceptedMs = new Map<string, number>();
  const coredumpLastAcceptedMs = new Map<string, number>();
  const telemetryIntervalMs = (options.telemetryMinIntervalS ?? DEFAULT_TELEMETRY_INTERVAL_S) * 1000;
  const coredumpIntervalMs = (options.coredumpMinIntervalS ?? DEFAULT_COREDUMP_INTERVAL_S) * 1000;
  const version = options.version ?? "0.1.0";
  const health: () => HealthSnapshot = options.health ?? (() => ({ upstreamOk: false }));

  const latestHandler = async (
    request: FastifyRequest,
    reply: FastifyReply,
    contractVersion: DisplayContractVersion,
  ) => {
    const auth = requireToken(request, reply, options, limiter);
    if (!auth) return;

    const now = clock();
    const reading = options.cache.get(now, cacheMaxAgeS);
    options.onCacheLookup?.(reading !== undefined);
    if (!reading) {
      const unavailable = options.unavailableError?.() ?? {
        code: "upstream_unavailable" as const,
        detail: "no validated reading within max_age",
        retryAfterS: 30,
      };
      return sendError(reply, 503, unavailable.code, unavailable.detail, unavailable.retryAfterS);
    }
    const body = selectLatestReading(reading, contractVersion);
    const etag = readingEtag(body);
    const baseHeaders = {
      "cache-control": "no-store",
      etag,
      "x-request-id": randomUUID(),
    };
    if (request.headers["if-none-match"] === etag) {
      return reply
        .code(304)
        .headers({
          ...baseHeaders,
          "x-reading-server-time": reading.server_time,
          "x-reading-age-s": String(reading.age_s),
        })
        .send();
    }
    return reply.code(200).headers(baseHeaders).send(body);
  };

  app.get("/api/v1/latest", async (request, reply) => latestHandler(request, reply, 1));
  app.get("/api/v2/latest", async (request, reply) => latestHandler(request, reply, 2));

  app.get("/healthz", async (_request, reply) => {
    const snapshot = health();
    const now = clock();
    const lastSuccessAge = snapshot.lastSuccessAt
      ? Math.max(0, Math.floor((now.getTime() - snapshot.lastSuccessAt.getTime()) / 1000))
      : null;
    return reply.code(200).send({
      ok: true,
      upstream_ok: snapshot.upstreamOk,
      last_success_age_s: lastSuccessAge,
      version,
    });
  });

  app.get("/metrics", async (request, reply) => {
    const auth = requireToken(request, reply, options, limiter);
    if (!auth) return;
    return reply.code(200).type("text/plain; version=0.0.4").send(options.metrics?.() ?? "");
  });

  app.post("/api/v1/telemetry", async (request, reply) => {
    const tokenHash = requireToken(request, reply, options, limiter);
    if (!tokenHash) return;
    const parsed = TelemetrySchema.safeParse(request.body);
    if (!parsed.success) return sendError(reply, 400, "invalid_payload", "telemetry payload rejected", 0);
    const nowMs = clock().getTime();
    const claim = claimInterval(telemetryLastAcceptedMs, tokenHash, nowMs, telemetryIntervalMs);
    if (!claim.allowed) {
      return sendError(reply, 429, "rate_limited", "telemetry rate limit exceeded", claim.retryAfterS);
    }
    try {
      await options.onTelemetry?.(parsed.data);
    } catch (error) {
      releaseIntervalClaim(telemetryLastAcceptedMs, tokenHash, nowMs);
      throw error;
    }
    return reply.code(202).send();
  });

  app.post("/api/v1/coredump", async (request, reply) => {
    const tokenHash = requireToken(request, reply, options, limiter);
    if (!tokenHash) return;
    const body = request.body;
    if (
      typeof body === "object"
      && body !== null
      && "data_base64" in body
      && typeof body.data_base64 === "string"
      && body.data_base64.length > MAX_COREDUMP_BASE64_CHARS
    ) return sendError(reply, 413, "invalid_payload", "coredump payload is too large", 0);
    const parsed = CoredumpSchema.safeParse(body);
    if (!parsed.success) return sendError(reply, 400, "invalid_payload", "coredump payload rejected", 0);
    const nowMs = clock().getTime();
    const claim = claimInterval(coredumpLastAcceptedMs, tokenHash, nowMs, coredumpIntervalMs);
    if (!claim.allowed) {
      return sendError(reply, 429, "rate_limited", "coredump rate limit exceeded", claim.retryAfterS);
    }
    try {
      await options.onCoredump?.(parsed.data);
    } catch (error) {
      releaseIntervalClaim(coredumpLastAcceptedMs, tokenHash, nowMs);
      throw error;
    }
    return reply.code(202).send();
  });

  app.get("/api/v1/ota/manifest", async (request, reply) => {
    const auth = requireToken(request, reply, options, limiter);
    if (!auth) return;
    const manifest = options.otaManifest?.();
    if (!manifest) return sendError(reply, 503, "ota_unavailable", "no firmware release is available", 300);
    return reply.code(200).send(manifest);
  });

  app.setErrorHandler((_error, _request, reply) => {
    return sendError(reply, 500, "internal", "bridge internal error", 30);
  });

  app.addHook("onResponse", async (request, reply) => {
    if (request.url.startsWith("/api/v1/") || request.url.startsWith("/api/v2/")) options.onDeviceResponse?.(reply.statusCode);
  });

  return { app };
}

type IntervalClaim = { allowed: true } | { allowed: false; retryAfterS: number };

function claimInterval(
  acceptedAtMs: Map<string, number>,
  tokenHash: string,
  nowMs: number,
  intervalMs: number,
): IntervalClaim {
  if (!Number.isFinite(nowMs) || !Number.isFinite(intervalMs) || intervalMs <= 0) {
    return { allowed: false, retryAfterS: 1 };
  }
  const previousMs = acceptedAtMs.get(tokenHash);
  if (previousMs !== undefined && nowMs - previousMs < intervalMs) {
    const retryAfterS = Math.min(3_600, Math.max(1, Math.ceil((previousMs + intervalMs - nowMs) / 1000)));
    return { allowed: false, retryAfterS };
  }
  acceptedAtMs.set(tokenHash, nowMs);
  return { allowed: true };
}

function releaseIntervalClaim(acceptedAtMs: Map<string, number>, tokenHash: string, claimedAtMs: number): void {
  if (acceptedAtMs.get(tokenHash) === claimedAtMs) acceptedAtMs.delete(tokenHash);
}

function requireToken(
  request: FastifyRequest,
  reply: FastifyReply,
  options: BridgeServerOptions,
  limiter: TokenRateLimiter,
): string | undefined {
  const authorization = typeof request.headers.authorization === "string"
    ? request.headers.authorization
    : undefined;
  const decision = authorizeBearer(authorization, options.activeTokenHashes);
  if (!decision.authenticated || !decision.tokenHash) {
    const error = decision.statusCode === 403 ? "revoked_token" : "invalid_token";
    void sendError(reply, decision.statusCode, error, "device token rejected", 0);
    return undefined;
  }
  const rate = limiter.check(decision.tokenHash);
  if (!rate.allowed) {
    void sendError(reply, 429, "rate_limited", "device request rate limit exceeded", rate.retryAfterS);
    return undefined;
  }
  return decision.tokenHash;
}

function sendError(
  reply: FastifyReply,
  statusCode: number,
  error: string,
  detail: string,
  retryAfterS: number,
): FastifyReply {
  reply.header("cache-control", "no-store");
  if (statusCode === 429) reply.header("retry-after", String(retryAfterS));
  return reply.code(statusCode).send({ error, detail, retry_after_s: retryAfterS });
}

function readingEtag(reading: LatestReading | ReturnType<typeof selectLatestReading>): string {
  const canonical = JSON.stringify({
    glucose: reading.glucose,
    unit: reading.unit,
    trend: reading.trend,
    range: reading.range,
    target_low: reading.target_low,
    target_high: reading.target_high,
    timestamp: reading.timestamp,
    ...(reading.schema === 2 ? { libre_local_time: reading.libre_local_time } : {}),
    source: reading.source,
    schema: reading.schema,
  });
  return `"${createHash("sha256").update(canonical, "utf8").digest("hex")}"`;
}
