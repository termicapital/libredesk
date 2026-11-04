import type { DisplayUnit, ReadingSource } from "./domain/types.js";

export interface BridgeConfig {
  host: string;
  port: number;
  bridgeVersion: string;
  libreEmail: string;
  librePassword: string;
  lluProduct: string;
  lluAppVersion: string;
  upstreamBaseUrl: string;
  upstreamSource: ReadingSource;
  requestTimeoutMs: number;
  activeTokenHashes: string[];
  pollIntervalS: number;
  pollJitterS: number;
  maxBackoffS: number;
  maxAgeS: number;
  urgentLowMgDl: number;
  fallbackTargetLowMgDl: number | undefined;
  fallbackTargetHighMgDl: number | undefined;
  unitOverride: DisplayUnit | undefined;
  firmwareDir: string;
}

export class BridgeConfigError extends Error {
  constructor(variableNames: string, reason: string) {
    super(`invalid bridge configuration: ${variableNames} ${reason}`);
    this.name = "BridgeConfigError";
  }
}

export function loadBridgeConfig(
  env: Readonly<Record<string, string | undefined>> = process.env,
): BridgeConfig {
  const libreEmail = required(env, "LIBRE_EMAIL");
  const librePassword = required(env, "LIBRE_PASSWORD");
  const lluAppVersion = required(env, "LLU_APP_VERSION");
  const upstreamSource = enumValue(env.UPSTREAM_SOURCE ?? "librelinkup", "UPSTREAM_SOURCE", ["librelinkup", "mock"] as const);
  const upstreamBaseUrl = env.UPSTREAM_BASE_URL?.trim() || "https://api.libreview.io";
  validateUrl(upstreamBaseUrl, upstreamSource);

  const activeTokenHashes = required(env, "DEVICE_TOKEN_HASHES")
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean);
  if (activeTokenHashes.length < 1 || activeTokenHashes.length > 2 || activeTokenHashes.some((hash) => !/^[a-f0-9]{64}$/i.test(hash))) {
    throw new BridgeConfigError("DEVICE_TOKEN_HASHES", "must contain one or two comma-separated SHA-256 hex hashes");
  }

  const fallbackTargetLowMgDl = optionalNumber(env, "RANGE_LOW", 20, 500);
  const fallbackTargetHighMgDl = optionalNumber(env, "RANGE_HIGH", 20, 500);
  if ((fallbackTargetLowMgDl === undefined) !== (fallbackTargetHighMgDl === undefined)) {
    throw new BridgeConfigError("RANGE_LOW and RANGE_HIGH", "must be supplied together");
  }
  if (fallbackTargetLowMgDl !== undefined && fallbackTargetHighMgDl !== undefined && fallbackTargetLowMgDl > fallbackTargetHighMgDl) {
    throw new BridgeConfigError("RANGE_LOW and RANGE_HIGH", "must be ordered low to high");
  }

  const urgentLowMgDl = numberValue(env, "RANGE_URGENT_LOW", 70, 40, 100);
  if (fallbackTargetLowMgDl !== undefined && urgentLowMgDl > fallbackTargetLowMgDl) {
    throw new BridgeConfigError("RANGE_URGENT_LOW and RANGE_LOW", "must be ordered urgent-low at or below target low");
  }

  const unitOverride = env.DISPLAY_UNIT === undefined
    ? undefined
    : enumValue(env.DISPLAY_UNIT, "DISPLAY_UNIT", ["mg/dL", "mmol/L"] as const);
  const pollIntervalS = integer(env, "POLL_INTERVAL_S", 60, 20, 900);
  const pollJitterS = integer(env, "POLL_JITTER_S", 10, 0, pollIntervalS);
  const maxBackoffS = integer(env, "MAX_BACKOFF_S", 900, pollIntervalS, 3_600);

  return {
    host: env.HOST?.trim() || "0.0.0.0",
    port: integer(env, "PORT", 8080, 1, 65_535),
    bridgeVersion: env.BRIDGE_VERSION?.trim() || "0.1.0",
    libreEmail,
    librePassword,
    lluProduct: env.LLU_PRODUCT?.trim() || "llu.android",
    lluAppVersion,
    upstreamBaseUrl,
    upstreamSource,
    requestTimeoutMs: integer(env, "UPSTREAM_TIMEOUT_MS", 15_000, 100, 120_000),
    activeTokenHashes,
    pollIntervalS,
    pollJitterS,
    maxBackoffS,
    maxAgeS: integer(env, "MAX_AGE_S", 3_600, 60, 86_400),
    urgentLowMgDl,
    fallbackTargetLowMgDl,
    fallbackTargetHighMgDl,
    unitOverride,
    firmwareDir: env.FIRMWARE_DIR?.trim() || "/srv/firmware",
  };
}

function required(env: Readonly<Record<string, string | undefined>>, name: string): string {
  const value = env[name]?.trim();
  if (!value) throw new BridgeConfigError(name, "is required");
  return value;
}

function integer(
  env: Readonly<Record<string, string | undefined>>,
  name: string,
  fallback: number,
  minimum: number,
  maximum: number,
): number {
  const value = numberValue(env, name, fallback, minimum, maximum);
  if (!Number.isInteger(value)) throw new BridgeConfigError(name, "must be an integer");
  return value;
}

function optionalNumber(
  env: Readonly<Record<string, string | undefined>>,
  name: string,
  minimum: number,
  maximum: number,
): number | undefined {
  if (env[name] === undefined || env[name]?.trim() === "") return undefined;
  return numberValue(env, name, 0, minimum, maximum);
}

function numberValue(
  env: Readonly<Record<string, string | undefined>>,
  name: string,
  fallback: number,
  minimum: number,
  maximum: number,
): number {
  const raw = env[name];
  if (raw === undefined || raw.trim() === "") return fallback;
  const value = Number(raw);
  if (!Number.isFinite(value) || value < minimum || value > maximum) {
    throw new BridgeConfigError(name, `must be between ${minimum} and ${maximum}`);
  }
  return value;
}

function enumValue<const T extends readonly string[]>(value: string, name: string, allowed: T): T[number] {
  if (!allowed.includes(value as T[number])) {
    throw new BridgeConfigError(name, `must be one of ${allowed.join(", ")}`);
  }
  return value as T[number];
}

function validateUrl(value: string, source: ReadingSource): void {
  let parsed: URL;
  try {
    parsed = new URL(value);
  } catch {
    throw new BridgeConfigError("UPSTREAM_BASE_URL", "must be an absolute URL");
  }
  if (source === "librelinkup" && parsed.protocol !== "https:") {
    throw new BridgeConfigError("UPSTREAM_BASE_URL", "must use HTTPS for librelinkup");
  }
  if (source === "mock" && !["http:", "https:"].includes(parsed.protocol)) {
    throw new BridgeConfigError("UPSTREAM_BASE_URL", "must use HTTP or HTTPS");
  }
}
