import { describe, expect, it } from "vitest";
import { PusherConfigError, PusherError } from "../bridge/src/vercel/pusher.js";
import { runPusherCli } from "../bridge/src/vercel/pusher-cli.js";

const CONFIG = {
  localUrl: "http://127.0.0.1:18080/api/v2/latest",
  ingestUrl: "https://example.test/api/v1/ingest",
  deviceToken: Buffer.alloc(32, 1).toString("base64url"),
  ingestToken: Buffer.alloc(32, 2).toString("base64url"),
  timeoutMs: 5_000,
  maxBodyBytes: 4_096,
};

describe("Vercel pusher CLI", () => {
  it("is silent on success and honors the explicit secure-config path", async () => {
    let loadedPath = "";
    const errors: string[] = [];
    const exitCode = await runPusherCli({
      env: { LIBRE_DESK_VERCEL_PUSH_CONFIG: "/secure/pusher.json" },
      loadConfig: async (filePath) => {
        loadedPath = filePath;
        return CONFIG;
      },
      push: async () => ({ result: "stored" }),
      writeError: (message) => errors.push(message),
    });

    expect(exitCode).toBe(0);
    expect(loadedPath).toBe("/secure/pusher.json");
    expect(errors).toEqual([]);
  });

  it("maps all failures to generic secret-free stderr", async () => {
    for (const error of [
      new PusherConfigError("secret-config-detail"),
      new PusherError("secret-push-detail"),
      new Error("secret-internal-detail"),
    ]) {
      const errors: string[] = [];
      const exitCode = await runPusherCli({
        env: {},
        loadConfig: async () => {
          throw error;
        },
        push: async () => ({ result: "stored" }),
        writeError: (message) => errors.push(message),
      });

      expect(exitCode).toBe(1);
      expect(errors).toHaveLength(1);
      expect(errors[0]).toMatch(/^Libre Desk Vercel push failed: (configuration invalid|push unavailable|internal error)\n$/);
      expect(errors[0]).not.toContain("secret");
    }
  });
});
