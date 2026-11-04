import { describe, expect, it } from "vitest";
import { loadBridgeConfig } from "../bridge/src/config.js";

const hashA = "a".repeat(64);
const hashB = "b".repeat(64);
const baseEnv = {
  LIBRE_EMAIL: "follower@example.invalid",
  LIBRE_PASSWORD: "follower-password",
  LLU_APP_VERSION: "4.16.0",
  DEVICE_TOKEN_HASHES: `${hashA},${hashB}`,
};

describe("bridge environment configuration", () => {
  it("loads hardened defaults without exposing raw device tokens", () => {
    const config = loadBridgeConfig(baseEnv);
    expect(config).toMatchObject({
      host: "0.0.0.0",
      port: 8080,
      upstreamBaseUrl: "https://api.libreview.io",
      upstreamSource: "librelinkup",
      pollIntervalS: 60,
      pollJitterS: 10,
      maxBackoffS: 900,
      maxAgeS: 3_600,
      urgentLowMgDl: 70,
      activeTokenHashes: [hashA, hashB],
    });
    expect(JSON.stringify(config)).not.toContain("device-token");
  });

  it("accepts explicit unit and target-range fallbacks", () => {
    const config = loadBridgeConfig({
      ...baseEnv,
      DISPLAY_UNIT: "mmol/L",
      RANGE_LOW: "80",
      RANGE_HIGH: "160",
      RANGE_URGENT_LOW: "70",
      PORT: "9090",
    });
    expect(config).toMatchObject({
      port: 9090,
      unitOverride: "mmol/L",
      fallbackTargetLowMgDl: 80,
      fallbackTargetHighMgDl: 160,
      urgentLowMgDl: 70,
    });
  });

  it("rejects implausible urgent-low settings and fallback ordering", () => {
    expect(() => loadBridgeConfig({ ...baseEnv, RANGE_URGENT_LOW: "500" })).toThrow(/RANGE_URGENT_LOW/);
    expect(() => loadBridgeConfig({
      ...baseEnv,
      RANGE_URGENT_LOW: "90",
      RANGE_LOW: "80",
      RANGE_HIGH: "180",
    })).toThrow(/RANGE_URGENT_LOW.*RANGE_LOW/);
  });

  it("reports only variable names when required secret configuration is missing", () => {
    expect(() => loadBridgeConfig({ ...baseEnv, LIBRE_PASSWORD: undefined })).toThrow(/LIBRE_PASSWORD/);
    expect(() => loadBridgeConfig({ ...baseEnv, LIBRE_PASSWORD: undefined })).not.toThrow(/follower-password/);
  });

  it("rejects malformed token hashes and incomplete fallback ranges", () => {
    expect(() => loadBridgeConfig({ ...baseEnv, DEVICE_TOKEN_HASHES: "raw-device-token" })).toThrow(/DEVICE_TOKEN_HASHES/);
    expect(() => loadBridgeConfig({ ...baseEnv, RANGE_LOW: "80" })).toThrow(/RANGE_LOW.*RANGE_HIGH|RANGE_HIGH.*RANGE_LOW/);
  });
});
