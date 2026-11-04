import { describe, expect, it } from "vitest";
import { classifyRange } from "../bridge/src/domain/range.js";
import { normalizeReading } from "../bridge/src/domain/normalize.js";
import { mapTrendArrow } from "../bridge/src/domain/trend.js";
import { validateMeasurement } from "../bridge/src/domain/validate.js";
import { toLatestReadingV1 } from "../bridge/src/domain/version.js";

const now = new Date("2026-08-22T09:15:47.000Z");
const validConnection = {
  patientId: "mock-patient-1",
  targetLow: 70,
  targetHigh: 180,
  glucoseMeasurement: {
    ValueInMgPerDl: 112,
    Value: 112,
    GlucoseUnits: "mg/dL",
    FactoryTimestamp: "2026-08-22T09:15:00.000Z",
    Timestamp: "2026-08-22 11:15",
    TrendArrow: 3,
  },
};

describe("glucose_core-like bridge domain rules", () => {
  it("maps only the Libre 2 trend arrows used by the contract", () => {
    expect(mapTrendArrow(5)).toBe("rising");
    expect(mapTrendArrow(4)).toBe("rising_slightly");
    expect(mapTrendArrow(3)).toBe("stable");
    expect(mapTrendArrow(2)).toBe("falling_slightly");
    expect(mapTrendArrow(1)).toBe("falling");
    expect(mapTrendArrow(6)).toBe("unknown");
    expect(mapTrendArrow(7)).toBe("unknown");
    expect(mapTrendArrow(999)).toBe("unknown");
  });

  it("uses urgent-low before target-range classification", () => {
    expect(classifyRange(70, { targetLow: 80, targetHigh: 180, urgentLowMgDl: 70 })).toBe("urgent_low");
    expect(classifyRange(75, { targetLow: 80, targetHigh: 180, urgentLowMgDl: 70 })).toBe("low");
    expect(classifyRange(112, { targetLow: 80, targetHigh: 180, urgentLowMgDl: 70 })).toBe("in_range");
    expect(classifyRange(220, { targetLow: 80, targetHigh: 180, urgentLowMgDl: 70 })).toBe("high");
  });

  it("returns unknown rather than applying a clinical-looking hue to inconsistent thresholds", () => {
    expect(classifyRange(112, { urgentLowMgDl: 500, targetLow: 70, targetHigh: 180 })).toBe("unknown");
    expect(classifyRange(112, { urgentLowMgDl: 70, targetLow: 0, targetHigh: 10_000 })).toBe("unknown");
  });

  it("rejects implausible upstream target ranges", () => {
    const result = normalizeReading({
      ...validConnection,
      targetLow: 0,
      targetHigh: 10_000,
    }, now, { urgentLowMgDl: 70 });
    expect(result).toMatchObject({ ok: false, reason: "invalid_target_range" });
  });

  it("returns unknown range rather than guessing when thresholds are absent", () => {
    expect(classifyRange(112, { urgentLowMgDl: 70 })).toBe("unknown");
  });

  it("normalizes FactoryTimestamp and computes server age", () => {
    const result = normalizeReading(validConnection, now, { urgentLowMgDl: 70, source: "mock" });

    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.reading).toMatchObject({
      glucose: 112,
      unit: "mg/dL",
      trend: "stable",
      range: "in_range",
      target_low: 70,
      target_high: 180,
      timestamp: "2026-08-22T09:15:00.000Z",
      libre_local_time: "11:15:00",
      age_s: 47,
      server_time: "2026-08-22T09:15:47.000Z",
      source: "mock",
      poll_after_s: 60,
      schema: 2,
    });
  });

  it("projects schema 2 to the frozen schema 1 payload without leaking local time", () => {
    const result = normalizeReading(validConnection, now, { urgentLowMgDl: 70, source: "mock" });
    expect(result.ok).toBe(true);
    if (!result.ok) return;

    const legacy = toLatestReadingV1(result.reading);
    expect(legacy.schema).toBe(1);
    expect(legacy).not.toHaveProperty("libre_local_time");
    expect(Object.keys(legacy).sort()).toEqual([
      "age_s", "glucose", "poll_after_s", "range", "schema", "server_time",
      "source", "target_high", "target_low", "timestamp", "trend", "unit",
    ]);
  });

  it("parses Abbott's live month-first FactoryTimestamp as UTC", () => {
    const result = normalizeReading({
      ...validConnection,
      glucoseMeasurement: {
        ...validConnection.glucoseMeasurement,
        FactoryTimestamp: "8/24/2026 7:38:00 AM",
        Timestamp: "8/24/2026 9:38:00 AM",
      },
    }, new Date("2026-08-24T07:38:47.000Z"), { source: "librelinkup" });

    expect(result).toMatchObject({
      ok: true,
      reading: {
        timestamp: "2026-08-24T07:38:00.000Z",
        libre_local_time: "09:38:00",
        age_s: 47,
      },
    });
  });

  it.each([
    ["missing local timestamp", { Timestamp: undefined }],
    ["impossible local date", { Timestamp: "2026-02-30 11:15" }],
    ["invalid local hour", { Timestamp: "2026-08-22 24:00" }],
  ] as const)("rejects %s", (_label: string, changes: Readonly<Record<string, unknown>>) => {
    const result = validateMeasurement({
      ...validConnection,
      glucoseMeasurement: { ...validConnection.glucoseMeasurement, ...changes },
    }, now);
    expect(result).toMatchObject({ ok: false, reason: "invalid_timestamp" });
  });

  it("converts mmol/L output without changing internal mg/dL validation", () => {
    const mmolConnection = {
      ...validConnection,
      glucoseMeasurement: { ...validConnection.glucoseMeasurement, GlucoseUnits: "mmol/L" },
    };
    const result = normalizeReading(mmolConnection, now, { urgentLowMgDl: 70, source: "mock" });

    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.reading).toMatchObject({
      glucose: 6.2,
      unit: "mmol/L",
      target_low: 3.9,
      target_high: 10,
    });
  });

  it.each([
    [1, "mg/dL", 112],
    [0, "mmol/L", 6.2],
  ] as const)("interprets live numeric LibreLinkUp unit code %s", (code, unit, glucose) => {
    const result = normalizeReading({
      ...validConnection,
      glucoseMeasurement: { ...validConnection.glucoseMeasurement, GlucoseUnits: code },
    }, now, { urgentLowMgDl: 70, source: "librelinkup" });

    expect(result).toMatchObject({ ok: true, reading: { unit, glucose } });
  });

  it("uses configured target fallbacks only when the connection omits both thresholds", () => {
    const withoutTargets = { ...validConnection } as Record<string, unknown>;
    delete withoutTargets.targetLow;
    delete withoutTargets.targetHigh;

    const fallback = normalizeReading(withoutTargets, now, {
      urgentLowMgDl: 70,
      fallbackTargetLowMgDl: 80,
      fallbackTargetHighMgDl: 160,
    });
    const upstreamWins = normalizeReading(validConnection, now, {
      urgentLowMgDl: 70,
      fallbackTargetLowMgDl: 80,
      fallbackTargetHighMgDl: 160,
    });

    expect(fallback).toMatchObject({ ok: true, reading: { target_low: 80, target_high: 160 } });
    expect(upstreamWins).toMatchObject({ ok: true, reading: { target_low: 70, target_high: 180 } });
  });

  it.each([
    ["null value", { ValueInMgPerDl: null }, "missing_value"],
    ["string value", { ValueInMgPerDl: "112" }, "non_numeric_value"],
    ["too low", { ValueInMgPerDl: 19 }, "value_out_of_range"],
    ["too high", { ValueInMgPerDl: 501 }, "value_out_of_range"],
    ["bad timestamp", { FactoryTimestamp: "not-a-date" }, "invalid_timestamp"],
    ["impossible calendar timestamp", { FactoryTimestamp: "2026-02-30T09:15:00Z" }, "invalid_timestamp"],
    ["impossible Abbott calendar timestamp", { FactoryTimestamp: "2/30/2026 1:15:00 PM" }, "invalid_timestamp"],
  ])("rejects %s rather than repairing it", (_label, changes, reason) => {
    const measurement = { ...validConnection.glucoseMeasurement, ...changes };
    const result = validateMeasurement({ ...validConnection, glucoseMeasurement: measurement }, now);

    expect(result).toEqual(expect.objectContaining({ ok: false, reason }));
  });

  it.each([
    ["fractional mg/dL", { ValueInMgPerDl: 112.5 }, "non_integer_mg_dl"],
    ["unsupported units", { GlucoseUnits: "g/L" }, "invalid_units"],
    ["unsupported numeric unit code", { GlucoseUnits: 2 }, "invalid_units"],
    ["non-string timestamp", { FactoryTimestamp: 12345 }, "invalid_timestamp"],
  ])("rejects %s explicitly", (_label, changes, reason) => {
    const result = validateMeasurement({
      ...validConnection,
      glucoseMeasurement: { ...validConnection.glucoseMeasurement, ...changes },
    }, now);
    expect(result).toEqual(expect.objectContaining({ ok: false, reason }));
  });

  it("rejects an invalid server clock and invalid target ranges", () => {
    expect(validateMeasurement(validConnection, new Date(Number.NaN))).toEqual(
      expect.objectContaining({ ok: false, reason: "invalid_timestamp" }),
    );
    expect(validateMeasurement({ ...validConnection, targetLow: 190, targetHigh: 180 }, now)).toEqual(
      expect.objectContaining({ ok: false, reason: "invalid_target_range" }),
    );
  });

  it("rejects inconsistent range configuration and clamps server pacing", () => {
    expect(classifyRange(112, { targetLow: 190, targetHigh: 180, urgentLowMgDl: 70 })).toBe("unknown");
    expect(classifyRange(112, { targetLow: 70, targetHigh: 180, urgentLowMgDl: Number.NaN })).toBe("unknown");

    const tooFast = normalizeReading(validConnection, now, { pollAfterS: 1 });
    const tooSlow = normalizeReading(validConnection, now, { pollAfterS: 1_000 });
    const invalid = normalizeReading(validConnection, now, { pollAfterS: Number.NaN });
    expect(tooFast).toMatchObject({ ok: true, reading: { poll_after_s: 20 } });
    expect(tooSlow).toMatchObject({ ok: true, reading: { poll_after_s: 300 } });
    expect(invalid).toMatchObject({ ok: true, reading: { poll_after_s: 20 } });
  });

  it("rejects future timestamps beyond the allowed clock skew", () => {
    const result = validateMeasurement({
      ...validConnection,
      glucoseMeasurement: {
        ...validConnection.glucoseMeasurement,
        FactoryTimestamp: "2026-08-22T09:18:00.000Z",
      },
    }, now);

    expect(result).toEqual(expect.objectContaining({ ok: false, reason: "future_timestamp" }));
  });

  it("rejects readings older than the bridge maximum age", () => {
    const result = validateMeasurement({
      ...validConnection,
      glucoseMeasurement: {
        ...validConnection.glucoseMeasurement,
        FactoryTimestamp: "2026-08-22T08:00:00.000Z",
      },
    }, now, { maxAgeS: 3600 });

    expect(result).toEqual(expect.objectContaining({ ok: false, reason: "too_old" }));
  });
});
