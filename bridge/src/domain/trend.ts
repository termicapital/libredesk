import type { Trend } from "./types.js";

export function mapTrendArrow(value: number | undefined): Trend {
  switch (value) {
    case 5: return "rising";
    case 4: return "rising_slightly";
    case 3: return "stable";
    case 2: return "falling_slightly";
    case 1: return "falling";
    default: return "unknown";
  }
}
