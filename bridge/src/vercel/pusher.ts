import { constants } from "node:fs";
import { open } from "node:fs/promises";
import { z } from "zod";

const LOCAL_LATEST_URL = "http://127.0.0.1:18080/api/v2/latest";
const CONFIG_MAX_BYTES = 4_096;
const DEFAULT_TIMEOUT_MS = 5_000;
const DEFAULT_MAX_BODY_BYTES = 4_096;
const TOKEN_PATTERN = /^[A-Za-z0-9_-]{43}$/;

const LocalReadingSchema = z.object({
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
  timestamp: z.string().min(1),
  libre_local_time: z.string().regex(/^(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d$/),
  age_s: z.number().int().nonnegative(),
  server_time: z.string().min(1),
  source: z.literal("librelinkup"),
  poll_after_s: z.number().int().min(20).max(300),
  schema: z.literal(2),
}).strict();

const AcknowledgementSchema = z.object({
  accepted: z.literal(true),
  result: z.enum(["stored", "duplicate"]),
}).strict();

export type FetchLike = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export interface PusherConfig {
  localUrl: string;
  ingestUrl: string;
  deviceToken: string;
  ingestToken: string;
  timeoutMs: number;
  maxBodyBytes: number;
}

export class PusherConfigError extends Error {
  constructor(message: string) {
    super(`invalid pusher configuration: ${message}`);
    this.name = "PusherConfigError";
  }
}

export class PusherError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PusherError";
  }
}

export function parsePusherConfig(raw: unknown): PusherConfig {
  if (!isRecord(raw)) throw new PusherConfigError("JSON object required");
  const expectedKeys = ["device_token", "ingest_token", "ingest_url"];
  const actualKeys = Object.keys(raw).sort();
  if (actualKeys.length !== expectedKeys.length || actualKeys.some((key, index) => key !== expectedKeys[index])) {
    throw new PusherConfigError("unexpected or missing fields");
  }
  const deviceToken = parseToken(raw.device_token, "device_token");
  const ingestToken = parseToken(raw.ingest_token, "ingest_token");
  if (deviceToken === ingestToken) {
    throw new PusherConfigError("device and ingestion tokens must be distinct");
  }
  const ingestUrl = parseIngestUrl(raw.ingest_url);
  return {
    localUrl: LOCAL_LATEST_URL,
    ingestUrl,
    deviceToken,
    ingestToken,
    timeoutMs: DEFAULT_TIMEOUT_MS,
    maxBodyBytes: DEFAULT_MAX_BODY_BYTES,
  };
}

export async function loadSecurePusherConfig(filePath: string): Promise<PusherConfig> {
  let handle;
  try {
    handle = await open(filePath, constants.O_RDONLY | constants.O_NOFOLLOW);
  } catch {
    throw new PusherConfigError("configuration file could not be opened securely");
  }
  try {
    const stat = await handle.stat();
    if (!stat.isFile()) throw new PusherConfigError("configuration must be a regular file");
    if ((stat.mode & 0o077) !== 0) throw new PusherConfigError("configuration permissions must be 0600");
    if (typeof process.getuid === "function" && stat.uid !== process.getuid()) {
      throw new PusherConfigError("configuration must be owned by the current user");
    }
    if (stat.size < 2 || stat.size > CONFIG_MAX_BYTES) {
      throw new PusherConfigError("configuration file size is invalid");
    }
    const buffer = Buffer.alloc(stat.size);
    const { bytesRead } = await handle.read(buffer, 0, stat.size, 0);
    if (bytesRead !== stat.size) throw new PusherConfigError("configuration read was incomplete");

    let parsed: unknown;
    try {
      parsed = JSON.parse(buffer.toString("utf8")) as unknown;
    } catch {
      throw new PusherConfigError("configuration is not valid JSON");
    }
    return parsePusherConfig(parsed);
  } finally {
    await handle.close();
  }
}

