import { describe, expect, it } from "vitest";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { Ajv2020 } from "ajv/dist/2020.js";
import { parse as parseYaml } from "yaml";

const root = path.resolve(import.meta.dirname, "..");

async function readJson<T = unknown>(relativePath: string): Promise<T> {
  return JSON.parse(await readFile(path.join(root, relativePath), "utf8")) as T;
}

function asRecord(value: unknown): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error("expected object");
  }
  return value as Record<string, unknown>;
}

function successSchemaRef(pathDocument: unknown): unknown {
  const operation = asRecord(asRecord(pathDocument).get);
  const responses = asRecord(operation.responses);
  const success = asRecord(responses["200"]);
  const content = asRecord(success.content);
  return asRecord(asRecord(content["application/json"]).schema)["$ref"];
}

describe("Libre Desk device contract", () => {
  it("publishes the required OpenAPI paths and schemas", async () => {
    const document = parseYaml(await readFile(path.join(root, "contracts/openapi.yaml"), "utf8")) as {
      openapi?: string;
      paths?: Record<string, unknown>;
      components?: { schemas?: Record<string, unknown> };
    };

    expect(document.openapi).toBe("3.1.0");
    expect(document.paths).toEqual(expect.objectContaining({
      "/api/v1/latest": expect.any(Object),
      "/api/v2/latest": expect.any(Object),
      "/healthz": expect.any(Object),
      "/api/v1/telemetry": expect.any(Object),
      "/api/v1/coredump": expect.any(Object),
      "/api/v1/ota/manifest": expect.any(Object),
    }));
    expect(document.components?.schemas).toEqual(expect.objectContaining({
      LatestReadingV1: expect.any(Object),
      LatestReadingV2: expect.any(Object),
      ErrorResponse: expect.any(Object),
    }));
    expect(successSchemaRef(document.paths?.["/api/v1/latest"])).toBe(
      "#/components/schemas/LatestReadingV1",
    );
    expect(successSchemaRef(document.paths?.["/api/v2/latest"])).toBe(
      "#/components/schemas/LatestReadingV2",
    );
  });

  it("keeps legacy v1 and clock-aware v2 payloads mutually exclusive", async () => {
    const v1Schema = await readJson<Record<string, unknown>>("contracts/latest-v1.schema.json");
    const v2Schema = await readJson<Record<string, unknown>>("contracts/latest.schema.json");
    const validateV1 = new Ajv2020({ allErrors: true, strict: true }).compile(v1Schema);
    const validateV2 = new Ajv2020({ allErrors: true, strict: true }).compile(v2Schema);
    const fixtureNames = ["fresh", "aging", "stale"];

    for (const name of fixtureNames) {
      const fixture = asRecord(await readJson(`contracts/fixtures/${name}.json`));
      const { libre_local_time: _localTime, ...withoutLocalTime } = fixture;
      const legacy = { ...withoutLocalTime, schema: 1 };

      expect(validateV2(fixture), `${name} v2: ${JSON.stringify(validateV2.errors)}`).toBe(true);
      expect(validateV1(legacy), `${name} v1: ${JSON.stringify(validateV1.errors)}`).toBe(true);
      expect(validateV1(fixture), `${name} v1 accepted v2`).toBe(false);
      expect(validateV2(legacy), `${name} v2 accepted v1`).toBe(false);
    }
  });

  it("validates the standard error fixtures without accepting partial readings", async () => {
    const schema = await readJson<Record<string, unknown>>("contracts/error.schema.json");
    const validate = new Ajv2020({ allErrors: true, strict: true }).compile(schema);
    const fixtureNames = ["no-data", "auth-error", "server-error"];

    for (const name of fixtureNames) {
      const fixture = await readJson(`contracts/fixtures/${name}.json`);
      expect(validate(fixture), `${name}: ${JSON.stringify(validate.errors)}`).toBe(true);
      expect(fixture).not.toHaveProperty("glucose");
      expect(fixture).not.toHaveProperty("timestamp");
    }
  });
});
