import type {
  DisplayUnit,
  UpstreamConnection,
  UpstreamMeasurement,
  ValidationReason,
  ValidationResult,
} from "./types.js";

export const DEFAULT_MAX_AGE_S = 3_600;
export const DEFAULT_FUTURE_SKEW_S = 120;

export interface ValidationOptions {
  maxAgeS?: number;
  futureSkewS?: number;
}

export function validateMeasurement(
  input: unknown,
  now: Date,
  options: ValidationOptions = {},
): ValidationResult {
  const maxAgeS = options.maxAgeS ?? DEFAULT_MAX_AGE_S;
  const futureSkewS = options.futureSkewS ?? DEFAULT_FUTURE_SKEW_S;
  if (!isRecord(input)) return failure("invalid_connection", "connection payload is not an object");

  const connection = input as UpstreamConnection;
  if (!isRecord(connection.glucoseMeasurement)) {
    return failure("missing_measurement", "connection has no glucoseMeasurement");
  }
  const measurement = connection.glucoseMeasurement as unknown as UpstreamMeasurement;

  if (measurement.ValueInMgPerDl === null || measurement.ValueInMgPerDl === undefined) {
    return failure("missing_value", "ValueInMgPerDl is missing");
  }
  if (!isFiniteNumber(measurement.ValueInMgPerDl)) {
    return failure("non_numeric_value", "ValueInMgPerDl is not a finite number");
  }
  if (measurement.ValueInMgPerDl < 20 || measurement.ValueInMgPerDl > 500) {
    return failure("value_out_of_range", "ValueInMgPerDl is outside [20, 500]");
  }
  if (!Number.isInteger(measurement.ValueInMgPerDl)) {
    return failure("non_integer_mg_dl", "ValueInMgPerDl is not an integer");
  }

  const unit = displayUnit(measurement.GlucoseUnits);
  if (unit === undefined) {
    return failure("invalid_units", "GlucoseUnits is not a supported unit");
  }
  if (typeof measurement.FactoryTimestamp !== "string") {
    return failure("invalid_timestamp", "FactoryTimestamp is not a string");
  }
  const timestamp = parseUtcTimestamp(measurement.FactoryTimestamp);
  if (!timestamp) return failure("invalid_timestamp", "FactoryTimestamp is not RFC3339 UTC");
  if (typeof measurement.Timestamp !== "string") {
    return failure("invalid_timestamp", "Timestamp is not a local timestamp string");
  }
  const localTime = parseLocalTime(measurement.Timestamp);
  if (!localTime) {
    return failure("invalid_timestamp", "Timestamp is not a valid LibreLinkUp local timestamp");
  }
  if (!(now instanceof Date) || !Number.isFinite(now.getTime())) {
    return failure("invalid_timestamp", "server clock is invalid");
  }

  const ageS = Math.floor((now.getTime() - timestamp.getTime()) / 1000);
  if (ageS < -futureSkewS) {
    return failure("future_timestamp", "FactoryTimestamp is too far in the future");
  }
  if (ageS > maxAgeS) {
    return failure("too_old", "reading is older than max_age");
  }

  const targetResult = validateTargetRange(connection.targetLow, connection.targetHigh);
  if (!targetResult.ok) return targetResult;

  return {
    ok: true,
    measurement: {
      valueMgDl: measurement.ValueInMgPerDl,
      timestamp,
      localTime,
      trendArrow: isFiniteInteger(measurement.TrendArrow) ? measurement.TrendArrow : undefined,
      unit,
      targetLowMgDl: targetResult.targetLowMgDl,
      targetHighMgDl: targetResult.targetHighMgDl,
      ageS: Math.max(0, ageS),
    },
  };
}

type TargetValidation =
  | { ok: true; targetLowMgDl: number | undefined; targetHighMgDl: number | undefined }
  | { ok: false; reason: "invalid_target_range"; detail: string };

function validateTargetRange(low: unknown, high: unknown): TargetValidation {
  if (low === undefined && high === undefined) {
    return { ok: true, targetLowMgDl: undefined, targetHighMgDl: undefined };
  }
  if (!isFiniteNumber(low) || !isFiniteNumber(high) || low < 20 || high > 500 || low > high) {
    return { ok: false, reason: "invalid_target_range", detail: "targetLow/targetHigh are invalid" };
  }
  return { ok: true, targetLowMgDl: low, targetHighMgDl: high };
}

