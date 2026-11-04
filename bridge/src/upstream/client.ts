import { createHash } from "node:crypto";
import { request } from "undici";
import { z } from "zod";

const LoginResponse = z.object({
  data: z.object({
    redirect: z.boolean().optional(),
    region: z.string().optional(),
    user: z.object({ id: z.string().min(1) }).optional(),
    authTicket: z.object({ token: z.string().min(1), expires: z.number().positive().optional() }).optional(),
  }),
});
const ConnectionsResponse = z.object({ data: z.array(z.record(z.string(), z.unknown())) });
const GraphResponse = z.object({ data: z.object({ connection: z.record(z.string(), z.unknown()) }) });

export interface LibreLinkUpClientOptions {
  baseUrl: string;
  email: string;
  password: string;
  product: string;
  version: string;
  timeoutMs?: number;
  regionBaseUrl?: (region: string, currentBaseUrl: string) => string;
  onLoginAttempt?: () => void;
}

interface Session {
  token: string;
  expiresAtMs: number;
  refreshAtMs: number;
  accountId: string;
}

export class UpstreamError extends Error {
  constructor(public readonly code: string, message: string) {
    super(message);
    this.name = "UpstreamError";
  }
}

export class UpstreamHttpError extends UpstreamError {
  constructor(public readonly statusCode: number, message: string, public readonly termsRequired = false) {
    super("upstream_http_error", message);
    this.name = "UpstreamHttpError";
  }
}

export class UpstreamProtocolError extends UpstreamError {
  constructor(message: string) {
    super("upstream_protocol_error", message);
    this.name = "UpstreamProtocolError";
  }
}

export class UpstreamTransportError extends UpstreamError {
  constructor(message: string) {
    super("upstream_transport_error", message);
    this.name = "UpstreamTransportError";
  }
}

export class TermsOfUseRequiredError extends UpstreamError {
  constructor() {
    super("terms_of_use_required", "accept the new terms in the LibreLinkUp app, then restart the bridge");
    this.name = "TermsOfUseRequiredError";
  }
}

export class LibreLinkUpClient {
  private baseUrl: string;
  private session: Session | undefined;
  private readonly timeoutMs: number;
  private readonly regionBaseUrl: (region: string, currentBaseUrl: string) => string;

  constructor(private readonly options: LibreLinkUpClientOptions) {
    this.baseUrl = options.baseUrl.replace(/\/+$/, "");
    this.timeoutMs = options.timeoutMs ?? 15_000;
    this.regionBaseUrl = options.regionBaseUrl ?? ((region) => resolveRegionBaseUrl(region));
  }

  get accountId(): string | undefined {
    return this.session?.accountId;
  }

  setBaseUrl(baseUrl: string): void {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.clearSession();
  }

  clearSession(): void {
    this.session = undefined;
  }

  async login(): Promise<void> {
    await this.loginOnce(false);
  }

  async getConnections(): Promise<ReadonlyArray<Record<string, unknown>>> {
    const payload = await this.authenticatedGet("/llu/connections");
    const parsed = ConnectionsResponse.safeParse(payload);
    if (!parsed.success) throw new UpstreamProtocolError("connections response schema mismatch");
    return parsed.data.data;
  }

  async getGraph(patientId: string): Promise<Record<string, unknown>> {
    const payload = await this.authenticatedGet(`/llu/connections/${encodeURIComponent(patientId)}/graph`);
    const parsed = GraphResponse.safeParse(payload);
    if (!parsed.success) throw new UpstreamProtocolError("graph response schema mismatch");
    return parsed.data;
  }

  private async ensureSession(): Promise<void> {
    if (!this.session || Date.now() >= this.session.refreshAtMs) await this.login();
  }

  private async authenticatedGet(path: string): Promise<unknown> {
    await this.ensureSession();
    try {
      return await this.requestJson(path, "GET", undefined, true);
    } catch (error) {
      if (!(error instanceof UpstreamHttpError) || error.statusCode !== 401) throw error;
      this.clearSession();
      await this.login();
      return this.requestJson(path, "GET", undefined, true);
    }
  }

