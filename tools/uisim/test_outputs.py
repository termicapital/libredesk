#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path
from typing import NoReturn

from verify_lvgl_pin import EXPECTED_COMPONENT_HASH, EXPECTED_TREE_DIGEST

EXPECTED_SCREENS = [
    "boot",
    "provisioning",
    "wifi-connecting",
    "time-syncing",
    "fetching",
    "fresh-urgent-low",
    "fresh-low",
    "fresh-in-range",
    "fresh-high",
    "fresh-rising",
    "fresh-rising-slightly",
    "fresh-falling",
    "fresh-falling-slightly",
    "fresh-rising-fast",
    "fresh-falling-fast",
    "fresh-unknown-range",
    "fresh-mmol",
    "aging-low",
    "aging-in-range",
    "stale",
    "no-data",
    "auth-error",
    "terms-required",
    "server-error",
    "no-wifi",
    "ota-update",
    "fresh-night",
    "fresh-jitter",
    "layout-number-with-arrow",
    "layout-number-no-arrow",
    "layout-two-digit-with-arrow",
    "layout-two-digit-no-arrow",
    "layout-asymmetric-with-arrow",
    "layout-asymmetric-no-arrow",
    "layout-mmol-wide-with-arrow",
    "layout-mmol-wide-no-arrow",
    "layout-mmol-wide-jitter",
]
NO_NUMBER_SCREENS = {
    "boot",
    "provisioning",
    "wifi-connecting",
    "time-syncing",
    "fetching",
    "stale",
    "no-data",
    "auth-error",
    "terms-required",
    "server-error",
    "no-wifi",
    "ota-update",
}
NO_NUMBER_RGB_SHA256 = {
    "auth-error": (
        "af378f92637b33318cdd2ddb55f1c113"
        "b9b7ac338decd879decbcac98d24343d"
    ),
    "boot": "7d2ca77054961f104852f12337854ef700d62f0e516051f4a854e17367084fb9",
    "fetching": "b83fc59a65e5f3fcc8906aab6269eb69ee05b3265047d50219b1f75dad6bf3e7",
    "no-data": "ba0b11768c5c1e1d407aa3728ee0f0b6b4256d84df17650ebecffe459fd773a2",
    "no-wifi": "e4a152b42b0f3e8012108d443efa500d562ac9cb6303542158a79d83cce5ef76",
    "ota-update": "70d311a29def471dc08963c589feda5c7b586574d799b8ca0f58686d2eeb8b74",
    "provisioning": (
        "73b40e07ee4983251b6ef014ea7455e06"
        "ed513fe9a2307ad29f89c96a30666d4"
    ),
    "server-error": "3db2c7934c25b237856904c926d9cc1bf0ff86d570c135722f09b7d4af371220",
    "stale": "9c7e1aadeec4d0698c7f7b08542f32e3c24d2c1b8f0b33daae9c93f237b7051a",
    "terms-required": "8b5445c72da4b6b58607d6370eca0fcc27e44dc7c486ca7ccd765857a62384af",
    "time-syncing": "86eca16ab0b565ea290e8adb2654bd031efd84093532959c584a78873eaccb26",
    "wifi-connecting": "6eae4acdb571e58b9868d6c97a77b01f0cc881d06c5ab7fafe9489ce70723847",
}
TREND_SCREEN_DIRECTIONS = {
    "rising": "fresh-rising",
    "rising_slightly": "fresh-rising-slightly",
    "stable": "fresh-in-range",
    "falling_slightly": "fresh-falling-slightly",
    "falling": "fresh-falling",
    "rising_fast": "fresh-rising-fast",
    "falling_fast": "fresh-falling-fast",
    "unknown": "fresh-unknown-range",
}
TREND_ARROW_BOUNDS = (194, 63, 233, 99)
TREND_ARROW_EXPECTED_BOUNDS = {
    "rising": (194, 65, 211, 85),
    "rising_slightly": (194, 65, 211, 85),
    "stable": (194, 71, 212, 92),
    "falling_slightly": (194, 76, 211, 96),
    "falling": (194, 76, 211, 96),
    "rising_fast": (194, 65, 211, 85),
    "falling_fast": (194, 76, 211, 96),
    "unknown": None,
}
CLOCK_BOUNDS = (40, 2, 200, 36)
EXPECTED_CLOCK_PIXEL_BOUNDS = (88, 8, 151, 24)
VALUE_BOUNDS = (0, 35, 240, 132)
ISOLATED_VALUE_ARROW_BOUNDS = (0, 30, 240, 136)
LAYOUT_DIGIT_CASES = (
    (
        "three symmetric digits",
        "layout-number-with-arrow",
        "layout-number-no-arrow",
        (52, 35, 188, 132),
        (52, 187),
        (196, 71, 230, 92),
    ),
    (
        "two digits",
        "layout-two-digit-with-arrow",
        "layout-two-digit-no-arrow",
        (75, 35, 164, 132),
        (75, 163),
        (172, 71, 206, 92),
    ),
    (
        "three asymmetric digits",
        "layout-asymmetric-with-arrow",
        "layout-asymmetric-no-arrow",
        (34, 35, 170, 132),
        (69, 169),
        (180, 65, 211, 96),
    ),
    (
        "widest decimal",
        "layout-mmol-wide-with-arrow",
        "layout-mmol-wide-no-arrow",
        (44, 35, 195, 132),
        (44, 194),
        (203, 71, 237, 92),
    ),
)
REMOVED_UNIT_BOUNDS = (68, 143, 172, 167)
SCREEN_BACKGROUND = (0, 8, 8)
SSID_LABEL_BOUNDS = (28, 121, 212, 141)
SSID_LABEL_RGB_SHA256 = "9a37edfd419f95189de5562a5327445da3816c282730290154c524c70188cecc"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def fail(message: str) -> NoReturn:
    raise SystemExit(f"screen verification failed: {message}")


