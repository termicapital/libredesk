#!/usr/bin/env python3
from __future__ import annotations

import csv
import hashlib
import html
import json
import re
import struct
import sys
import zlib
from pathlib import Path
from typing import NoReturn

from verify_lvgl_pin import EXPECTED_COMPONENT_HASH, EXPECTED_TREE_DIGEST

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SCREEN_WIDTH = 240
SCREEN_HEIGHT = 240
CONTACT_COLUMNS = 4
CONTACT_GAP = 12
CONTACT_BACKGROUND = (11, 16, 20)
NAME_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")


def fail(message: str) -> NoReturn:
    raise SystemExit(f"screen generation failed: {message}")


def parse_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        fail(f"invalid PPM header: {path}")
    try:
        width_text, height_text = parts[1].split(b" ", 1)
        width = int(width_text)
        height = int(height_text)
    except (ValueError, TypeError) as exc:
        fail(f"invalid PPM dimensions in {path}: {exc}")
    pixels = parts[3]
    if width != SCREEN_WIDTH or height != SCREEN_HEIGHT:
        fail(f"unexpected PPM dimensions in {path}: {width}x{height}")
    if len(pixels) != width * height * 3:
        fail(f"unexpected PPM payload length in {path}: {len(pixels)}")
    return width, height, pixels


def chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = zlib.crc32(kind)
    checksum = zlib.crc32(payload, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def encode_png(width: int, height: int, pixels: bytes) -> bytes:
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        fail("invalid RGB image passed to PNG encoder")
    stride = width * 3
    filtered = bytearray()
    for row in range(height):
        filtered.append(0)
        start = row * stride
        filtered.extend(pixels[start : start + stride])
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(filtered), level=9))
        + chunk(b"IEND", b"")
    )


def paste(canvas: bytearray, canvas_width: int, source: bytes,
          source_width: int, source_height: int, x: int, y: int) -> None:
    canvas_stride = canvas_width * 3
    source_stride = source_width * 3
    for row in range(source_height):
        destination = (y + row) * canvas_stride + x * 3
        source_start = row * source_stride
        canvas[destination : destination + source_stride] = source[
            source_start : source_start + source_stride
        ]