function parseUtcTimestamp(value: string): Date | undefined {
  const candidate = value.trim();
  const rfc3339 = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?Z$/.exec(candidate);
  if (rfc3339) {
    const timestamp = new Date(candidate);
    if (!Number.isFinite(timestamp.getTime())) return undefined;
    const [, year, month, day, hour, minute, second] = rfc3339;
    if (
      timestamp.getUTCFullYear() !== Number(year)
      || timestamp.getUTCMonth() + 1 !== Number(month)
      || timestamp.getUTCDate() !== Number(day)
      || timestamp.getUTCHours() !== Number(hour)
      || timestamp.getUTCMinutes() !== Number(minute)
      || timestamp.getUTCSeconds() !== Number(second)
    ) return undefined;
    return timestamp;
  }

  const abbott = /^(\d{1,2})\/(\d{1,2})\/(\d{4}) (\d{1,2}):(\d{2}):(\d{2}) (AM|PM)$/.exec(candidate);
  if (!abbott) return undefined;
  const [, monthText, dayText, yearText, hourText, minuteText, secondText, meridiem] = abbott;
  const month = Number(monthText);
  const day = Number(dayText);
  const year = Number(yearText);
  const hour12 = Number(hourText);
  const minute = Number(minuteText);
  const second = Number(secondText);
  if (hour12 < 1 || hour12 > 12) return undefined;
  const hour24 = meridiem === "AM" ? hour12 % 12 : (hour12 % 12) + 12;
  const timestamp = new Date(Date.UTC(year, month - 1, day, hour24, minute, second));
  if (
    timestamp.getUTCFullYear() !== year
    || timestamp.getUTCMonth() + 1 !== month
    || timestamp.getUTCDate() !== day
    || timestamp.getUTCHours() !== hour24
    || timestamp.getUTCMinutes() !== minute
    || timestamp.getUTCSeconds() !== second
  ) return undefined;
  return timestamp;
}

function parseLocalTime(value: string): string | undefined {
  const candidate = value.trim();
  const iso = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})(?::(\d{2}))?$/.exec(candidate);
  if (iso) {
    return canonicalLocalTime(
      Number(iso[1]), Number(iso[2]), Number(iso[3]), Number(iso[4]),
      Number(iso[5]), Number(iso[6] ?? 0),
    );
  }
  const abbott = /^(\d{1,2})\/(\d{1,2})\/(\d{4}) (\d{1,2}):(\d{2})(?::(\d{2}))? (AM|PM)$/.exec(candidate);
  if (!abbott) return undefined;
  const hour12 = Number(abbott[4]);
  if (hour12 < 1 || hour12 > 12) return undefined;
  const hour24 = abbott[7] === "AM" ? hour12 % 12 : (hour12 % 12) + 12;
  return canonicalLocalTime(
    Number(abbott[3]), Number(abbott[1]), Number(abbott[2]), hour24,
    Number(abbott[5]), Number(abbott[6] ?? 0),
  );
}

function canonicalLocalTime(
  year: number,
  month: number,
  day: number,
  hour: number,
  minute: number,
  second: number,
): string | undefined {
  const timestamp = new Date(Date.UTC(year, month - 1, day, hour, minute, second));
  if (
    timestamp.getUTCFullYear() !== year
    || timestamp.getUTCMonth() + 1 !== month
    || timestamp.getUTCDate() !== day
    || timestamp.getUTCHours() !== hour
    || timestamp.getUTCMinutes() !== minute
    || timestamp.getUTCSeconds() !== second
  ) return undefined;
  return `${String(hour).padStart(2, "0")}:${String(minute).padStart(2, "0")}:${String(second).padStart(2, "0")}`;
}

function failure(reason: ValidationReason, detail: string): ValidationResult {
  return { ok: false, reason, detail };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function isFiniteInteger(value: unknown): value is number {
  return isFiniteNumber(value) && Number.isInteger(value);
}

function displayUnit(value: unknown): DisplayUnit | undefined {
  if (value === "mg/dL" || value === 1) return "mg/dL";
  if (value === "mmol/L" || value === 0) return "mmol/L";
  return undefined;
}
