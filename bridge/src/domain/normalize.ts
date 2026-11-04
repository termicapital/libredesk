import { classifyRange } from "./range.js";
import { mapTrendArrow } from "./trend.js";
import { validateMeasurement } from "./validate.js";
import type {
  DisplayUnit,
  LatestReading,
  NormalizationOptions,
  NormalizationResult,
} from "./types.js";

const DEFAULT_URGENT_LOW_MG_DL = 70;
const DEFAULT_POLL_AFTER_S = 60;

export function normalizeReading(
  input: unknown,
  now: Date,
  options: NormalizationOptions = {},
): NormalizationResult {
  const validation = validateMeasurement(input, now, options);
  if (!validation.ok) return validation;

  const measurement = validation.measurement;
  const outputUnit = options.unitOverride ?? measurement.unit;
  const urgentLowMgDl = options.urgentLowMgDl ?? DEFAULT_URGENT_LOW_MG_DL;
  const targetLowMgDl = measurement.targetLowMgDl ?? options.fallbackTargetLowMgDl;
  const targetHighMgDl = measurement.targetHighMgDl ?? options.fallbackTargetHighMgDl;
  const source = options.source ?? "librelinkup";
  const pollAfterS = clampInteger(options.pollAfterS ?? DEFAULT_POLL_AFTER_S, 20, 300);
  const range = classifyRange(measurement.valueMgDl, {
    targetLow: targetLowMgDl,
    targetHigh: targetHighMgDl,
    urgentLowMgDl,
  });

  const reading: LatestReading = {
    glucose: convertValue(measurement.valueMgDl, outputUnit),
    unit: outputUnit,
    trend: mapTrendArrow(measurement.trendArrow),
    range,
    target_low: convertOptional(targetLowMgDl, outputUnit),
    target_high: convertOptional(targetHighMgDl, outputUnit),
    timestamp: measurement.timestamp.toISOString(),
    libre_local_time: measurement.localTime,
    age_s: measurement.ageS,
    server_time: now.toISOString(),
    source,
    poll_after_s: pollAfterS,
    schema: 2,
  };

  return { ok: true, reading };
}

function convertOptional(value: number | undefined, unit: DisplayUnit): number | null {
  return value === undefined ? null : convertValue(value, unit);
}

function convertValue(valueMgDl: number, unit: DisplayUnit): number {
  if (unit === "mg/dL") return valueMgDl;
  return round(valueMgDl / 18, 1);
}

function round(value: number, decimalPlaces: number): number {
  const scale = 10 ** decimalPlaces;
  return Math.round(value * scale) / scale;
}

function clampInteger(value: number, minimum: number, maximum: number): number {
  if (!Number.isFinite(value)) return minimum;
  return Math.min(maximum, Math.max(minimum, Math.trunc(value)));
}
