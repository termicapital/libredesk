export type DisplayUnit = "mg/dL" | "mmol/L";
export type Trend =
  | "rising"
  | "rising_slightly"
  | "stable"
  | "falling_slightly"
  | "falling"
  | "rising_fast"
  | "falling_fast"
  | "unknown";
export type GlucoseRange = "urgent_low" | "low" | "in_range" | "high" | "unknown";
export type ReadingSource = "librelinkup" | "mock";

export interface UpstreamMeasurement {
  ValueInMgPerDl: unknown;
  Value?: unknown;
  GlucoseUnits?: unknown;
  FactoryTimestamp: unknown;
  Timestamp?: unknown;
  TrendArrow?: unknown;
}

export interface UpstreamConnection {
  patientId?: unknown;
  targetLow?: unknown;
  targetHigh?: unknown;
  glucoseMeasurement?: unknown;
}

export interface ValidatedMeasurement {
  valueMgDl: number;
  timestamp: Date;
  localTime: string;
  trendArrow: number | undefined;
  unit: DisplayUnit;
  targetLowMgDl: number | undefined;
  targetHighMgDl: number | undefined;
  ageS: number;
}

export interface LatestReading {
  glucose: number;
  unit: DisplayUnit;
  trend: Trend;
  range: GlucoseRange;
  target_low: number | null;
  target_high: number | null;
  timestamp: string;
  libre_local_time: string;
  age_s: number;
  server_time: string;
  source: ReadingSource;
  poll_after_s: number;
  schema: 2;
}

export type ValidationReason =
  | "invalid_connection"
  | "missing_measurement"
  | "missing_value"
  | "non_numeric_value"
  | "value_out_of_range"
  | "non_integer_mg_dl"
  | "invalid_units"
  | "invalid_timestamp"
  | "future_timestamp"
  | "too_old"
  | "invalid_target_range"
  | "non_monotonic_timestamp";

export interface ValidationFailure {
  ok: false;
  reason: ValidationReason;
  detail: string;
}

export interface ValidationSuccess {
  ok: true;
  measurement: ValidatedMeasurement;
}

export type ValidationResult = ValidationFailure | ValidationSuccess;

export interface NormalizationOptions {
  maxAgeS?: number;
  futureSkewS?: number;
  urgentLowMgDl?: number;
  fallbackTargetLowMgDl?: number | undefined;
  fallbackTargetHighMgDl?: number | undefined;
  unitOverride?: DisplayUnit;
  pollAfterS?: number;
  source?: ReadingSource;
}

export interface NormalizationFailure extends ValidationFailure {
  ok: false;
}

export interface NormalizationSuccess {
  ok: true;
  reading: LatestReading;
}

export type NormalizationResult = NormalizationFailure | NormalizationSuccess;
