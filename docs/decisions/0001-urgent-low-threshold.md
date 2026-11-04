# ADR-0001: Urgent-low threshold

- Status: accepted
- Date: 2026-08-22
- Decision: classify `urgent_low` at 70 mg/dL (3.9 mmol/L), or the equivalent converted value when the display unit is mmol/L.
- Context: target-low and target-high come from the user's Libre app, but the private upstream connection object does not provide the separate urgent-low threshold. The plan requires this value to be explicit rather than guessed.
- Consequences: the bridge default and test fixtures must use 70 mg/dL; `RANGE_URGENT_LOW` remains an explicit environment override. This colour is not an alarm and never keeps a stale number visible.