def parse_index(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    names: set[str] = set()
    with path.open("r", encoding="utf-8", newline="") as handle:
        for row_number, row in enumerate(csv.reader(handle, delimiter="\t"), start=1):
            if len(row) != 7:
                fail(f"manifest.tsv row {row_number} has {len(row)} columns")
            name, title, kind, shows_value, brightness, jitter_x, jitter_y = row
            if not NAME_PATTERN.fullmatch(name) or name in names:
                fail(f"invalid or duplicate scenario name: {name!r}")
            names.add(name)
            try:
                brightness_value = int(brightness)
                jitter_x_value = int(jitter_x)
                jitter_y_value = int(jitter_y)
            except ValueError as exc:
                fail(f"invalid numeric metadata for {name}: {exc}")
            if shows_value not in {"true", "false"}:
                fail(f"invalid shows_value metadata for {name}")
            if not 1 <= brightness_value <= 100:
                fail(f"invalid brightness metadata for {name}")
            if not -2 <= jitter_x_value <= 2 or not -2 <= jitter_y_value <= 2:
                fail(f"invalid jitter metadata for {name}")
            records.append(
                {
                    "name": name,
                    "title": title,
                    "kind": kind,
                    "shows_value": shows_value == "true",
                    "perceived_brightness_percent": brightness_value,
                    "jitter_x": jitter_x_value,
                    "jitter_y": jitter_y_value,
                }
            )
    if not records:
        fail("scenario index is empty")
    return records


def build_html(records: list[dict[str, object]]) -> str:
    cards = []
    for index, record in enumerate(records, start=1):
        name = html.escape(str(record["name"]))
        title = html.escape(str(record["title"]))
        kind = html.escape(str(record["kind"]))
        metadata = (
            f'{kind} · brightness {record["perceived_brightness_percent"]}% · '
            f'jitter {record["jitter_x"]:+d},{record["jitter_y"]:+d}'
        )
        cards.append(
            f'<article class="card" id="{name}">'
            f'<div class="ordinal">{index:02d}</div>'
            f'<img src="{name}.png" width="240" height="240" alt="{title}">'
            f'<h2>{title}</h2><p>{html.escape(metadata)}</p></article>'
        )
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; img-src 'self'; style-src 'unsafe-inline'">
<title>Libre Desk UI review</title>
<style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#070b0e;color:#ecf4f7}
*{box-sizing:border-box}body{margin:0;padding:28px;background:#070b0e}.wrap{max-width:1240px;margin:auto}
h1{font-size:clamp(28px,5vw,52px);margin:0 0 8px}.lede{max-width:760px;color:#93a1aa;line-height:1.55;margin:0 0 26px}
.notice{padding:14px 16px;border:1px solid #354049;border-radius:10px;color:#cbd5da;background:#10161a;margin-bottom:28px}
.contact{display:block;max-width:100%;height:auto;border:1px solid #263139;background:#0b1014;margin:0 0 34px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:20px}.card{position:relative;background:#10161a;border:1px solid #263139;border-radius:12px;padding:12px;min-width:0}
.card img{display:block;width:240px;max-width:100%;height:auto;margin:auto;background:#05080a;image-rendering:auto}.card h2{font-size:16px;margin:13px 0 5px}.card p{font:12px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace;color:#7d8991;margin:0;overflow-wrap:anywhere}
.ordinal{position:absolute;top:18px;left:18px;z-index:1;background:#05080ae6;border:1px solid #54616a;padding:3px 6px;border-radius:4px;font:11px ui-monospace,monospace;color:#ecf4f7}
@media(max-width:520px){body{padding:18px}.grid{grid-template-columns:1fr}.card{padding:10px}}
</style>
</head>
<body><main class="wrap">
<h1>Libre Desk UI states</h1>
<p class="lede">Exact 240×240 LVGL renders from the shared firmware component. Inspect hierarchy, clipping, range hue, aging opacity, no-number errors, night dimming, and position jitter.</p>
<div class="notice"><strong>Passive display:</strong> synthetic values only. This device makes no sound, does not flash, and must not be relied upon as a low-glucose alarm.</div>
<img class="contact" src="contact-sheet.png" alt="Contact sheet of all Libre Desk UI states">
<section class="grid">
""" + "\n".join(cards) + """
</section></main></body></html>
"""


def main() -> int:
    if len(sys.argv) != 3:
        fail("usage: generate_screens.py RAW_DIRECTORY OUTPUT_DIRECTORY")
    raw_directory = Path(sys.argv[1]).resolve()
    output_directory = Path(sys.argv[2]).resolve()
    if not raw_directory.is_dir() or not output_directory.is_dir():
        fail("raw and output directories must already exist")
    if any(output_directory.iterdir()):
        fail(f"output directory must be empty: {output_directory}")

    records = parse_index(raw_directory / "manifest.tsv")
    rendered: list[tuple[dict[str, object], bytes]] = []
    for record in records:
        name = str(record["name"])
        width, height, pixels = parse_ppm(raw_directory / f"{name}.ppm")
        png = encode_png(width, height, pixels)
        file_name = f"{name}.png"
        (output_directory / file_name).write_bytes(png)
        record["file"] = file_name
        record["sha256"] = hashlib.sha256(png).hexdigest()
        rendered.append((record, pixels))

    rows = (len(rendered) + CONTACT_COLUMNS - 1) // CONTACT_COLUMNS
    contact_width = CONTACT_GAP + CONTACT_COLUMNS * (SCREEN_WIDTH + CONTACT_GAP)
    contact_height = CONTACT_GAP + rows * (SCREEN_HEIGHT + CONTACT_GAP)
    background = bytes(CONTACT_BACKGROUND)
    contact_pixels = bytearray(background * (contact_width * contact_height))
    for index, (_, pixels) in enumerate(rendered):
        column = index % CONTACT_COLUMNS
        row = index // CONTACT_COLUMNS
        x = CONTACT_GAP + column * (SCREEN_WIDTH + CONTACT_GAP)
        y = CONTACT_GAP + row * (SCREEN_HEIGHT + CONTACT_GAP)
        paste(contact_pixels, contact_width, pixels, SCREEN_WIDTH, SCREEN_HEIGHT, x, y)
    contact_png = encode_png(contact_width, contact_height, bytes(contact_pixels))
    (output_directory / "contact-sheet.png").write_bytes(contact_png)

    manifest = {
        "schema": 1,
        "renderer": "LVGL 9.5.0 / shared ui_screens / RGB565",
        "screen_width": SCREEN_WIDTH,
        "screen_height": SCREEN_HEIGHT,
        "lvgl_component_hash": EXPECTED_COMPONENT_HASH,
        "lvgl_tree_sha256": EXPECTED_TREE_DIGEST,
        "contact_sheet": {
            "file": "contact-sheet.png",
            "width": contact_width,
            "height": contact_height,
            "columns": CONTACT_COLUMNS,
            "sha256": hashlib.sha256(contact_png).hexdigest(),
        },
        "screens": records,
    }
    (output_directory / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    (output_directory / "index.html").write_text(build_html(records), encoding="utf-8")
    print(f"packaged {len(records)} deterministic LVGL screen PNGs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
