import { randomUUID } from "node:crypto";
import { readFileSync } from "node:fs";
import { mkdir as mkdirAsync, readdir as readdirAsync, stat as statAsync, unlink as unlinkAsync, writeFile as writeFileAsync } from "node:fs/promises";
import path from "node:path";
import { nextPollDelayS } from "./backoff.js";
import { LatestCache } from "./cache.js";
import type { BridgeConfig } from "./config.js";
import {
  createBridgeServer,
  type BridgeServer,
  type DeviceCoredump,
  type OtaManifest,
  type UnavailableError,
} from "./http/server.js";
import { createRedactingLogger, type RedactingLogger } from "./obs/logger.js";
import { BridgeMetrics } from "./obs/metrics.js";
import { Poller, type PollResult } from "./poller.js";
import { LibreLinkUpClient } from "./upstream/client.js";

export interface BridgeRuntimeDependencies {
  clock?: () => Date;
  random?: () => number;
  logger?: RedactingLogger;
  coredumpDir?: string;
}

export interface BridgeRuntime {
  readonly server: BridgeServer;
  readonly metrics: BridgeMetrics;
  pollOnce(): Promise<PollResult>;
  start(): Promise<void>;
  stop(): Promise<void>;
}

export function createBridgeRuntime(
  config: BridgeConfig,
  dependencies: BridgeRuntimeDependencies = {},
): BridgeRuntime {
  const clock = dependencies.clock ?? (() => new Date());
  const random = dependencies.random ?? Math.random;
  const logger = dependencies.logger ?? createRedactingLogger();
  const coredumpDir = dependencies.coredumpDir ?? "/tmp/libre-desk-coredumps";
  const cache = new LatestCache();
  const metrics = new BridgeMetrics();
  let lastSuccessAt: Date | undefined;
  let upstreamOk = false;
  let unavailableError: UnavailableError = defaultUnavailableError();
  let consecutiveFailures = 0;
  let cacheLookups = 0;
  let cacheHits = 0;
  let running = false;
  let timer: NodeJS.Timeout | undefined;
  let runtimePoll: Promise<PollResult> | undefined;
  let coredumpWrite: Promise<void> = Promise.resolve();

  const client = new LibreLinkUpClient({
    baseUrl: config.upstreamBaseUrl,
    email: config.libreEmail,
    password: config.librePassword,
    product: config.lluProduct,
    version: config.lluAppVersion,
    timeoutMs: config.requestTimeoutMs,
    ...(config.upstreamSource === "mock" ? { regionBaseUrl: () => config.upstreamBaseUrl } : {}),
    onLoginAttempt: () => metrics.recordLogin(),
  });
  const pollerOptions = {
    maxAgeS: config.maxAgeS,
    urgentLowMgDl: config.urgentLowMgDl,
    fallbackTargetLowMgDl: config.fallbackTargetLowMgDl,
    fallbackTargetHighMgDl: config.fallbackTargetHighMgDl,
    pollAfterS: config.pollIntervalS,
    source: config.upstreamSource,
    ...(config.unitOverride ? { unitOverride: config.unitOverride } : {}),
  };
  const poller = new Poller(client, cache, pollerOptions);

  const server = createBridgeServer({
    cache,
    activeTokenHashes: config.activeTokenHashes,
    maxAgeS: config.maxAgeS,
    version: config.bridgeVersion,
    clock,
    health: () => lastSuccessAt ? { upstreamOk, lastSuccessAt } : { upstreamOk },
    unavailableError: () => unavailableError,
    metrics: () => {
      const currentAge = cache.ageSeconds(clock());
      if (currentAge !== undefined) metrics.setReadingAge(currentAge);
      return metrics.render();
    },
    otaManifest: () => readOtaManifest(config.firmwareDir),
    onTelemetry: (telemetry) => logger.info("device_telemetry", telemetry),
    onCoredump: (coredump) => {
      const operation = coredumpWrite.then(() => persistCoredump(coredumpDir, coredump, clock(), logger));
      coredumpWrite = operation.catch(() => undefined);
      return operation;
    },
    onDeviceResponse: (statusCode) => metrics.recordDeviceResponse(statusCode),
    onCacheLookup: (hit) => {
      cacheLookups += 1;
      if (hit) cacheHits += 1;
      metrics.setCacheHitRatio(cacheHits / cacheLookups);
    },
  });

  const performPoll = async (): Promise<PollResult> => {
    const polledAt = clock();
    const result = await poller.pollOnce(polledAt);
    metrics.recordPoll(result.status);
    if (result.status === "success") {
      lastSuccessAt = polledAt;
      upstreamOk = true;
      unavailableError = defaultUnavailableError();
      consecutiveFailures = 0;
      metrics.setReadingAge(result.reading.age_s);
      logger.info("upstream_poll_success", { source: result.reading.source });
    } else if (result.status === "rejected") {
      upstreamOk = false;
      unavailableError = rejectedUnavailableError(result.reason);
      consecutiveFailures += 1;
      metrics.recordRejection(result.reason);
      logger.warn("upstream_reading_rejected", { reason: result.reason });
    } else {
      upstreamOk = false;
      unavailableError = pollErrorUnavailableError(result.code);
      consecutiveFailures += 1;
      logger.error("upstream_poll_error", { code: result.code, detail: result.detail });
    }
    return result;
  };

  const pollOnce = (): Promise<PollResult> => {
    if (runtimePoll) return runtimePoll;
    const operation = performPoll();
    runtimePoll = operation;
    void operation.then(
      () => { if (runtimePoll === operation) runtimePoll = undefined; },
      () => { if (runtimePoll === operation) runtimePoll = undefined; },
    );
    return operation;
  };

  const schedule = (): void => {
    if (!running) return;
    const delayS = nextPollDelayS(consecutiveFailures, {
      baseS: config.pollIntervalS,
      maxS: config.maxBackoffS,
      jitterS: config.pollJitterS,
      random,
    });
    timer = setTimeout(() => {
      void pollOnce().finally(schedule);
    }, delayS * 1000);
    timer.unref();
  };

  return {
    server,
    metrics,
    pollOnce,
    async start() {
      if (running) return;
      await server.app.listen({ host: config.host, port: config.port });
      running = true;
      logger.info("bridge_listening", { host: config.host, port: config.port, version: config.bridgeVersion });
      await pollOnce();
      schedule();
    },
    async stop() {
      running = false;
      if (timer) clearTimeout(timer);
      timer = undefined;
      await server.app.close();
    },
  };
}

