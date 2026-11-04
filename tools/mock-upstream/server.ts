import {
  createServer,
  type IncomingHttpHeaders,
  type IncomingMessage,
  type Server,
  type ServerResponse,
} from "node:http";

export const MOCK_SCENARIOS = [
  "normal", "flat_line", "rising", "falling", "stale_5m", "stale_20m", "stale_2h",
  "no_readings", "future_timestamp", "value_out_of_range", "null_value", "malformed_json",
  "truncated_response", "http_500", "http_401_expired_ticket", "region_redirect",
  "terms_of_use_required", "slow_10s", "connection_reset", "mmol_units", "trend_notcomputable",
  "range_low", "range_urgent_low", "range_high", "no_target_range", "target_range_changed",
] as const;

export type MockScenario = (typeof MOCK_SCENARIOS)[number];

export interface MockUpstreamOptions {
  host?: string;
  port?: number;
  scenario?: MockScenario;
  now?: Date;
  slowDelayMs?: number;
}

export interface MockUpstream {
  readonly server: Server;
  readonly url: string;
  readonly ready: Promise<void>;
  requestCount(pathname: string): number;
  lastHeaders(pathname: string): Readonly<IncomingHttpHeaders> | undefined;
  setScenario(scenario: MockScenario): Promise<void>;
  close(): Promise<void>;
}

type Measurement = {
  ValueInMgPerDl: number | null;
  Value: number | null;
  GlucoseUnits: "mg/dL" | "mmol/L";
  FactoryTimestamp: string;
  Timestamp: string;
  TrendArrow: number;
};

type Connection = {
  patientId: string;
  targetLow?: number;
  targetHigh?: number;
  glucoseMeasurement?: Measurement;
};

const PATIENT_ID = "mock-patient-1";
const USER_ID = "mock-user-1";
const FIXED_NOW = new Date("2026-08-22T09:15:00.000Z");

export function createMockUpstream(options: MockUpstreamOptions = {}): MockUpstream {
  const host = options.host ?? "127.0.0.1";
  const requestedPort = options.port ?? 0;
  const clock = options.now ?? FIXED_NOW;
  const slowDelayMs = options.slowDelayMs ?? 10_000;
  let scenario: MockScenario = options.scenario ?? "normal";
  let regionRedirectIssued = false;
  const requestCounts = new Map<string, number>();
  const latestHeaders = new Map<string, IncomingHttpHeaders>();

  const server = createServer((request, response) => {
    const pathname = new URL(request.url ?? "/", "http://mock-upstream.invalid").pathname;
    requestCounts.set(pathname, (requestCounts.get(pathname) ?? 0) + 1);
    latestHeaders.set(pathname, { ...request.headers });
    void handleRequest(
      request,
      response,
      () => scenario,
      () => {
        if (regionRedirectIssued) return false;
        regionRedirectIssued = true;
        return true;
      },
      clock,
      slowDelayMs,
    );
  });
  const ready = new Promise<void>((resolve, reject) => {
    server.once("listening", () => resolve());
    server.once("error", reject);
  });
  server.listen(requestedPort, host);

  return {
    server,
    get url(): string {
      const address = server.address();
      if (!address || typeof address === "string") {
        throw new Error("mock upstream is not listening");
      }
      return `http://${host}:${address.port}`;
    },
    ready,
    requestCount(pathname) {
      return requestCounts.get(pathname) ?? 0;
    },
    lastHeaders(pathname) {
      return latestHeaders.get(pathname);
    },
    async setScenario(nextScenario) {
      if (!MOCK_SCENARIOS.includes(nextScenario)) {
        throw new Error(`unknown mock scenario: ${nextScenario}`);
      }
      scenario = nextScenario;
      regionRedirectIssued = false;
    },
    close() {
      return new Promise<void>((resolve, reject) => {
        if (!server.listening) {
          resolve();
          return;
        }
        server.close((error) => error ? reject(error) : resolve());
      });
    },
  };
}

