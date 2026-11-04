# glucose_core

`glucose_core` is the platform-independent C99 safety boundary between the normalized bridge contract and every firmware UI/network caller. It has no ESP-IDF, LVGL, socket, storage, or logging dependency.

## Enforced invariants

- Parse only frozen schema version `2`, including LibreLinkUp local measurement wall time, and reject unknown, duplicate, missing, malformed, oversized, non-UTF-8, future-dated, or time-inconsistent input.
- Accept at most 2048 bytes for `/api/v2/latest` and 512 bytes for error bodies.
- Convert wire values to canonical mg/dL internally while preserving the configured display unit.
- Never infer unknown trend arrows or missing target ranges.
- Increase reading age only from a monotonic clock, ignore clock rollback, and saturate instead of wrapping.
- Show a large number and range hue only in `FRESH` or `AGING`.
- Replace the value with `--` for stale, no-data, connectivity, authentication, and server-error states.
- Clear a prior reading transactionally after malformed or explicit error responses.
- Bound every display string and backoff result.

The official Libre app/reader remains authoritative. This component provides no alarms, treatment advice, interpolation, prediction, or dosing behavior.

## Native gate

From the repository root, with CMake 3.24 or newer, GCC/gcov, and Python 3 available:

```sh
scripts/test-glucose-core.sh
```

The script first self-tests the coverage parser, then performs three independent CMake/CTest builds:

1. strict C99 (`-Wall -Wextra -Werror -pedantic`);
2. AddressSanitizer plus UndefinedBehaviorSanitizer;
3. GCC coverage instrumentation.

The coverage check requires at least 90% line coverage, 100% branch-site execution, and 90% taken-branch coverage in the state facade. It also requires every branch direction in the freshness validator/classifier and number-visibility invariant to execute.

## ESP-IDF gate

`firmware/main` declares `glucose_core` as a private component dependency. The exact-board build therefore compiles the same sources with the pinned ESP-IDF v5.5.5 ESP32-C6 toolchain:

```sh
scripts/build-display-spike.sh
```

The production firmware consumes schema-2 live readings through this component, advances age from a monotonic clock, and publishes validated snapshots to the LVGL UI. Blocking HTTPS transport runs independently from the one-second runtime tick.
