import { describe, expect, it } from "vitest";
import { createRedactingLogger, redactForLog } from "../bridge/src/obs/logger.js";

describe("privacy-preserving bridge logger", () => {
  it("removes credentials, JWTs, and glucose values recursively", () => {
    const lines: string[] = [];
    const logger = createRedactingLogger((line) => lines.push(line));
    const jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJwYXRpZW50In0.signature";

    logger.info("upstream_result", {
      authorization: "Bearer device-token-value",
      password: "follower-password",
      token: "auth-ticket-value",
      jwt,
      glucose: 112,
      nested: {
        ValueInMgPerDl: 112,
        targetLow: 70,
        targetHigh: 180,
        region: "eu",
      },
    });

    const output = lines.join("\n");
    for (const forbidden of [
      "device-token-value",
      "follower-password",
      "auth-ticket-value",
      jwt,
      '"glucose":112',
      '"ValueInMgPerDl":112',
      '"targetLow":70',
      '"targetHigh":180',
    ]) {
      expect(output).not.toContain(forbidden);
    }
    expect(output).toContain("[REDACTED]");
    expect(output).toContain('"region":"eu"');
  });

  it("redacts camelCase, snake_case, unlabelled tokens, hashes, and health-key variants", () => {
    const lines: string[] = [];
    const logger = createRedactingLogger((line) => lines.push(line));
    const rawToken = Buffer.alloc(32, 9).toString("base64url");
    const tokenHash = "d".repeat(64);
    logger.warn("variant_probe", {
      deviceToken: rawToken,
      access_token: "access-secret-sentinel",
      librePassword: "password-secret-sentinel",
      glucoseMgDl: 123,
      target_low: 77,
      nested: { detail: `request failed for ${rawToken} hash ${tokenHash}` },
    });
    const output = lines.join("\n");
    for (const forbidden of [rawToken, tokenHash, "access-secret-sentinel", "password-secret-sentinel", "123", "77"]) {
      expect(output).not.toContain(forbidden);
    }
    expect(output).toContain("[REDACTED]");
  });

  it("sanitizes sensitive values embedded in messages", () => {
    const value = redactForLog("glucose=112 password=follower-password Authorization: Bearer abc.def.ghi");
    expect(value).not.toContain("112");
    expect(value).not.toContain("follower-password");
    expect(value).not.toContain("abc.def.ghi");
  });
});
