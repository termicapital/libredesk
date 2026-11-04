import type { LatestReading } from "./types.js";

export interface LatestReadingV1 {
  glucose: number;
  unit: LatestReading["unit"];
  trend: LatestReading["trend"];
  range: LatestReading["range"];
  target_low: number | null;
  target_high: number | null;
  timestamp: string;
  age_s: number;
  server_time: string;
  source: LatestReading["source"];
  poll_after_s: number;
  schema: 1;
}

export type DisplayContractVersion = 1 | 2;

export function toLatestReadingV1(reading: LatestReading): LatestReadingV1 {
  return {
    glucose: reading.glucose,
    unit: reading.unit,
    trend: reading.trend,
    range: reading.range,
    target_low: reading.target_low,
    target_high: reading.target_high,
    timestamp: reading.timestamp,
    age_s: reading.age_s,
    server_time: reading.server_time,
    source: reading.source,
    poll_after_s: reading.poll_after_s,
    schema: 1,
  };
}

export function selectLatestReading(reading: LatestReading, version: DisplayContractVersion): LatestReading | LatestReadingV1 {
  return version === 1 ? toLatestReadingV1(reading) : reading;
}