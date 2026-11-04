import { readFile } from "node:fs/promises";
import path from "node:path";
import { describe, expect, it } from "vitest";

const root = path.resolve(import.meta.dirname, "..");

describe("Vercel deployment wiring", () => {
  it("exports the latest, ingestion, and health function entrypoints", async () => {
    const latest = await import("../api/latest.js");
    const latestV2 = await import("../api/latest-v2.js");
    const ingest = await import("../api/ingest.js");
    const health = await import("../api/health.js");

    expect(latest.default).toBeTypeOf("function");
    expect(latestV2.default).toBeTypeOf("function");
    expect(ingest.default).toBeTypeOf("function");
    expect(health.default).toBeTypeOf("function");
  });

  it("preserves the public bridge paths with bounded functions", async () => {
    const vercel = JSON.parse(await readFile(path.join(root, "vercel.json"), "utf8")) as {
      functions?: Record<string, { maxDuration?: number }>;
      outputDirectory?: string;
      regions?: string[];
      rewrites?: Array<{ source?: string; destination?: string }>;
    };

    expect(vercel.functions?.["api/*.ts"]?.maxDuration).toBe(30);
    expect(vercel.outputDirectory).toBe("public");
    expect(vercel.regions).toEqual(["fra1"]);
    expect(vercel.rewrites).toEqual(expect.arrayContaining([
      { source: "/api/v1/latest", destination: "/api/latest" },
      { source: "/api/v2/latest", destination: "/api/latest-v2" },
      { source: "/api/v1/ingest", destination: "/api/ingest" },
      { source: "/healthz", destination: "/api/health" },
    ]));
  });

  it("publishes only a minimal non-sensitive static root", async () => {
    const html = await readFile(path.join(root, "public/index.html"), "utf8");
    expect(html).toContain("Libre Desk bridge");
    expect(html).not.toMatch(/glucose|libre_email|password|device_token|authorization/i);
  });

  it("typechecks Vercel entrypoints as part of the normal build", async () => {
    const tsconfig = JSON.parse(await readFile(path.join(root, "tsconfig.json"), "utf8")) as {
      include?: string[];
    };

    expect(tsconfig.include).toContain("api/**/*.ts");
  });
});
