import { describe, expect, it } from "vitest";
import { BridgeMetrics } from "../bridge/src/obs/metrics.js";

describe("Prometheus bridge metrics", () => {
  it("renders the required redacted counters and gauges", () => {
    const metrics = new BridgeMetrics();
    metrics.recordLogin();
    metrics.recordPoll("success");
    metrics.recordPoll("error");
    metrics.recordRejection("future_timestamp");
    metrics.setReadingAge(47);
    metrics.recordDeviceResponse(200);
    metrics.recordDeviceResponse(503);
    metrics.setCacheHitRatio(0.5);

    const output = metrics.render();
    expect(output).toContain("upstream_login_total 1");
    expect(output).toContain('upstream_poll_total{result="success"} 1');
    expect(output).toContain('upstream_poll_total{result="error"} 1');
    expect(output).toContain('reading_rejected_total{reason="future_timestamp"} 1');
    expect(output).toContain("reading_age_seconds 47");
    expect(output).toContain('device_requests_total{status="200"} 1');
    expect(output).toContain('device_requests_total{status="503"} 1');
    expect(output).toContain("cache_hit_ratio 0.5");
    expect(output).not.toContain("glucose");
  });
});