function defaultUnavailableError(): UnavailableError {
  return { code: "upstream_unavailable", detail: "no validated reading within max_age", retryAfterS: 30 };
}

function rejectedUnavailableError(reason: string): UnavailableError {
  if (reason === "missing_measurement" || reason === "too_old") return defaultUnavailableError();
  return { code: "malformed_upstream", detail: "upstream reading failed strict validation", retryAfterS: 60 };
}

function pollErrorUnavailableError(code: string): UnavailableError {
  if (code === "upstream_auth_failed") {
    return { code: "upstream_auth_failed", detail: "LibreLinkUp authentication failed", retryAfterS: 300 };
  }
  if (code === "terms_of_use_required") {
    return { code: "terms_of_use_required", detail: "accept updated terms in the LibreLinkUp app", retryAfterS: 300 };
  }
  if (code === "upstream_protocol_error") {
    return { code: "malformed_upstream", detail: "upstream response failed protocol validation", retryAfterS: 60 };
  }
  return defaultUnavailableError();
}

export async function persistCoredump(
  directory: string,
  coredump: DeviceCoredump,
  receivedAt: Date,
  logger: RedactingLogger,
): Promise<void> {
  await mkdirAsync(directory, { recursive: true, mode: 0o700 });
  const version = coredump.firmware_version.replace(/[^A-Za-z0-9_.-]/g, "_");
  const filename = `${receivedAt.getTime()}-${version}-${randomUUID()}.bin`;
  const destination = path.join(directory, filename);
  const payload = Buffer.from(coredump.data_base64, "base64");
  await writeFileAsync(destination, payload, { flag: "wx", mode: 0o600 });
  await pruneCoredumps(directory, 3, 2 * 1024 * 1024);
  logger.warn("device_coredump_received", {
    firmware_version: coredump.firmware_version,
    reset_reason: coredump.reset_reason,
    bytes: payload.length,
    path: destination,
  });
}

async function pruneCoredumps(directory: string, maxFiles: number, maxBytes: number): Promise<void> {
  const names = (await readdirAsync(directory)).filter(
    (name) => /^\d+-[A-Za-z0-9_.-]+-[0-9a-f-]+\.bin$/i.test(name),
  );
  const files = await Promise.all(names.map(async (name) => {
    const stats = await statAsync(path.join(directory, name));
    return { name, bytes: stats.size, modifiedMs: stats.mtimeMs };
  }));
  files.sort((left, right) => left.modifiedMs - right.modifiedMs || left.name.localeCompare(right.name));
  let totalBytes = files.reduce((sum, file) => sum + file.bytes, 0);
  while (files.length > maxFiles || totalBytes > maxBytes) {
    const oldest = files.shift();
    if (!oldest) break;
    await unlinkAsync(path.join(directory, oldest.name));
    totalBytes -= oldest.bytes;
  }
}

function isSafeOtaUrl(value: unknown): value is string {
  if (typeof value !== "string") return false;
  try {
    const parsed = new URL(value);
    return parsed.protocol === "https:"
      && parsed.hostname.length > 0
      && parsed.username.length === 0
      && parsed.password.length === 0;
  } catch {
    return false;
  }
}

export function readOtaManifest(firmwareDir: string): OtaManifest | undefined {
  try {
    const candidate = JSON.parse(readFileSync(path.join(firmwareDir, "manifest.json"), "utf8")) as Partial<OtaManifest>;
    if (
      typeof candidate.version !== "string"
      || !isSafeOtaUrl(candidate.url)
      || typeof candidate.sha256 !== "string"
      || !/^[a-f0-9]{64}$/.test(candidate.sha256)
      || typeof candidate.min_prev_version !== "string"
    ) return undefined;
    return {
      version: candidate.version,
      url: candidate.url,
      sha256: candidate.sha256,
      min_prev_version: candidate.min_prev_version,
    };
  } catch {
    return undefined;
  }
}
