const REDACTED = "[REDACTED]";
const JWT = /\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b/g;
const CANONICAL_DEVICE_TOKEN = /\b[A-Za-z0-9_-]{43}\b/g;
const SHA256_HEX = /\b[a-f0-9]{64}\b/gi;

export interface RedactingLogger {
  info(event: string, fields?: Record<string, unknown>): void;
  warn(event: string, fields?: Record<string, unknown>): void;
  error(event: string, fields?: Record<string, unknown>): void;
}

export type LogSink = (line: string) => void;

export function createRedactingLogger(
  sink: LogSink = (line) => process.stdout.write(`${line}\n`),
): RedactingLogger {
  const emit = (level: "info" | "warn" | "error", event: string, fields: Record<string, unknown> = {}) => {
    const record = {
      level,
      event: sanitizeString(event),
      ...redactObject(fields, new WeakSet<object>()),
    };
    sink(JSON.stringify(record));
  };
  return {
    info: (event, fields) => emit("info", event, fields),
    warn: (event, fields) => emit("warn", event, fields),
    error: (event, fields) => emit("error", event, fields),
  };
}

export function redactForLog(value: string): string;
export function redactForLog(value: unknown): unknown;
export function redactForLog(value: unknown): unknown {
  return redactValue(value, new WeakSet<object>());
}

function redactValue(value: unknown, seen: WeakSet<object>): unknown {
  if (typeof value === "string") return sanitizeString(value);
  if (typeof value !== "object" || value === null) return value;
  if (value instanceof Date) return value.toISOString();
  if (value instanceof Error) {
    return { name: value.name, message: sanitizeString(value.message) };
  }
  if (seen.has(value)) return "[CIRCULAR]";
  seen.add(value);
  if (Array.isArray(value)) return value.map((entry) => redactValue(entry, seen));
  return redactObject(value as Record<string, unknown>, seen);
}

function redactObject(value: Record<string, unknown>, seen: WeakSet<object>): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(value)) {
    if (isSensitiveKey(key) || isHealthValueKey(key)) {
      result[key] = REDACTED;
    } else {
      result[key] = redactValue(entry, seen);
    }
  }
  return result;
}

function canonicalKey(key: string): string {
  return key.replace(/[^A-Za-z0-9]/g, "").toLowerCase();
}

function isSensitiveKey(key: string): boolean {
  const canonical = canonicalKey(key);
  return canonical === "authorization"
    || canonical === "passwd"
    || canonical === "jwt"
    || canonical === "ticket"
    || canonical === "authticket"
    || canonical.includes("password")
    || canonical.includes("token")
    || canonical.includes("secret")
    || canonical.includes("apikey")
    || canonical.includes("accesskey")
    || canonical.includes("credential");
}

function isHealthValueKey(key: string): boolean {
  const canonical = canonicalKey(key);
  return canonical === "value"
    || canonical === "valueinmgperdl"
    || canonical === "measurementvalue"
    || canonical === "targetlow"
    || canonical === "targethigh"
    || canonical.startsWith("glucose");
}

function sanitizeString(value: string): string {
  return value
    .replace(/\bBearer\s+[A-Za-z0-9._~-]+/gi, `Bearer ${REDACTED}`)
    .replace(JWT, REDACTED)
    .replace(CANONICAL_DEVICE_TOKEN, REDACTED)
    .replace(SHA256_HEX, REDACTED)
    .replace(/\b(?:authorization|[A-Za-z0-9_.-]*(?:password|passwd|token|ticket|jwt|secret|credential|api[_-]?key|access[_-]?key))\s*[:=]\s*[^\s,}]+/gi, REDACTED)
    .replace(/\b(?:glucose[A-Za-z0-9_.-]*|valueinmgperdl|target[_-]?low|target[_-]?high)\s*[:=]\s*-?\d+(?:\.\d+)?/gi, REDACTED);
}
