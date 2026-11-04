import type { LatestCache } from "./cache.js";
import { normalizeReading } from "./domain/normalize.js";
import type {
  LatestReading,
  NormalizationOptions,
  ValidationReason,
} from "./domain/types.js";
import { UpstreamError, UpstreamHttpError } from "./upstream/client.js";

export interface UpstreamSource {
  getConnections(): Promise<ReadonlyArray<Record<string, unknown>>>;
  getGraph(patientId: string): Promise<Record<string, unknown>>;
}

export interface PollerOptions extends NormalizationOptions {}

export type PollResult =
  | { status: "success"; reading: LatestReading }
  | { status: "rejected"; reason: ValidationReason; detail: string }
  | { status: "error"; code: string; detail: string };

export class Poller {
  private inFlight: Promise<PollResult> | undefined;

  constructor(
    private readonly source: UpstreamSource,
    private readonly cache: LatestCache,
    private readonly options: PollerOptions = {},
  ) {}

  pollOnce(now = new Date()): Promise<PollResult> {
    if (this.inFlight) return this.inFlight;
    const operation = this.performPoll(now);
    this.inFlight = operation;
    void operation.then(
      () => this.clearIfCurrent(operation),
      () => this.clearIfCurrent(operation),
    );
    return operation;
  }

  private async performPoll(now: Date): Promise<PollResult> {
    let connections: ReadonlyArray<Record<string, unknown>>;
    try {
      connections = await this.source.getConnections();
    } catch (error) {
      return upstreamFailure(error);
    }

    const candidates = connections.filter(
      (connection) => typeof connection.patientId === "string" && connection.patientId.trim().length > 0,
    );
    const patientIds = new Set(candidates.map((connection) => connection.patientId as string));
    if (patientIds.size === 0) {
      return { status: "rejected", reason: "missing_measurement", detail: "no patient connection available" };
    }
    if (patientIds.size !== 1) {
      return { status: "rejected", reason: "invalid_connection", detail: "multiple patient connections are ambiguous" };
    }
    const candidate = candidates[0];
    const patientId = [...patientIds][0];
    if (!candidate || !patientId) {
      return { status: "rejected", reason: "invalid_connection", detail: "patient connection selection failed" };
    }
    if (!isRecord(candidate.glucoseMeasurement)) {
      return { status: "rejected", reason: "missing_measurement", detail: "connection has no glucoseMeasurement" };
    }

    let graph: Record<string, unknown>;
    try {
      graph = await this.source.getGraph(patientId);
    } catch (error) {
      return upstreamFailure(error);
    }
    const graphData = isRecord(graph.data) ? graph.data : undefined;
    const connection = graphData && isRecord(graphData.connection) ? graphData.connection : undefined;
    if (!connection) {
      return { status: "error", code: "upstream_protocol_error", detail: "graph response omitted connection" };
    }
    if (connection.patientId !== patientId) {
      return { status: "error", code: "upstream_protocol_error", detail: "graph patient identity mismatch" };
    }

    const normalized = normalizeReading(connection, now, this.options);
    if (!normalized.ok) {
      return { status: "rejected", reason: normalized.reason, detail: normalized.detail };
    }
    if (!this.cache.set(normalized.reading)) {
      return { status: "rejected", reason: "non_monotonic_timestamp", detail: "reading timestamp regressed" };
    }
    return { status: "success", reading: normalized.reading };
  }

  private clearIfCurrent(operation: Promise<PollResult>): void {
    if (this.inFlight === operation) this.inFlight = undefined;
  }
}

function upstreamFailure(error: unknown): PollResult {
  if (error instanceof UpstreamHttpError && (error.statusCode === 401 || error.statusCode === 403)) {
    return { status: "error", code: "upstream_auth_failed", detail: "LibreLinkUp authentication failed" };
  }
  if (error instanceof UpstreamError) {
    return { status: "error", code: error.code, detail: error.message };
  }
  return { status: "error", code: "upstream_failure", detail: "upstream request failed" };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