export async function pushLatestReading(
  config: PusherConfig,
  fetcher: FetchLike = globalThis.fetch.bind(globalThis),
): Promise<{ result: "stored" | "duplicate" }> {
  let localResponse: Response;
  try {
    localResponse = await fetcher(config.localUrl, {
      method: "GET",
      redirect: "error",
      headers: { authorization: `Bearer ${config.deviceToken}` },
      signal: AbortSignal.timeout(config.timeoutMs),
    });
  } catch {
    throw new PusherError("local bridge request failed");
  }
  if (localResponse.status !== 200) throw new PusherError("local bridge did not return a reading");
  const localPayload = await readBoundedJson(localResponse, config.maxBodyBytes, "local bridge");
  const parsedReading = LocalReadingSchema.safeParse(localPayload);
  if (!parsedReading.success) throw new PusherError("local bridge reading failed validation");

  let ingestResponse: Response;
  try {
    ingestResponse = await fetcher(config.ingestUrl, {
      method: "POST",
      redirect: "error",
      headers: {
        authorization: `Bearer ${config.ingestToken}`,
        "content-type": "application/json",
      },
      body: JSON.stringify(parsedReading.data),
      signal: AbortSignal.timeout(config.timeoutMs),
    });
  } catch {
    throw new PusherError("Vercel ingestion request failed");
  }
  if (ingestResponse.status !== 202) throw new PusherError("Vercel ingestion rejected the reading");
  const acknowledgement = await readBoundedJson(ingestResponse, config.maxBodyBytes, "Vercel ingestion");
  const parsedAcknowledgement = AcknowledgementSchema.safeParse(acknowledgement);
  if (!parsedAcknowledgement.success) {
    throw new PusherError("Vercel ingestion returned an invalid acknowledgement");
  }
  return { result: parsedAcknowledgement.data.result };
}

async function readBoundedJson(response: Response, maximumBytes: number, source: string): Promise<unknown> {
  const contentLength = response.headers.get("content-length");
  if (contentLength !== null && (!/^\d+$/.test(contentLength) || Number(contentLength) > maximumBytes)) {
    throw new PusherError(`${source} response exceeded size limit`);
  }
  if (response.body === null) throw new PusherError(`${source} returned an empty body`);

  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > maximumBytes) {
        void reader.cancel().catch(() => undefined);
        throw new PusherError(`${source} response exceeded size limit`);
      }
      chunks.push(value);
    }
  } catch (error) {
    if (error instanceof PusherError) throw error;
    throw new PusherError(`${source} response body could not be read`);
  }

  const bytes = Buffer.concat(chunks.map((chunk) => Buffer.from(chunk)), total);
  let text: string;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    throw new PusherError(`${source} returned invalid UTF-8`);
  }
  try {
    return JSON.parse(text) as unknown;
  } catch {
    throw new PusherError(`${source} returned malformed JSON`);
  }
}

function parseToken(raw: unknown, name: string): string {
  if (typeof raw !== "string" || !TOKEN_PATTERN.test(raw)) {
    throw new PusherConfigError(`${name} must be a 256-bit base64url token`);
  }
  let decoded: Buffer;
  try {
    decoded = Buffer.from(raw, "base64url");
  } catch {
    throw new PusherConfigError(`${name} must be a 256-bit base64url token`);
  }
  if (decoded.byteLength !== 32 || decoded.toString("base64url") !== raw) {
    throw new PusherConfigError(`${name} must be canonical base64url`);
  }
  return raw;
}

function parseIngestUrl(raw: unknown): string {
  if (typeof raw !== "string") throw new PusherConfigError("ingest_url must be a URL");
  let url: URL;
  try {
    url = new URL(raw);
  } catch {
    throw new PusherConfigError("ingest_url must be an absolute HTTPS URL");
  }
  if (
    url.protocol !== "https:"
    || url.username
    || url.password
    || url.search
    || url.hash
    || (url.port !== "" && url.port !== "443")
    || url.pathname !== "/api/v1/ingest"
  ) {
    throw new PusherConfigError("ingest_url must be an HTTPS /api/v1/ingest endpoint");
  }
  return url.toString();
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
