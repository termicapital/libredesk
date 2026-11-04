import { describe, expect, it } from "vitest";
import { LatestCache } from "../bridge/src/cache.js";

const reading = {
  glucose: 112,
  unit: "mg/dL" as const,
  trend: "stable" as const,
  range: "in_range" as const,
  target_low: 70,
  target_high: 180,
  timestamp: "2026-08-22T09:15:00.000Z",
  libre_local_time: "11:15:00",
  age_s: 47,
  server_time: "2026-08-22T09:15:47.000Z",
  source: "mock" as const,
  poll_after_s: 60,
  schema: 2 as const,
};

describe("in-memory latest cache", () => {
  it("ages a cached reading from its original timestamp", () => {
    const cache = new LatestCache();
    cache.set(reading);

    const current = cache.get(new Date("2026-08-22T09:16:00.000Z"), 3_600);
    expect(current).toMatchObject({ age_s: 60, server_time: "2026-08-22T09:16:00.000Z" });
  });

  it("fails closed after max age while retaining no usable stale response", () => {
    const cache = new LatestCache();
    cache.set(reading);

    expect(cache.get(new Date("2026-08-22T10:16:00.000Z"), 3_600)).toBeUndefined();
    expect(cache.hasValue()).toBe(true);
  });

  it("never decreases age or server time when the wall clock moves backward", () => {
    const cache = new LatestCache();
    cache.set(reading);

    const first = cache.get(new Date("2026-08-22T09:16:00.000Z"), 3_600);
    const afterClockRollback = cache.get(new Date("2026-08-22T09:15:30.000Z"), 3_600);

    expect(first).toMatchObject({ age_s: 60, server_time: "2026-08-22T09:16:00.000Z" });
    expect(afterClockRollback).toMatchObject({ age_s: 60, server_time: "2026-08-22T09:16:00.000Z" });
  });

  it("does not rejuvenate a replayed measurement after the wall clock rolls back", () => {
    const cache = new LatestCache();
    cache.set(reading);
    expect(cache.get(new Date("2026-08-22T09:25:00.000Z"), 3_600)?.age_s).toBe(600);

    cache.set({
      ...reading,
      age_s: 60,
      server_time: "2026-08-22T09:16:00.000Z",
    });

    const replayed = cache.get(new Date("2026-08-22T09:16:00.000Z"), 3_600);
    expect(replayed?.age_s).toBe(600);
    expect(replayed?.server_time).toBe("2026-08-22T09:25:00.000Z");
  });

  it("rejects a measurement timestamp older than the cached measurement", () => {
    const cache = new LatestCache();
    expect(cache.set(reading)).toBe(true);
    expect(cache.set({ ...reading, timestamp: "2026-08-22T09:14:00.000Z" })).toBe(false);
    expect(cache.get(new Date("2026-08-22T09:16:00.000Z"), 3_600)?.timestamp).toBe(reading.timestamp);
  });

  it("starts empty after process construction", () => {
    expect(new LatestCache().hasValue()).toBe(false);
  });
});
