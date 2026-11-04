# LVGL host UI simulator

This tool compiles the shared firmware `ui_screens` component against the exact managed LVGL 9.5.0 source and captures a memory-only RGB565 display. It does not use SDL, a browser, screenshots of HTML, or a second visual implementation.

## Generate and verify

From the repository root:

```sh
make screens
```

Prerequisites:

- CMake 3.24 or newer;
- a C99 compiler;
- Python 3;
- Linux/POSIX support for Python `fcntl.flock`, `O_NOFOLLOW` secure file opening, and kernel `renameat2(RENAME_NOREPLACE)`;
- a little-endian host for the explicit RGB565 framebuffer capture (the simulator refuses other byte orders);
- `firmware/managed_components/lvgl__lvgl` at exact version 9.5.0. Running `scripts/build-display-spike.sh` obtains the locked managed component when it is absent.

The command:

1. verifies the managed LVGL version/component hash and a canonical SHA-256 over every source path, file byte, and symlink target;
2. compiles LVGL and the shared screen component and runs the native model tests;
3. renders 37 scenarios to temporary PPM buffers;
4. encodes PNGs with Python's standard library and requires two consecutive candidate trees to be byte-identical on the same pinned host;
5. creates 37 screen PNGs plus `docs/screens/contact-sheet.png`, `index.html`, and `manifest.json` (an exact 40-file set); the manifest records both LVGL identities;
6. verifies dimensions, file hashes, confined manifest paths, all 12 independent no-number decoded-RGB hashes, exact RGB565 palette values, exact arrow presence/orientation/bounds for every trend enum, byte-identical decoded-RGB number rasters and visible-ink centering for 2-digit, symmetric/asymmetric 3-digit, and widest decimal layouts with and without arrows, an independent geometry matrix over all 749 accepted mg/dL/mmol/L values plus 1,498 native LVGL renders exercising each value without and with an arrow, isolated widest-layout ±2 px clearance and exact jitter bounds, an exact centered 64×17 clock raster, an exact one-line SSID raster, aging/night dimming, and exact jitter translation;
7. locks the no-follow, owner/mode/inode-validated repository-parent namespace for the complete run, retains a separate repository identity descriptor, and builds only in a private per-run `/tmp` workspace; repository-path replacement therefore cannot create a second renderer lock domain;
8. copies only owned, single-link regular candidate files through no-follow directory descriptors, then publishes through a signal-masked, invocation-owned transaction. Backup, install, restore, and quarantine moves use atomic `renameat2(RENAME_NOREPLACE)` and retained-descriptor validation, so an injected destination is never overwritten. Rollback moves a pathname only while it still matches the held transaction descriptor; if a target is displaced, the unrelated replacement and recoverable backup are both preserved. After success, the complete previous generation remains under one ignored `.screens-quarantine.<random>` directory (or the quarantine is empty when no previous target existed); after rollback, the failed copied candidate remains quarantined intact. Publication performs no file or directory deletion in any path. Name-based child deletion or a final pathname-based `rmdir` would reopen races that could delete a swapped entry or lose the last recoverable generation.

All displayed numbers are synthetic visual-test fixtures. The device is a passive display, not an alarm: it makes no sound, does not flash, and must not be relied upon for low-glucose notification.