def decode_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        fail(f"invalid PNG signature: {path}")
    position = len(PNG_SIGNATURE)
    width = height = 0
    compressed = bytearray()
    saw_end = False
    while position < len(data):
        if position + 12 > len(data):
            fail(f"truncated PNG chunk: {path}")
        length = struct.unpack(">I", data[position : position + 4])[0]
        kind = data[position + 4 : position + 8]
        payload_start = position + 8
        payload_end = payload_start + length
        crc_end = payload_end + 4
        if crc_end > len(data):
            fail(f"truncated PNG payload: {path}")
        payload = data[payload_start:payload_end]
        expected_crc = struct.unpack(">I", data[payload_end:crc_end])[0]
        actual_crc = zlib.crc32(kind)
        actual_crc = zlib.crc32(payload, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            fail(f"PNG CRC mismatch: {path}")
        if kind == b"IHDR":
            if len(payload) != 13:
                fail(f"invalid IHDR: {path}")
            width, height, depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if (depth, color_type, compression, filtering, interlace) != (8, 2, 0, 0, 0):
                fail(f"unsupported PNG format: {path}")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            saw_end = True
            if crc_end != len(data):
                fail(f"trailing PNG data: {path}")
            break
        position = crc_end
    if not saw_end or width <= 0 or height <= 0:
        fail(f"incomplete PNG: {path}")
    raw = zlib.decompress(bytes(compressed))
    stride = width * 3
    if len(raw) != height * (stride + 1):
        fail(f"unexpected decoded PNG length: {path}")
    pixels = bytearray()
    for row in range(height):
        start = row * (stride + 1)
        if raw[start] != 0:
            fail(f"unexpected PNG row filter: {path}")
        pixels.extend(raw[start + 1 : start + 1 + stride])
    return width, height, bytes(pixels)


def pixel_triplets(pixels: bytes):
    return zip(pixels[0::3], pixels[1::3], pixels[2::3])


def saturated_pixel_count(pixels: bytes) -> int:
    return sum(
        1
        for red, green, blue in pixel_triplets(pixels)
        if max(red, green, blue) >= 100 and max(red, green, blue) - min(red, green, blue) >= 55
    )


def crop(pixels: bytes, width: int, x1: int, y1: int, x2: int, y2: int) -> bytes:
    result = bytearray()
    for y in range(y1, y2):
        start = (y * width + x1) * 3
        result.extend(pixels[start : start + (x2 - x1) * 3])
    return bytes(result)


def max_channel(pixels: bytes) -> int:
    return max(pixels) if pixels else 0


def dominant_visible_color(pixels: bytes) -> tuple[int, int, int]:
    colors = Counter(
        (red, green, blue)
        for red, green, blue in pixel_triplets(pixels)
        if max(red, green, blue) > 35
    )
    if not colors:
        fail("pixel region has no visible color")
    return colors.most_common(1)[0][0]


def has_long_vertical_run(
    pixels: bytes,
    width: int,
    x1: int,
    y1: int,
    x2: int,
    y2: int,
    minimum_run: int,
) -> bool:
    for x in range(x1, x2):
        run = 0
        for y in range(y1, y2):
            start = (y * width + x) * 3
            if max(pixels[start : start + 3]) > 45:
                run += 1
                if run >= minimum_run:
                    return True
            else:
                run = 0
    return False


def translated_rgb(
    pixels: bytes,
    width: int,
    height: int,
    dx: int,
    dy: int,
    background: tuple[int, int, int],
) -> bytes:
    result = bytearray()
    for y in range(height):
        for x in range(width):
            source_x = x - dx
            source_y = y - dy
            if 0 <= source_x < width and 0 <= source_y < height:
                start = (source_y * width + source_x) * 3
                result.extend(pixels[start : start + 3])
            else:
                result.extend(background)
    return bytes(result)


def expected_artifact_files() -> set[str]:
    return {f"{name}.png" for name in EXPECTED_SCREENS} | {
        "contact-sheet.png",
        "index.html",
        "manifest.json",
    }


def validate_artifact_set(directory: Path) -> None:
    entries = list(directory.iterdir())
    expected = expected_artifact_files()
    actual = {entry.name for entry in entries}
    if actual != expected:
        fail(f"unexpected artifact set: {sorted(actual ^ expected)}")
    non_regular = sorted(
        entry.name for entry in entries if entry.is_symlink() or not entry.is_file()
    )
    if non_regular:
        fail(f"artifacts must be regular files: {non_regular}")


def confined_manifest_path(
    directory: Path,
    file_value: object,
    expected_file: str,
    label: str,
) -> Path:
    if not isinstance(file_value, str):
        fail(f"manifest path for {label} must be a string")
    if (
        Path(file_value).is_absolute()
        or "/" in file_value
        or "\\" in file_value
        or file_value in {".", ".."}
    ):
        fail(f"unsafe manifest path for {label}: {file_value!r}")
    if file_value != expected_file:
        fail(f"manifest path for {label} must be exactly {expected_file!r}")

    root = directory.resolve()
    resolved = (root / file_value).resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        fail(f"manifest path for {label} escapes screen directory")
    if resolved.parent != root:
        fail(f"manifest path for {label} is not a direct child of screen directory")
    return resolved


def validate_manifest_and_paths(
    directory: Path, manifest: object
) -> tuple[list[dict[str, object]], dict[str, Path], dict[str, object], Path]:
    if not isinstance(manifest, dict):
        fail("manifest root must be an object")
    if (
        manifest.get("schema") != 1
        or manifest.get("screen_width") != 240
        or manifest.get("screen_height") != 240
    ):
        fail("invalid manifest identity or dimensions")
    if (
        manifest.get("lvgl_component_hash") != EXPECTED_COMPONENT_HASH
        or manifest.get("lvgl_tree_sha256") != EXPECTED_TREE_DIGEST
    ):
        fail("manifest LVGL source identity does not match the managed tree")

    records = manifest.get("screens")
    if not isinstance(records, list) or not all(isinstance(record, dict) for record in records):
        fail("manifest screens must be an array of objects")
    names = [record.get("name") for record in records]
    if names != EXPECTED_SCREENS:
        fail(f"unexpected screen order: {names}")

    screen_paths: dict[str, Path] = {}
    for record in records:
        name = record["name"]
        if not isinstance(name, str):
            fail("manifest screen name must be a string")
        screen_paths[name] = confined_manifest_path(
            directory, record.get("file"), f"{name}.png", f"screen {name}"
        )

    contact = manifest.get("contact_sheet")
    if not isinstance(contact, dict):
        fail("manifest contact_sheet must be an object")
    contact_path = confined_manifest_path(
        directory,
        contact.get("file"),
        "contact-sheet.png",
        "contact sheet",
    )
    return records, screen_paths, contact, contact_path


def foreground_mask(
    pixels: bytes,
    width: int,
    bounds: tuple[int, int, int, int],
    background: tuple[int, int, int],
) -> frozenset[tuple[int, int]]:
    x1, y1, x2, y2 = bounds
    return frozenset(
        (x, y)
        for y in range(y1, y2)
        for x in range(x1, x2)
        if tuple(pixels[(y * width + x) * 3 : (y * width + x + 1) * 3]) != background
    )


def mask_centroid_y(mask: frozenset[tuple[int, int]]) -> float:
    if not mask:
        fail("empty foreground mask has no centroid")
    return sum(y for _, y in mask) / len(mask)


def mask_bounds(
    mask: frozenset[tuple[int, int]], label: str
) -> tuple[int, int, int, int]:
    if not mask:
        fail(f"{label} foreground mask is empty")
    xs = [x for x, _ in mask]
    ys = [y for _, y in mask]
    return min(xs), min(ys), max(xs), max(ys)


def region_sha256(
    pixels: bytes, width: int, bounds: tuple[int, int, int, int]
) -> str:
    return hashlib.sha256(crop(pixels, width, *bounds)).hexdigest()


def verify_artifacts(directory: Path, manifest: object) -> int:
    records, screen_paths, contact, contact_path = validate_manifest_and_paths(
        directory, manifest
    )
    validate_artifact_set(directory)

    images: dict[str, bytes] = {}
    html_text = (directory / "index.html").read_text(encoding="utf-8")
    for record in records:
        name = record["name"]
        path = screen_paths[name]
        width, height, pixels = decode_png(path)
        if (width, height) != (240, 240):
            fail(f"wrong screen dimensions: {name}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
            fail(f"screen hash mismatch: {name}")
        if f'{name}.png' not in html_text:
            fail(f"screen is absent from index.html: {name}")
        if name in NO_NUMBER_SCREENS and record["shows_value"] is not False:
            fail(f"no-number screen claims to show a value: {name}")
        if name not in NO_NUMBER_SCREENS and record["shows_value"] is not True:
            fail(f"value screen claims to hide its value: {name}")
        if sum(1 for red, green, blue in pixel_triplets(pixels) if max(red, green, blue) > 35) < 100:
            fail(f"screen appears blank: {name}")
        images[name] = pixels

    for name, pixels in images.items():
        background = (pixels[0], pixels[1], pixels[2])
        clock_pixels = foreground_mask(pixels, 240, CLOCK_BOUNDS, background)
        unit_pixels = foreground_mask(pixels, 240, REMOVED_UNIT_BOUNDS, background)
        if name in NO_NUMBER_SCREENS:
            if clock_pixels:
                fail(f"clock leaked onto non-reading screen: {name}")
        elif not clock_pixels:
            fail(f"HH:MM clock is absent from reading screen: {name}")
        if name not in NO_NUMBER_SCREENS and unit_pixels:
            fail(f"visible glucose unit remains on reading screen: {name}")

    for (
        label,
        with_arrow_name,
        no_arrow_name,
        digit_bounds,
        expected_visible_x,
        expected_arrow_bounds,
    ) in LAYOUT_DIGIT_CASES:
        with_arrow_full = foreground_mask(
            images[with_arrow_name], 240, VALUE_BOUNDS, SCREEN_BACKGROUND
        )
        no_arrow_full = foreground_mask(
            images[no_arrow_name], 240, VALUE_BOUNDS, SCREEN_BACKGROUND
        )
        no_arrow_digits = foreground_mask(
            images[no_arrow_name], 240, digit_bounds, SCREEN_BACKGROUND
        )
        with_arrow_digits = foreground_mask(
            images[with_arrow_name], 240, digit_bounds, SCREEN_BACKGROUND
        )
        with_arrow_rgb = crop(images[with_arrow_name], 240, *digit_bounds)
        no_arrow_rgb = crop(images[no_arrow_name], 240, *digit_bounds)
        if with_arrow_rgb != no_arrow_rgb:
            fail(f"trend arrow changes the {label} decoded RGB raster")
        if no_arrow_full != no_arrow_digits:
            fail(f"{label} escape the centered digit rectangle")
        if with_arrow_digits != no_arrow_digits:
            fail(f"trend arrow changes the {label} position or raster")
        removed_number_pixels = no_arrow_full - with_arrow_full
        if removed_number_pixels:
            fail(f"trend arrow removes pixels from the {label} raster")
        arrow_pixels = frozenset(with_arrow_full - no_arrow_full)
        if not arrow_pixels:
            fail(f"trend arrow is absent from the {label} fixture")
        if any(x < digit_bounds[2] for x, _ in arrow_pixels):
            fail(f"trend arrow intrudes into the {label} rectangle")
        actual_arrow_bounds = mask_bounds(arrow_pixels, f"{label} arrow")
        if actual_arrow_bounds != expected_arrow_bounds:
            fail(
                f"{label} arrow bounds changed: "
                f"actual={actual_arrow_bounds}, expected={expected_arrow_bounds}"
            )
        number_x1, _, number_x2, _ = mask_bounds(no_arrow_digits, label)
        expected_x1, expected_x2 = expected_visible_x
        if number_x1 != expected_x1 or number_x2 != expected_x2:
            fail(
                f"{label} visible ink is not centered: "
                f"x={number_x1}..{number_x2}, expected={expected_x1}..{expected_x2}"
            )

    wide_geometry = foreground_mask(
        images["layout-mmol-wide-with-arrow"], 240,
        ISOLATED_VALUE_ARROW_BOUNDS, SCREEN_BACKGROUND
    )
    wide_bounds = mask_bounds(wide_geometry, "widest decimal layout")
    if wide_bounds != (44, 40, 237, 125):
        fail(f"widest decimal value/arrow bounds changed: {wide_bounds}")
    if wide_bounds[0] - 2 < 0 or wide_bounds[2] + 2 > 239:
        fail(f"widest decimal layout lacks +/-2 horizontal clearance: {wide_bounds}")
    expected_wide_jitter = translated_rgb(
        images["layout-mmol-wide-with-arrow"], 240, 240, 2, -2, SCREEN_BACKGROUND
    )
    if images["layout-mmol-wide-jitter"] != expected_wide_jitter:
        fail("widest decimal jitter is not an exact +2/-2 translation")
    wide_jitter_geometry = foreground_mask(
        images["layout-mmol-wide-jitter"], 240,
        ISOLATED_VALUE_ARROW_BOUNDS, SCREEN_BACKGROUND
    )
    wide_jitter_bounds = mask_bounds(wide_jitter_geometry, "jittered widest decimal layout")
    if wide_jitter_bounds != (46, 38, 239, 123):
        fail(f"jittered widest decimal value/arrow bounds changed: {wide_jitter_bounds}")

    clock_mask = foreground_mask(
        images["fresh-in-range"], 240, CLOCK_BOUNDS, SCREEN_BACKGROUND
    )
    clock_x1, clock_y1, clock_x2, clock_y2 = mask_bounds(clock_mask, "HH:MM clock")
    actual_clock_bounds = (clock_x1, clock_y1, clock_x2, clock_y2)
    if actual_clock_bounds != EXPECTED_CLOCK_PIXEL_BOUNDS:
        fail(
            f"HH:MM clock pixel bounds changed: actual={actual_clock_bounds}, "
            f"expected={EXPECTED_CLOCK_PIXEL_BOUNDS}"
        )

    for name in NO_NUMBER_SCREENS:
        if saturated_pixel_count(images[name]) != 0:
            fail(f"no-number state contains range-like hue: {name}")
        actual_rgb_hash = hashlib.sha256(images[name]).hexdigest()
        if actual_rgb_hash != NO_NUMBER_RGB_SHA256[name]:
            fail(
                f"no-number raster changed unexpectedly: {name} "
                f"actual={actual_rgb_hash} expected={NO_NUMBER_RGB_SHA256[name]}"
            )
    for name in {"fresh-urgent-low", "fresh-low", "fresh-high", "aging-low"}:
        if saturated_pixel_count(images[name]) < 100:
            fail(f"range screen lacks visible hue: {name}")

    expected_value_colors = {
        "fresh-urgent-low": (230, 53, 66),
        "fresh-low": (255, 97, 82),
        "fresh-in-range": (239, 247, 247),
        "fresh-high": (247, 166, 41),
        "fresh-unknown-range": (239, 247, 247),
        "aging-low": (140, 53, 41),
    }
    for name, expected_color in expected_value_colors.items():
        value_region = crop(images[name], 240, 0, 35, 240, 132)
        actual_color = dominant_visible_color(value_region)
        if actual_color != expected_color:
            fail(f"unexpected RGB565 value color for {name}: {actual_color}")

    fresh_value = crop(images["fresh-in-range"], 240, 0, 35, 240, 132)
    aging_value = crop(images["aging-in-range"], 240, 0, 35, 240, 132)
    if max_channel(aging_value) >= max_channel(fresh_value):
        fail("aging value is not dimmer than fresh value")
    if max_channel(images["fresh-night"]) >= max_channel(images["fresh-in-range"]):
        fail("night screen is not dimmer than day screen")
    expected_jitter = translated_rgb(
        images["fresh-in-range"], 240, 240, 2, -2, (0, 8, 8)
    )
    if images["fresh-jitter"] != expected_jitter:
        fail("jitter scenario is not an exact +2/-2 pixel translation")
    trend_masks = {
        direction: foreground_mask(
            images[screen_name], 240, TREND_ARROW_BOUNDS, SCREEN_BACKGROUND
        )
        for direction, screen_name in TREND_SCREEN_DIRECTIONS.items()
    }
    for direction in (
        "rising",
        "rising_slightly",
        "stable",
        "falling_slightly",
        "falling",
        "rising_fast",
        "falling_fast",
    ):
        if not trend_masks[direction]:
            fail(f"trend arrow is absent: {direction}")
    if trend_masks["unknown"]:
        fail("unknown trend must not render an arrow")
    for direction, expected_bounds in TREND_ARROW_EXPECTED_BOUNDS.items():
        if expected_bounds is None:
            continue
        actual_bounds = mask_bounds(trend_masks[direction], f"{direction} trend arrow")
        if actual_bounds != expected_bounds:
            fail(
                f"{direction} trend arrow bounds changed: "
                f"actual={actual_bounds}, expected={expected_bounds}"
            )
    if not (
        trend_masks["rising"]
        == trend_masks["rising_slightly"]
        == trend_masks["rising_fast"]
    ):
        fail("rising trend family does not share its deliberate arrow shape")
    if not (
        trend_masks["falling"]
        == trend_masks["falling_slightly"]
        == trend_masks["falling_fast"]
    ):
        fail("falling trend family does not share its deliberate arrow shape")
    if len({trend_masks["rising"], trend_masks["stable"], trend_masks["falling"]}) != 3:
        fail("rising, stable, and falling trend shapes are not distinct")
    rising_centroid = mask_centroid_y(trend_masks["rising"])
    stable_centroid = mask_centroid_y(trend_masks["stable"])
    falling_centroid = mask_centroid_y(trend_masks["falling"])
    if not rising_centroid < stable_centroid < falling_centroid:
        fail("trend arrow orientation is incorrect")

    wifi_long_ssid = images["wifi-connecting"]
    wrapped_region = crop(wifi_long_ssid, 240, 20, 141, 220, 165)
    if any(tuple(wrapped_region[index : index + 3]) != (0, 8, 8)
           for index in range(0, len(wrapped_region), 3)):
        fail("long SSID wrapped below its single-line label")
    if region_sha256(wifi_long_ssid, 240, SSID_LABEL_BOUNDS) != SSID_LABEL_RGB_SHA256:
        fail("long SSID one-line ellipsis raster changed unexpectedly")

    contact_width, contact_height, _ = decode_png(contact_path)
    if (contact_width, contact_height) != (contact.get("width"), contact.get("height")):
        fail("contact sheet dimensions do not match manifest")
    if hashlib.sha256(contact_path.read_bytes()).hexdigest() != contact.get("sha256"):
        fail("contact sheet hash mismatch")

    print(f"screen-output-ok screens={len(records)} no-number={len(NO_NUMBER_SCREENS)}")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: test_outputs.py SCREEN_DIRECTORY")
    directory = Path(sys.argv[1]).resolve()
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    return verify_artifacts(directory, manifest)


if __name__ == "__main__":
    raise SystemExit(main())
