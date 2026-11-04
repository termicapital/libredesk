import { createRedisIngestRateLimiterFromEnvironment } from "./redis-rate-limiter.js";
import { createRedisReadingStoreFromEnvironment } from "./redis-store.js";
import {
  createStoredBridge,
  type ReadingStore,
  type SharedRateLimiter,
  type StoredBridge,
} from "./stored.js";

export const VERCEL_BRIDGE_VERSION = "0.3.0-vercel";

export class VercelStoredConfigError extends Error {
  constructor(message: string) {
    super(`invalid stored bridge configuration: ${message}`);
    this.name = "VercelStoredConfigError";
  }
}

export interface VercelStoredBridgeDependencies {
  storeFactory?: (env: Readonly<Record<string, string | undefined>>) => ReadingStore;
  sharedRateLimiterFactory?: (
    env: Readonly<Record<string, string | undefined>>,
  ) => SharedRateLimiter;
  clock?: () => Date;
  requestId?: () => string;
}

let defaultBridge: StoredBridge | undefined;

export function createStoredBridgeFromEnvironment(
  env: Readonly<Record<string, string | undefined>>,
  dependencies: VercelStoredBridgeDependencies = {},
): StoredBridge {
  const deviceTokenHashes = parseHashes(env.DEVICE_TOKEN_HASHES, "DEVICE_TOKEN_HASHES");
  const ingestTokenHashes = parseHashes(env.INGEST_TOKEN_HASHES, "INGEST_TOKEN_HASHES");
  const overlap = deviceTokenHashes.find((hash) => ingestTokenHashes.includes(hash));
  if (overlap) {
    throw new VercelStoredConfigError("device and ingestion token hashes must be distinct");
  }
  if (!env.KV_REST_API_URL?.trim()) {
    throw new VercelStoredConfigError("KV_REST_API_URL is required");
  }
  if (!env.KV_REST_API_TOKEN?.trim()) {
    throw new VercelStoredConfigError("KV_REST_API_TOKEN is required");
  }

  const maxAgeS = parseInteger(env.MAX_AGE_S ?? "3600", "MAX_AGE_S", 300, 86_400);
  const pollAfterS = parseInteger(env.POLL_AFTER_S ?? "60", "POLL_AFTER_S", 20, 300);
  const store = (dependencies.storeFactory ?? createRedisReadingStoreFromEnvironment)(env);
  const ingestSharedRateLimiter = (
    dependencies.sharedRateLimiterFactory ?? createRedisIngestRateLimiterFromEnvironment
  )(env);

  return createStoredBridge({
    store,
    deviceTokenHashes,
    ingestTokenHashes,
    ingestSharedRateLimiter,
    maxAgeS,
    pollAfterS,
    version: VERCEL_BRIDGE_VERSION,
    ...(dependencies.clock !== undefined ? { clock: dependencies.clock } : {}),
    ...(dependencies.requestId !== undefined ? { requestId: dependencies.requestId } : {}),
  });
}

export function getDefaultStoredBridge(): StoredBridge {
  defaultBridge ??= createStoredBridgeFromEnvironment(process.env);
  return defaultBridge;
}

function parseHashes(raw: string | undefined, name: string): string[] {
  if (!raw?.trim()) throw new VercelStoredConfigError(`${name} is required`);
  const hashes = raw.split(",").map((value) => value.trim().toLowerCase());
  if (hashes.length < 1 || hashes.length > 4 || hashes.some((hash) => !/^[a-f0-9]{64}$/.test(hash))) {
    throw new VercelStoredConfigError(`${name} must contain one to four SHA-256 hex digests`);
  }
  if (new Set(hashes).size !== hashes.length) {
    throw new VercelStoredConfigError(`${name} contains duplicate digests`);
  }
  return hashes;
}

function parseInteger(
  raw: string,
  name: string,
  minimum: number,
  maximum: number,
): number {
  if (!/^\d+$/.test(raw)) {
    throw new VercelStoredConfigError(`${name} must be an integer`);
  }
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new VercelStoredConfigError(`${name} must be between ${minimum} and ${maximum}`);
  }
  return value;
}
