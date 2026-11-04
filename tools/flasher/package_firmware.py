#!/usr/bin/env python3
"""Package an ESP-IDF build as a fail-safe ESP Web Tools release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

FLASH_SIZE_BYTES = 4 * 1024 * 1024
EXPECTED_IDF_VERSION = "v5.5.5"
DEFAULT_PROJECT_NAME = "libre_desk_display_spike"
DEFAULT_RELEASE_NAME = "Libre Desk display spike"
EXPECTED_TARGET = "esp32c6"
EXPECTED_FLASH_OFFSETS = {0x000000, 0x008000, 0x00F000, 0x020000}
VERSION_PATTERN = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,63}\Z")
PROJECT_NAME_PATTERN = re.compile(r"[0-9A-Za-z][0-9A-Za-z_.+-]{0,63}\Z")
RELEASE_NAME_PATTERN = re.compile(r"[ -~]{1,80}\Z")
PUBLICATION_NAMES = ("firmware", "manifest.json", "release.json")


@dataclass(frozen=True)
class Artifact:
    offset: int
    source: Path
    destination_name: str
    size: int


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validated_artifacts(build_dir: Path, flash_files: object) -> list[Artifact]:
    if not isinstance(flash_files, dict) or not flash_files:
        raise SystemExit("flasher_args.json has no flash_files map")

    artifacts: list[Artifact] = []
    for offset_text, source_text in flash_files.items():
        if not isinstance(offset_text, str) or not isinstance(source_text, str):
            raise SystemExit("flasher_args.json contains a malformed flash_files entry")
        try:
            offset = int(offset_text, 0)
        except ValueError as error:
            raise SystemExit(f"invalid flash offset: {offset_text}") from error
        if offset < 0 or offset >= FLASH_SIZE_BYTES:
            raise SystemExit(f"flash offset is outside the 4 MiB device: {offset_text}")

        relative_source = Path(source_text)
        if relative_source.is_absolute() or ".." in relative_source.parts:
            raise SystemExit(f"unsafe or missing build artifact: {source_text}")
        source_candidate = build_dir / relative_source
        cursor = source_candidate
        while cursor != build_dir:
            if cursor.is_symlink():
                raise SystemExit(f"unsafe or missing build artifact: {source_text}")
            cursor = cursor.parent
        source = source_candidate.resolve()
        if build_dir not in source.parents or not source.is_file() or source.suffix != ".bin":
            raise SystemExit(f"unsafe or missing build artifact: {source_text}")
        size = source.stat().st_size
        if size <= 0 or offset + size > FLASH_SIZE_BYTES:
            raise SystemExit(f"build artifact exceeds the 4 MiB device: {source_text}")
        artifacts.append(Artifact(offset, source, f"{offset:06x}-{source.name}", size))

    artifacts.sort(key=lambda artifact: artifact.offset)
    for previous, current in zip(artifacts, artifacts[1:]):
        if previous.offset + previous.size > current.offset:
            raise SystemExit(
                f"overlapping flash artifacts: {previous.source.name} and {current.source.name}"
            )
    if len({artifact.destination_name for artifact in artifacts}) != len(artifacts):
        raise SystemExit("flash artifacts produce duplicate destination names")
    return artifacts


def expected_flash_layout(project_name: str) -> dict[int, tuple[Path, int]]:
    return {
        0x000000: (Path("bootloader/bootloader.bin"), 0x008000),
        0x008000: (Path("partition_table/partition-table.bin"), 0x009000),
        0x00F000: (Path("ota_data_initial.bin"), 0x011000),
        0x020000: (Path(f"{project_name}.bin"), 0x1B0000),
    }


def validate_artifact_roles(
    build_dir: Path,
    artifacts: list[Artifact],
    expected_layout: dict[int, tuple[Path, int]],
) -> None:
    for artifact in artifacts:
        expected_role = expected_layout.get(artifact.offset)
        try:
            actual_source = artifact.source.relative_to(build_dir)
        except ValueError as error:
            raise SystemExit("artifact role mismatch: source escaped the build directory") from error
        if expected_role is None:
            raise SystemExit(f"artifact role mismatch at {artifact.offset:#x}: no artifact expected")
        expected_source, region_end = expected_role
        if actual_source != expected_source:
            raise SystemExit(
                f"artifact role mismatch at {artifact.offset:#x}: "
                f"expected {expected_source.as_posix()}, got {actual_source.as_posix()}"
            )
        if artifact.offset + artifact.size > region_end:
            raise SystemExit(
                f"artifact exceeds assigned flash region at {artifact.offset:#x}: "
                f"{artifact.size} bytes ends after {region_end:#x}"
            )


def validate_build_identity(
    build_dir: Path,
    metadata: object,
    requested_version: str,
    project_name: str,
) -> None:
    if not isinstance(metadata, dict):
        raise SystemExit("flasher_args.json must contain a JSON object")
    description_path = build_dir / "project_description.json"
    try:
        description = json.loads(description_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit("missing or malformed project_description.json") from error
    if not isinstance(description, dict):
        raise SystemExit("project_description.json must contain a JSON object")

    expected_description = {
        "project_name": project_name,
        "project_version": requested_version,
        "git_revision": EXPECTED_IDF_VERSION,
        "target": EXPECTED_TARGET,
    }
    for key, expected in expected_description.items():
        if description.get(key) != expected:
            raise SystemExit(
                f"unexpected {key} in project_description.json: expected {expected!r}"
            )

    extra_args = metadata.get("extra_esptool_args")
    flash_settings = metadata.get("flash_settings")
    if not isinstance(extra_args, dict) or extra_args.get("chip") != EXPECTED_TARGET:
        raise SystemExit("flasher_args.json is not an ESP32-C6 build")
    expected_settings = {"flash_mode": "dio", "flash_size": "4MB", "flash_freq": "80m"}
    if not isinstance(flash_settings, dict) or any(
        flash_settings.get(key) != value for key, value in expected_settings.items()
    ):
        raise SystemExit("flasher_args.json does not match the exact-board flash settings")


def publish(output_dir: Path, staging_dir: Path) -> None:
    backup_dir = staging_dir / "previous"
    backup_dir.mkdir()
    moved_old: list[str] = []
    moved_new: list[str] = []
    try:
        for name in PUBLICATION_NAMES:
            current = output_dir / name
            if current.exists() or current.is_symlink():
                current.replace(backup_dir / name)
                moved_old.append(name)
        for name in PUBLICATION_NAMES:
            (staging_dir / "next" / name).replace(output_dir / name)
            moved_new.append(name)
    except Exception:
        for name in reversed(moved_new):
            current = output_dir / name
            if current.is_dir() and not current.is_symlink():
                shutil.rmtree(current)
            else:
                current.unlink(missing_ok=True)
        for name in moved_old:
            (backup_dir / name).replace(output_dir / name)
        raise


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--project-name", default=DEFAULT_PROJECT_NAME)
    parser.add_argument("--release-name", default=DEFAULT_RELEASE_NAME)
    args = parser.parse_args()

    if not VERSION_PATTERN.fullmatch(args.version):
        raise SystemExit("version must be 1-64 ASCII letters, digits, dots, pluses, or hyphens")
    if not PROJECT_NAME_PATTERN.fullmatch(args.project_name):
        raise SystemExit("project name must be 1-64 safe ASCII filename characters")
    if not RELEASE_NAME_PATTERN.fullmatch(args.release_name):
        raise SystemExit("release name must be 1-80 printable ASCII characters")

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    if (
        output_dir == build_dir
        or build_dir in output_dir.parents
        or output_dir in build_dir.parents
    ):
        raise SystemExit("output directory and ESP-IDF build directory must be disjoint")

    metadata_path = build_dir / "flasher_args.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit("missing or malformed flasher_args.json") from error
    validate_build_identity(build_dir, metadata, args.version, args.project_name)
    artifacts = validated_artifacts(build_dir, metadata.get("flash_files"))
    actual_offsets = {artifact.offset for artifact in artifacts}
    if actual_offsets != EXPECTED_FLASH_OFFSETS:
        raise SystemExit(
            "exact-board OTA release requires bootloader, partition table, and app at "
            "0x0, 0x8000, and 0x20000, plus OTA data at 0xf000"
        )
    validate_artifact_roles(build_dir, artifacts, expected_flash_layout(args.project_name))
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=".flasher-package-", dir=output_dir) as temporary:
        staging_dir = Path(temporary)
        next_dir = staging_dir / "next"
        firmware_dir = next_dir / "firmware"
        firmware_dir.mkdir(parents=True)

        parts: list[dict[str, object]] = []
        release_files: list[dict[str, object]] = []
        for artifact in artifacts:
            destination = firmware_dir / artifact.destination_name
            source_hash_before = sha256(artifact.source)
            shutil.copyfile(artifact.source, destination)
            published_size = destination.stat().st_size
            published_hash = sha256(destination)
            source_hash_after = sha256(artifact.source)
            if (
                published_size != artifact.size
                or published_hash != source_hash_before
                or source_hash_after != source_hash_before
            ):
                raise SystemExit(f"build artifact changed while packaging: {artifact.source.name}")
            relative = f"firmware/{artifact.destination_name}"
            parts.append({"path": relative, "offset": artifact.offset})
            release_files.append({
                "path": relative,
                "offset": artifact.offset,
                "bytes": published_size,
                "sha256": published_hash,
            })

        manifest = {
            "name": args.release_name,
            "version": args.version,
            "new_install_improv_wait_time": 10,
            "builds": [{"chipFamily": "ESP32-C6", "parts": parts}],
        }
        release = {
            "name": manifest["name"],
            "version": args.version,
            "esp_idf": "v5.5.5",
            "files": release_files,
        }
        (next_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        (next_dir / "release.json").write_text(json.dumps(release, indent=2) + "\n", encoding="utf-8")
        publish(output_dir, staging_dir)

    print(f"packaged {len(artifacts)} flash parts in {output_dir}")


if __name__ == "__main__":
    main()
