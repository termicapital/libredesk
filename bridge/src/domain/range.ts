import type { GlucoseRange } from "./types.js";

export interface TargetRange {
  targetLow?: number | undefined;
  targetHigh?: number | undefined;
  urgentLowMgDl: number;
}

export function classifyRange(glucoseMgDl: number, range: TargetRange): GlucoseRange {
  const { targetLow, targetHigh, urgentLowMgDl } = range;
  if (!isFiniteNumber(glucoseMgDl) || !isFiniteNumber(targetLow) || !isFiniteNumber(targetHigh)) {
    return "unknown";
  }
  if (
    !isFiniteNumber(urgentLowMgDl)
    || urgentLowMgDl < 20
    || urgentLowMgDl > 100
    || targetLow < 20
    || targetHigh > 500
    || urgentLowMgDl > targetLow
    || targetLow > targetHigh
  ) {
    return "unknown";
  }
  if (glucoseMgDl <= urgentLowMgDl) return "urgent_low";
  if (glucoseMgDl < targetLow) return "low";
  if (glucoseMgDl <= targetHigh) return "in_range";
  return "high";
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}
