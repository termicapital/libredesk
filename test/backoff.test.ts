import { describe, expect, it } from "vitest";
import { nextPollDelayS } from "../bridge/src/backoff.js";

describe("bridge poll backoff", () => {
  it("follows the specified 60 to 900 second capped sequence", () => {
    expect([0, 1, 2, 3, 4, 5, 6].map((failures) => nextPollDelayS(failures, {
      baseS: 60,
      maxS: 900,
      jitterS: 10,
      random: () => 0.5,
    }))).toEqual([60, 60, 120, 240, 480, 900, 900]);
  });

  it("applies bounded symmetric jitter without dropping below 20 seconds", () => {
    expect(nextPollDelayS(0, { baseS: 60, maxS: 900, jitterS: 10, random: () => 0 })).toBe(50);
    expect(nextPollDelayS(0, { baseS: 60, maxS: 900, jitterS: 10, random: () => 1 })).toBe(70);
    expect(nextPollDelayS(0, { baseS: 20, maxS: 900, jitterS: 10, random: () => 0 })).toBe(20);
  });
});
