import { createHash, timingSafeEqual } from "node:crypto";

const DEVICE_TOKEN_PATTERN = /^[A-Za-z0-9_-]{43}$/;
const HOUR_MS = 60 * 60 * 1000;
const BURST_MS = 1_000;

export interface AuthDecision {
  authenticated: boolean;
  statusCode: 401 | 403;
  tokenHash?: string;
}

export function hashDeviceToken(token: string): string {
  return createHash("sha256").update(token, "utf8").digest("hex");
}

function isCanonicalDeviceToken(token: string): boolean {
  if (!DEVICE_TOKEN_PATTERN.test(token)) return false;
  const decoded = Buffer.from(token, "base64url");
  return decoded.length === 32 && decoded.toString("base64url") === token;
}

export function authorizeBearer(
  authorization: string | undefined,
  activeTokenHashes: ReadonlyArray<string>,
): AuthDecision {
  if (!authorization) return { authenticated: false, statusCode: 401 };
  const match = /^Bearer\s+(.+)$/i.exec(authorization);
  const token = match?.[1];
  if (!token || !isCanonicalDeviceToken(token)) {
    return { authenticated: false, statusCode: 401 };
  }

  const tokenHash = hashDeviceToken(token);
  let matched = 0;
  const candidate = Buffer.from(tokenHash, "hex");
  for (const activeHash of activeTokenHashes) {
    const stored = Buffer.from(activeHash, "hex");
    const equal = stored.length === candidate.length && timingSafeEqual(stored, candidate);
    matched |= equal ? 1 : 0;
  }
  if (matched === 1) return { authenticated: true, statusCode: 403, tokenHash };
  return { authenticated: false, statusCode: 403, tokenHash };
}

export interface RateLimitOptions {
  maxPerHour?: number;
  burst?: number;
  windowMs?: number;
}

export interface RateLimitDecision {
  allowed: boolean;
  retryAfterS: number;
}

export class TokenRateLimiter {
  private readonly windows = new Map<string, number[]>();
  private readonly maxPerHour: number;
  private readonly burst: number;
  private readonly windowMs: number;

  constructor(options: RateLimitOptions = {}) {
    this.maxPerHour = options.maxPerHour ?? 120;
    this.burst = options.burst ?? 10;
    this.windowMs = options.windowMs ?? HOUR_MS;
  }

  check(tokenHash: string, nowMs = Date.now()): RateLimitDecision {
    const existing = this.windows.get(tokenHash) ?? [];
    const windowStart = nowMs - this.windowMs;
    const timestamps = existing.filter((timestamp) => timestamp > windowStart);
    const burstStart = nowMs - BURST_MS;
    const burstCount = timestamps.filter((timestamp) => timestamp > burstStart).length;
    if (timestamps.length >= this.maxPerHour || burstCount >= this.burst) {
      const threshold = timestamps.length >= this.maxPerHour ? windowStart : burstStart;
      const nextAllowed = timestamps.find((timestamp) => timestamp > threshold) ?? nowMs + 1_000;
      this.windows.set(tokenHash, timestamps);
      return { allowed: false, retryAfterS: Math.max(1, Math.ceil((nextAllowed + (timestamps.length >= this.maxPerHour ? this.windowMs : BURST_MS) - nowMs) / 1000)) };
    }
    timestamps.push(nowMs);
    this.windows.set(tokenHash, timestamps);
    return { allowed: true, retryAfterS: 0 };
  }
}
