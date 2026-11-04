# Libre Desk browser flasher

`index.html` uses vendored ESP Web Tools to flash the exact Waveshare ESP32-C6-LCD-1.3 firmware from desktop Chrome or Edge.

## Security boundary

The MVP application binary contains build-time Wi-Fi credentials, a revocable raw device token, and the HTTPS bridge origin. Generated binaries are ignored and must be treated as private per-device artifacts. Never commit them, attach them to a public GitHub release, or reuse one device's image on another board.

## Package a clean build

The canonical firmware script builds and packages automatically:

```sh
./scripts/build-display-spike.sh
```

To package an existing verified build explicitly:

```sh
python3 tools/flasher/package_firmware.py \
  --build-dir firmware/build \
  --output-dir tools/flasher \
  --version "$(cat firmware/version.txt)"
```

The packager reads ESP-IDF's generated `flasher_args.json`; offsets are never duplicated by hand. It verifies the project identity, version, ESP-IDF v5.5.5, ESP32-C6 target, DIO/80 MHz/4 MiB settings, exact required parts and offsets, bounded flash spans, non-overlap, regular-file identity, and source/destination hashes. Publication uses staging and restores the previous release if replacement fails.

Regression gates:

```sh
python3 tools/flasher/test_package_firmware.py -v
python3 tools/flasher/test_static_site.py -v
python3 tools/flasher/test_serve_private.py -v
```

Generated `.bin` files under `tools/flasher/firmware/` are excluded from Git. `manifest.json` and `release.json` contain only public package metadata and checksums; rebuild locally to recreate the ignored parts.

## Flash locally

Chromium treats localhost as a trustworthy WebSerial origin. Create a private device JSON outside Git with an `installer_path_token` containing 32 random bytes encoded as unpadded base64url, then run:

```sh
python3 tools/flasher/serve_private.py \
  --root tools/flasher \
  --device-config /absolute/private/path/device.json \
  --port 18081
```

Open:

```text
http://127.0.0.1:18081/install/<installer_path_token>/
```

The server loads an immutable release snapshot, serves only allowlisted artifacts under the capability path, emits `Cache-Control: no-store`, and suppresses request logs.

## Flash remotely

WebSerial is unavailable on ordinary insecure remote origins. If the build host and browser are different machines, expose the private server through certificate-verified HTTPS and retain the capability path. Remove the endpoint after flashing.

Do not reuse the device bearer token as the installer capability. Do not publish the installer URL in Git or an issue.

See `docs/SECOND_DEVICE.md` for end-to-end provisioning.

## Vendored dependency

Assets under `vendor/esp-web-tools-10.4.0/` come from `esp-web-tools@10.4.0`; the upstream license is included. The npm tarball was verified against its published integrity value:

```text
sha512-3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ==
```

No runtime JavaScript or font CDN is required.
