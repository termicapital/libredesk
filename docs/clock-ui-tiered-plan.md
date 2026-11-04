# Clock UI tiered development checkpoint

## Frozen user-visible contract

- Fresh and aging screens display a centered 24-hour `HH:MM` clock.
- The clock is anchored directly to LibreLinkUp's local measurement wall time and advanced by reading age; firmware does not infer a timezone or display seconds.
- Stale, no-data, setup, Wi-Fi, authentication, terms, server-error, fetching, update, and boot screens do not display a clock.
- Fresh and aging screens display the glucose number without a visible unit label.
- The unit remains mandatory internally for conversion, validation, and range semantics.
- `/api/v1/latest` remains the frozen 12-field display payload with `schema: 1` and no `libre_local_time`, for already-flashed firmware.
- `/api/v2/latest` is the 13-field clock-aware display payload with `schema: 2` and required `libre_local_time`.
- The VPS-to-Vercel pusher reads local `/api/v2/latest`; the new clock firmware polls remote `/api/v2/latest`.

## Model lanes

| Lane | Provider/model | Effort | Ownership |
|---|---|---|---|
| REASONING | `openai-codex/gpt-5.6-sol` | Sol reasoning tier; no separate effort field is exposed by this session | Contract, parser/state edge cases, deployment compatibility, review, release verdict |
| BULK | `openai-codex/gpt-5.6-luna` | `medium` | Mechanical tests, bounded fixes from frozen counterexamples, builds, deterministic artifact generation |

## Routing matrix

| Work block | Default lane | Pre-code gate | Post-GREEN gate | Escalation trigger | Resume point |
|---|---|---|---|---|---|
| LLU local-time contract | REASONING | Freeze direct local wall-time semantics and avoid offset inference | Strict schema/parser review | Any ambiguity in Abbott timestamp formats or clock advancement | Focused bridge + parser tests |
| Runtime/UI plumbing | BULK | Required `HH:MM` and clock-free non-reading states frozen | REASONING state/ownership review | Unexplained native-test failure after two evidence-led attempts | `scripts/test-runtime-core.sh` and `scripts/test-ui-task-host.sh` |
| LVGL layout and raster proof | BULK | Clock at top; glucose geometry retained; unit label removed | REASONING visual and semantic review | Semantic raster failure, overlap, clipping, or unreadable visual | `scripts/render-ui-screens.sh <dir>` |
| Full integration/build | BULK | Contract and reviewed layout frozen | REASONING cross-component/security review | Contract drift, compiler/sanitizer failure, or firmware-size issue | `npm test`, native matrices, ESP-IDF build |
| Vercel deployment and firmware release | REASONING | Backward-incompatible field rollout order and rollback path approved | Live API, flasher, and physical-display acceptance | Any live schema gap, secret exposure, stale deployment, or hardware mismatch | Deploy bridge, push a fresh reading, publish firmware, flash hardware |

## Evidence-debt ledger

- Direct bridge local-time parser: executed and passing focused tests; schema-2 compatibility amendment pending RED/GREEN.
- Strict Vercel pusher/storage field: executed and passing focused tests and typecheck.
- ESP glucose parser: executed and passing strict native test.
- Runtime `HH:MM`, minute/midnight rollover, clock suppression: executed and passing strict native test.
- Authentic production `ui_task.c` copy path: executed and passing host test.
- LVGL model/raster proof: implemented; deterministic render and visual inspection pending.
- Complete repository matrix and ESP-IDF firmware build: pending.
- Independent Sol spec/quality review after all fixes: pending.
- v1/v2 zero-regression route split and rollout proof: pending.
- Vercel deployment, stable flasher publication, and physical display proof: pending.

## Gate rules

- Luna must not reinterpret the clock source, add timezone inference, expose seconds, restore the visible unit, deploy, commit, or weaken failing tests.
- A failed or timed-out worker is incomplete; the parent re-runs canonical commands and inspects the live tree.
- Any post-review fix requires a fresh Sol review before commit or release.
