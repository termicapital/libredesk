#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import re
import stat
import sys
from pathlib import Path
from typing import NoReturn

EXPECTED_VERSION = {"MAJOR": "9", "MINOR": "5", "PATCH": "0"}
EXPECTED_COMPONENT_HASH = "184e532558c1c45fefed631f3e235423d22582aafb4630f3e8885c35281a49ae"
EXPECTED_TREE_DIGEST = "79c1abe1603ce70c8169580528bcad3495d81eaf3d67e1f8a0ddc9488285dd7f"
VERSION_DEFINE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+LVGL_VERSION_(MAJOR|MINOR|PATCH)[ \t]+([0-9]+)[ \t]*$"
)
LOCK_HASH = re.compile(r"^ {4}component_hash: ([0-9a-f]{64})$")
LOCK_VERSION = re.compile(r"^ {4}version: ['\"]?([^'\" ]+)['\"]?$")


class PinError(ValueError):
    pass


def _digest_frame(digest: "hashlib._Hash", marker: bytes, relative_path: str) -> None:
    encoded_path = relative_path.encode("utf-8")
    digest.update(marker)
    digest.update(len(encoded_path).to_bytes(8, "big"))
    digest.update(encoded_path)


def canonical_tree_digest(source: Path) -> str:
    """Hash every directory, file byte, and symlink target in a stable order."""
    try:
        root = source.resolve(strict=True)
    except OSError as error:
        raise PinError(f"LVGL source directory is missing: {source}") from error
    if not root.is_dir():
        raise PinError(f"LVGL source is not a directory: {source}")

    digest = hashlib.sha256(b"libre-desk-lvgl-tree-v1\0")

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as error:
            raise PinError(f"cannot enumerate LVGL source directory: {directory}") from error
        for entry in entries:
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise PinError(f"cannot stat LVGL source entry: {relative}") from error
            if stat.S_ISLNK(metadata.st_mode):
                _digest_frame(digest, b"L", relative)
                try:
                    target = os.readlink(path).encode("utf-8")
                except OSError as error:
                    raise PinError(f"cannot read LVGL symlink: {relative}") from error
                digest.update(len(target).to_bytes(8, "big"))
                digest.update(target)
            elif stat.S_ISDIR(metadata.st_mode):
                _digest_frame(digest, b"D", relative)
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                _digest_frame(digest, b"F", relative)
                digest.update(metadata.st_size.to_bytes(8, "big"))
                try:
                    with path.open("rb") as source_file:
                        while chunk := source_file.read(1024 * 1024):
                            digest.update(chunk)
                except OSError as error:
                    raise PinError(f"cannot read LVGL source file: {relative}") from error
            else:
                raise PinError(f"unsupported LVGL source entry type: {relative}")

    visit(root)
    return digest.hexdigest()


def _require_file(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise PinError(f"required pin file is missing: {path}") from error
    if not resolved.is_file():
        raise PinError(f"required pin path is not a file: {path}")
    return resolved


def _parse_header(path: Path) -> None:
    definitions: dict[str, list[str]] = {name: [] for name in EXPECTED_VERSION}
    for line in _require_file(path).read_text(encoding="utf-8").splitlines():
        match = VERSION_DEFINE.fullmatch(line)
        if match:
            definitions[match.group(1)].append(match.group(2))
    for name, expected in EXPECTED_VERSION.items():
        values = definitions[name]
        if values != [expected]:
            raise PinError(
                f"LVGL_VERSION_{name} must be defined exactly once as {expected}; found {values}"
            )


def _parse_lock(path: Path) -> None:
    lines = _require_file(path).read_text(encoding="utf-8").splitlines()
    starts = [
        index
        for index, line in enumerate(lines)
        if line == "  lvgl/lvgl:"
    ]
    if len(starts) != 1:
        raise PinError(f"dependencies.lock must contain one lvgl/lvgl block; found {len(starts)}")
    block: list[str] = []
    for line in lines[starts[0] + 1 :]:
        if line and not line.startswith("    "):
            break
        block.append(line)
    hashes = [match.group(1) for line in block if (match := LOCK_HASH.fullmatch(line))]
    versions = [match.group(1) for line in block if (match := LOCK_VERSION.fullmatch(line))]
    if hashes != [EXPECTED_COMPONENT_HASH]:
        raise PinError(f"lvgl/lvgl lock hash mismatch: {hashes}")
    if versions != ["9.5.0"]:
        raise PinError(f"lvgl/lvgl lock version mismatch: {versions}")


def verify_lvgl_pin(
    source: Path,
    expected_source: Path,
    lock_path: Path,
    *,
    expected_tree_digest: str = EXPECTED_TREE_DIGEST,
) -> None:
    try:
        resolved_source = source.resolve(strict=True)
        resolved_expected = expected_source.resolve(strict=True)
    except OSError as error:
        raise PinError(f"LVGL source directory is missing: {error}") from error
    if not resolved_source.is_dir() or not resolved_expected.is_dir():
        raise PinError("LVGL source and expected source must be directories")
    if resolved_source != resolved_expected:
        raise PinError(
            f"LVGL_SOURCE_DIR override is forbidden: expected {resolved_expected}, got {resolved_source}"
        )
    _require_file(resolved_source / "CMakeLists.txt")
    _parse_header(resolved_source / "lv_version.h")
    component_hash = _require_file(resolved_source / ".component_hash").read_text(
        encoding="ascii"
    )
    if component_hash != EXPECTED_COMPONENT_HASH:
        raise PinError("managed LVGL .component_hash does not match dependencies.lock")
    _parse_lock(lock_path)
    if not re.fullmatch(r"[0-9a-f]{64}", expected_tree_digest):
        raise PinError("expected LVGL tree digest is invalid")
    actual_tree_digest = canonical_tree_digest(resolved_source)
    if actual_tree_digest != expected_tree_digest:
        raise PinError(
            "managed LVGL source-tree digest mismatch: "
            f"expected {expected_tree_digest}, got {actual_tree_digest}"
        )


def fail(message: str) -> NoReturn:
    print(f"LVGL pin verification failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the exact managed LVGL source pin")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--expected-source", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        verify_lvgl_pin(arguments.source, arguments.expected_source, arguments.lock)
    except (OSError, UnicodeError, PinError) as error:
        fail(str(error))
    print("LVGL pin verified: lvgl/lvgl 9.5.0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
