import type {
  StoredRequest,
  StoredResponse,
} from "./stored.js";
import type { DisplayContractVersion } from "../domain/version.js";

export interface VercelRequestLike {
  method?: string;
  headers: Readonly<Record<string, string | string[] | undefined>>;
  body?: unknown;
}

export interface VercelResponseLike {
  status(code: number): this;
  setHeader(name: string, value: string): this;
  json(body: unknown): this;
  end(): this;
}

export interface VercelBridgeLike {
  latest(request: StoredRequest, version?: DisplayContractVersion): Promise<StoredResponse>;
  ingest(request: StoredRequest): Promise<StoredResponse>;
  health(): Promise<StoredResponse> | StoredResponse;
}

export type BridgeProvider = () => VercelBridgeLike;

export function createVercelLatestHandler(
  provider: BridgeProvider,
  version: DisplayContractVersion = 1,
) {
  return async (request: VercelRequestLike, response: VercelResponseLike): Promise<void> => {
    try {
      const result = await provider().latest({
        method: request.method,
        headers: {
          authorization: scalarHeader(request.headers, "authorization"),
          "if-none-match": scalarHeader(request.headers, "if-none-match"),
        },
      }, version);
      send(response, result);
    } catch {
      send(response, internalError());
    }
  };
}

export function createVercelIngestHandler(provider: BridgeProvider) {
  return async (request: VercelRequestLike, response: VercelResponseLike): Promise<void> => {
    try {
      const result = await provider().ingest({
        method: request.method,
        headers: {
          authorization: scalarHeader(request.headers, "authorization"),
        },
        body: request.body,
      });
      send(response, result);
    } catch {
      send(response, internalError());
    }
  };
}

export function createVercelHealthHandler(provider: BridgeProvider) {
  return async (_request: VercelRequestLike, response: VercelResponseLike): Promise<void> => {
    try {
      send(response, await provider().health());
    } catch {
      send(response, {
        statusCode: 503,
        headers: { "cache-control": "no-store" },
        body: {
          ok: false,
          configured: false,
          version: "0.3.0-vercel",
          mode: "stored_push",
        },
      });
    }
  };
}

function scalarHeader(
  headers: Readonly<Record<string, string | string[] | undefined>>,
  name: string,
): string | undefined {
  const matches = Object.entries(headers)
    .filter(([key]) => key.toLowerCase() === name);
  if (matches.length !== 1) return undefined;
  const value = matches[0]?.[1];
  return typeof value === "string" ? value : undefined;
}

function send(response: VercelResponseLike, result: StoredResponse): void {
  response.status(result.statusCode);
  for (const [name, value] of Object.entries(result.headers)) response.setHeader(name, value);
  if (result.body === undefined) {
    response.end();
    return;
  }
  response.json(result.body);
}

function internalError(): StoredResponse {
  return {
    statusCode: 500,
    headers: { "cache-control": "no-store" },
    body: { error: "internal", detail: "bridge internal error", retry_after_s: 30 },
  };
}
