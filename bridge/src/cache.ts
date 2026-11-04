import type { LatestReading } from "./domain/types.js";

export class LatestCache {
  private latest: LatestReading | undefined;
  private lastAgeS = 0;
  private lastServerTimeMs = Number.NEGATIVE_INFINITY;

  set(reading: LatestReading): boolean {
    const timestampMs = Date.parse(reading.timestamp);
    const serverTimeMs = Date.parse(reading.server_time);
    if (!Number.isFinite(timestampMs) || !Number.isFinite(serverTimeMs)) return false;

    const previousTimestampMs = this.latest ? Date.parse(this.latest.timestamp) : Number.NEGATIVE_INFINITY;
    if (Number.isFinite(previousTimestampMs) && timestampMs < previousTimestampMs) return false;

    const effectiveServerTimeMs = Math.max(serverTimeMs, this.lastServerTimeMs);
    const ageAtEffectiveServer = Math.max(0, Math.floor((effectiveServerTimeMs - timestampMs) / 1000));
    const sameMeasurement = this.latest !== undefined && timestampMs === previousTimestampMs;
    this.lastAgeS = Math.max(
      0,
      reading.age_s,
      ageAtEffectiveServer,
      sameMeasurement ? this.lastAgeS : 0,
    );
    this.lastServerTimeMs = effectiveServerTimeMs;
    this.latest = { ...reading };
    return true;
  }

  hasValue(): boolean {
    return this.latest !== undefined;
  }

  ageSeconds(now: Date): number | undefined {
    if (!this.latest) return undefined;
    const timestampMs = Date.parse(this.latest.timestamp);
    const requestedNowMs = now.getTime();
    if (!Number.isFinite(timestampMs) || !Number.isFinite(requestedNowMs)) return undefined;
    const effectiveNowMs = Math.max(requestedNowMs, this.lastServerTimeMs);
    const ageS = Math.max(this.lastAgeS, Math.max(0, Math.floor((effectiveNowMs - timestampMs) / 1000)));
    this.lastAgeS = ageS;
    this.lastServerTimeMs = effectiveNowMs;
    return ageS;
  }

  get(now: Date, maxAgeS: number): LatestReading | undefined {
    const ageS = this.ageSeconds(now);
    if (!this.latest || ageS === undefined || ageS > maxAgeS) return undefined;
    return { ...this.latest, age_s: ageS, server_time: new Date(this.lastServerTimeMs).toISOString() };
  }
}
