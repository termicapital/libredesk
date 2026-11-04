export type PollMetricResult = "success" | "rejected" | "error";

export class BridgeMetrics {
  private loginTotal = 0;
  private readonly pollTotals = new Map<PollMetricResult, number>();
  private readonly rejectionTotals = new Map<string, number>();
  private readonly deviceResponseTotals = new Map<number, number>();
  private readingAgeSeconds = 0;
  private cacheHitRatio = 0;

  recordLogin(): void {
    this.loginTotal += 1;
  }

  recordPoll(result: PollMetricResult): void {
    this.pollTotals.set(result, (this.pollTotals.get(result) ?? 0) + 1);
  }

  recordRejection(reason: string): void {
    this.rejectionTotals.set(reason, (this.rejectionTotals.get(reason) ?? 0) + 1);
  }

  setReadingAge(ageSeconds: number): void {
    this.readingAgeSeconds = finiteNonnegative(ageSeconds);
  }

  recordDeviceResponse(statusCode: number): void {
    const status = Math.trunc(statusCode);
    this.deviceResponseTotals.set(status, (this.deviceResponseTotals.get(status) ?? 0) + 1);
  }

  setCacheHitRatio(ratio: number): void {
    this.cacheHitRatio = Math.min(1, finiteNonnegative(ratio));
  }

  render(): string {
    const lines = [
      "# TYPE upstream_login_total counter",
      `upstream_login_total ${this.loginTotal}`,
      "# TYPE upstream_poll_total counter",
    ];
    for (const [result, count] of sortedEntries(this.pollTotals)) {
      lines.push(`upstream_poll_total{result="${escapeLabel(result)}"} ${count}`);
    }
    lines.push("# TYPE reading_rejected_total counter");
    for (const [reason, count] of sortedEntries(this.rejectionTotals)) {
      lines.push(`reading_rejected_total{reason="${escapeLabel(reason)}"} ${count}`);
    }
    lines.push("# TYPE reading_age_seconds gauge", `reading_age_seconds ${this.readingAgeSeconds}`);
    lines.push("# TYPE device_requests_total counter");
    for (const [status, count] of sortedEntries(this.deviceResponseTotals)) {
      lines.push(`device_requests_total{status="${status}"} ${count}`);
    }
    lines.push("# TYPE cache_hit_ratio gauge", `cache_hit_ratio ${this.cacheHitRatio}`);
    return `${lines.join("\n")}\n`;
  }
}

function finiteNonnegative(value: number): number {
  return Number.isFinite(value) ? Math.max(0, value) : 0;
}

function sortedEntries<K extends string | number>(map: ReadonlyMap<K, number>): Array<[K, number]> {
  return [...map.entries()].sort(([left], [right]) => String(left).localeCompare(String(right)));
}

function escapeLabel(value: string): string {
  return value.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n");
}