async function handleRequest(
  request: IncomingMessage,
  response: ServerResponse,
  getScenario: () => MockScenario,
  consumeRegionRedirect: () => boolean,
  now: Date,
  slowDelayMs: number,
): Promise<void> {
  const url = new URL(request.url ?? "/", "http://mock-upstream.invalid");
  const activeScenario = getScenario();

  if (url.pathname === "/__scenario") {
    writeJson(response, 200, { scenario: activeScenario });
    return;
  }

  if (url.pathname === "/llu/auth/login" && request.method === "POST") {
    if (activeScenario === "terms_of_use_required") {
      writeJson(response, 403, { status: 403, message: "terms of use acceptance required" });
      return;
    }
    if (activeScenario === "region_redirect" && consumeRegionRedirect()) {
      writeJson(response, 200, { data: { redirect: true, region: "eu" } });
      return;
    }
    writeJson(response, 200, {
      data: {
        user: { id: USER_ID },
        authTicket: { token: "mock-auth-ticket", expires: 3600 },
      },
    });
    return;
  }

  if (url.pathname === "/llu/connections" && request.method === "GET") {
    if (activeScenario === "http_500") {
      writeJson(response, 500, { error: "mock upstream failure" });
      return;
    }
    if (activeScenario === "http_401_expired_ticket") {
      writeJson(response, 401, { error: "expired ticket" });
      return;
    }
    writeJson(response, 200, { data: [buildConnection(activeScenario, now)] });
    return;
  }

  if (url.pathname === `/llu/connections/${PATIENT_ID}/graph` && request.method === "GET") {
    await writeGraph(response, activeScenario, now, slowDelayMs);
    return;
  }

  writeJson(response, 404, { error: "not_found" });
}

async function writeGraph(
  response: ServerResponse,
  scenario: MockScenario,
  now: Date,
  slowDelayMs: number,
): Promise<void> {
  if (scenario === "slow_10s") {
    await delay(slowDelayMs);
  }
  if (scenario === "connection_reset") {
    response.destroy();
    return;
  }
  if (scenario === "http_500") {
    writeJson(response, 500, { error: "mock upstream failure" });
    return;
  }
  if (scenario === "http_401_expired_ticket") {
    writeJson(response, 401, { error: "expired ticket" });
    return;
  }
  if (scenario === "malformed_json") {
    response.writeHead(200, { "content-type": "application/json" });
    response.end("{not-json");
    return;
  }
  if (scenario === "truncated_response") {
    response.writeHead(200, { "content-type": "application/json", "content-length": "512" });
    response.write('{"data":{"connection":');
    response.destroy();
    return;
  }
  writeJson(response, 200, { data: { connection: buildConnection(scenario, now) } });
}

function buildConnection(scenario: MockScenario, now: Date): Connection {
  const targetRange = targetRangeFor(scenario);
  if (scenario === "no_readings") {
    return { patientId: PATIENT_ID, ...targetRange };
  }

  const ageSeconds = ageFor(scenario);
  const timestamp = new Date(now.getTime() - ageSeconds * 1000);
  if (scenario === "future_timestamp") {
    timestamp.setTime(now.getTime() + 300 * 1000);
  }

  const unit = scenario === "mmol_units" ? "mmol/L" : "mg/dL";
  const glucose = glucoseFor(scenario);
  const measurement: Measurement = {
    ValueInMgPerDl: scenario === "null_value" ? null : glucose,
    Value: scenario === "null_value" ? null : unit === "mmol/L" ? round(glucose / 18, 1) : glucose,
    GlucoseUnits: unit,
    FactoryTimestamp: timestamp.toISOString(),
    Timestamp: timestamp.toISOString().slice(0, 16).replace("T", " "),
    TrendArrow: trendArrowFor(scenario),
  };
  return { patientId: PATIENT_ID, ...targetRange, glucoseMeasurement: measurement };
}

function targetRangeFor(scenario: MockScenario): { targetLow?: number; targetHigh?: number } {
  if (scenario === "no_target_range") return {};
  if (scenario === "range_low") return { targetLow: 80, targetHigh: 180 };
  if (scenario === "target_range_changed") return { targetLow: 80, targetHigh: 160 };
  return { targetLow: 70, targetHigh: 180 };
}

function glucoseFor(scenario: MockScenario): number {
  switch (scenario) {
    case "rising": return 125;
    case "falling": return 100;
    case "range_low": return 75;
    case "range_urgent_low": return 50;
    case "range_high": return 220;
    case "value_out_of_range": return 501;
    default: return 112;
  }
}

function ageFor(scenario: MockScenario): number {
  switch (scenario) {
    case "stale_5m": return 300;
    case "stale_20m": return 1_200;
    case "stale_2h": return 7_200;
    default: return 0;
  }
}

function trendArrowFor(scenario: MockScenario): number {
  switch (scenario) {
    case "rising": return 5;
    case "falling": return 1;
    case "trend_notcomputable": return 6;
    default: return 3;
  }
}

function round(value: number, decimals: number): number {
  const scale = 10 ** decimals;
  return Math.round(value * scale) / scale;
}

function writeJson(response: ServerResponse, statusCode: number, value: unknown): void {
  const body = JSON.stringify(value);
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
    "cache-control": "no-store",
  });
  response.end(body);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
