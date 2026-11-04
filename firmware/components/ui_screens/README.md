# ui_screens

`ui_screens` is the shared 240×240 LVGL rendering component used by firmware and `tools/uisim`. The host renderer captures this component directly; it does not recreate the UI in HTML, SVG, or Python.

## Safety invariants

- A large value is valid only for `UI_SCREEN_FRESH` and `UI_SCREEN_AGING`.
- Every stale, no-data, connectivity, authentication, terms, server, provisioning, boot, fetch, and update state rejects models that request a number or range hue.
- Value glyphs are bounded seven-segment LVGL geometry, so firmware and host output do not depend on an unpinned large font.
- Hue represents glucose range; dimming represents aging. No-number states use neutral grey.
- Trend glyphs encode direction only; slight, normal, and fast variants deliberately share the same rising or falling shape rather than inventing a visual rate-severity distinction.
- Jitter is bounded to ±2 px on each axis.
- SSIDs enter through a fixed 32-byte model field; non-printable/non-ASCII bytes are rendered as `?`, and long text uses one-line LVGL ellipsis behavior.
- Any detected child-object allocation failure discards the complete candidate screen so a partial seven-segment number cannot be returned; callers must blank or replace any older numeric screen when creation returns `NULL`.

## Visual proof

From the repository root:

```sh
make screens
```

This compiles against the exact managed LVGL 9.5.0 source, renders each scenario through a memory-only RGB565 display, and regenerates `docs/screens/`. See `tools/uisim/README.md` for prerequisites and artifact checks.