  private async loginOnce(alreadyRedirected: boolean): Promise<void> {
    this.options.onLoginAttempt?.();
    const payload = await this.requestJson("/llu/auth/login", "POST", {
      email: this.options.email,
      password: this.options.password,
    }, false);
    const parsed = LoginResponse.safeParse(payload);
    if (!parsed.success) throw new UpstreamProtocolError("login response schema mismatch");

    const data = parsed.data.data;
    if (data.redirect === true) {
      if (alreadyRedirected || !data.region) throw new UpstreamProtocolError("login returned an invalid region redirect");
      resolveRegionBaseUrl(data.region);
      this.baseUrl = this.regionBaseUrl(data.region, this.baseUrl).replace(/\/+$/, "");
      await this.loginOnce(true);
      return;
    }
    if (!data.user || !data.authTicket) throw new UpstreamProtocolError("login response omitted user or auth ticket");

    const issuedAtMs = Date.now();
    const expiresAtMs = resolveTicketExpiryMs(data.authTicket.expires, issuedAtMs);
    const ticketTtlMs = expiresAtMs - issuedAtMs;
    this.session = {
      token: data.authTicket.token,
      expiresAtMs,
      refreshAtMs: issuedAtMs + ticketTtlMs * 0.9,
      accountId: createHash("sha256").update(data.user.id, "utf8").digest("hex"),
    };
  }

  private async requestJson(
    path: string,
    method: "GET" | "POST",
    body: Record<string, unknown> | undefined,
    authenticated: boolean,
  ): Promise<unknown> {
    const headers: Record<string, string> = {
      accept: "application/json",
      "cache-control": "no-cache",
      "user-agent": "LibreDesk/0.1",
      "account-id": "",
      product: this.options.product,
      version: this.options.version,
    };
    if (body) {
      headers["content-type"] = "application/json";
    }
    if (authenticated && this.session) {
      headers.authorization = `Bearer ${this.session.token}`;
      headers["account-id"] = this.session.accountId;
    }

    try {
      const requestOptions = body
        ? { method, headers, body: JSON.stringify(body), signal: AbortSignal.timeout(this.timeoutMs) }
        : { method, headers, signal: AbortSignal.timeout(this.timeoutMs) };
      const response = await request(`${this.baseUrl}${path}`, requestOptions);
      const responseText = await response.body.text();
      if (response.statusCode < 200 || response.statusCode >= 300) {
        const termsRequired = /terms|privacy|policy/i.test(responseText);
        if (termsRequired) throw new TermsOfUseRequiredError();
        throw new UpstreamHttpError(response.statusCode, `LibreLinkUp returned HTTP ${response.statusCode}`);
      }
      if (responseText.length === 0) throw new UpstreamProtocolError("upstream returned an empty response");
      try {
        return JSON.parse(responseText) as unknown;
      } catch {
        throw new UpstreamProtocolError("upstream returned invalid JSON");
      }
    } catch (error) {
      if (error instanceof UpstreamError) throw error;
      throw new UpstreamTransportError("LibreLinkUp request failed");
    }
  }
}

export function resolveTicketExpiryMs(expires: number | undefined, issuedAtMs: number): number {
  const defaultTtlS = 6 * 30 * 24 * 60 * 60;
  const expiresAtMs = expires === undefined
    ? issuedAtMs + defaultTtlS * 1000
    : expires >= 1_000_000_000
      ? expires * 1000
      : issuedAtMs + expires * 1000;
  if (!Number.isFinite(expiresAtMs) || expiresAtMs <= issuedAtMs) {
    throw new UpstreamProtocolError("login returned an expired auth ticket");
  }
  return expiresAtMs;
}

export function resolveRegionBaseUrl(region: string): string {
  if (!/^[a-z0-9]{2,8}$/i.test(region)) {
    throw new UpstreamProtocolError("login returned an invalid region redirect");
  }
  return `https://api-${region.toLowerCase()}.libreview.io`;
}
