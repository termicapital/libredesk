from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).with_name("package_firmware.py")
SPEC = importlib.util.spec_from_file_location("flasher_package_firmware", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load package_firmware.py")
package_firmware = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = package_firmware
SPEC.loader.exec_module(package_firmware)


class PackageFirmwareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.output = self.root / "output"
        (self.build / "bootloader").mkdir(parents=True)
        (self.build / "partition_table").mkdir()
        (self.build / "bootloader" / "bootloader.bin").write_bytes(b"B" * 256)
        (self.build / "partition_table" / "partition-table.bin").write_bytes(b"P" * 128)
        (self.build / "ota_data_initial.bin").write_bytes(b"O" * 128)
        (self.build / "libre_desk_display_spike.bin").write_bytes(b"A" * 1024)
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "libre_desk_display_spike.bin",
        })
        self.write_project_description()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_flash_map(
        self,
        flash_files: dict[str, str],
        *,
        chip: str = "esp32c6",
        flash_size: str = "4MB",
    ) -> None:
        (self.build / "flasher_args.json").write_text(
            json.dumps({
                "flash_files": flash_files,
                "flash_settings": {
                    "flash_mode": "dio",
                    "flash_size": flash_size,
                    "flash_freq": "80m",
                },
                "extra_esptool_args": {"chip": chip},
            }),
            encoding="utf-8",
        )

    def write_project_description(
        self,
        *,
        version: str = "0.1.0-test",
        target: str = "esp32c6",
        idf_version: str = "v5.5.5",
        project_name: str = "libre_desk_display_spike",
    ) -> None:
        (self.build / "project_description.json").write_text(
            json.dumps({
                "project_name": project_name,
                "project_version": version,
                "git_revision": idf_version,
                "target": target,
            }),
            encoding="utf-8",
        )

    def run_packager(self, version: str = "0.1.0-test") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--output-dir",
                str(self.output),
                "--version",
                version,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_packages_generated_flash_map_and_hashes(self) -> None:
        result = self.run_packager()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        release = json.loads((self.output / "release.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["new_install_improv_wait_time"], 10)
        self.assertEqual(manifest["builds"][0]["chipFamily"], "ESP32-C6")
        self.assertEqual(
            [part["offset"] for part in manifest["builds"][0]["parts"]],
            [0, 0x8000, 0xF000, 0x20000],
        )
        for entry in release["files"]:
            published = self.output / entry["path"]
            self.assertEqual(entry["bytes"], published.stat().st_size)
            self.assertEqual(entry["sha256"], hashlib.sha256(published.read_bytes()).hexdigest())

    def test_packages_explicit_project_identity_and_release_name(self) -> None:
        (self.build / "libre_desk_display_spike.bin").rename(self.build / "libre_desk_mvp.bin")
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "libre_desk_mvp.bin",
        })
        self.write_project_description(project_name="libre_desk_mvp")
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--output-dir",
                str(self.output),
                "--version",
                "0.1.0-test",
                "--project-name",
                "libre_desk_mvp",
                "--release-name",
                "Libre Desk live-data MVP",
            ],
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["name"], "Libre Desk live-data MVP")
        self.assertEqual(manifest["builds"][0]["parts"][-1]["path"], "firmware/020000-libre_desk_mvp.bin")

    def test_rejects_path_traversal_without_destroying_previous_release(self) -> None:
        self.assertEqual(self.run_packager().returncode, 0)
        manifest_before = (self.output / "manifest.json").read_bytes()
        firmware_before = {
            path.name: path.read_bytes() for path in (self.output / "firmware").glob("*.bin")
        }
        (self.root / "outside.bin").write_bytes(b"outside")
        self.write_flash_map({"0x0": "../outside.bin"})

        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.output / "manifest.json").read_bytes(), manifest_before)
        self.assertEqual(
            {path.name: path.read_bytes() for path in (self.output / "firmware").glob("*.bin")},
            firmware_before,
        )

    def test_rejects_symlinked_artifact_inside_build_directory(self) -> None:
        (self.build / "app-link.bin").symlink_to("libre_desk_display_spike.bin")
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "app-link.bin",
        })
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe or missing build artifact", result.stderr)

    def test_rejects_same_size_copy_corruption(self) -> None:
        def corrupt_copy(source: str | Path, destination: str | Path) -> None:
            content = Path(source).read_bytes()
            Path(destination).write_bytes(bytes([content[0] ^ 1]) + content[1:])

        argv = [
            str(SCRIPT),
            "--build-dir",
            str(self.build),
            "--output-dir",
            str(self.output),
            "--version",
            "0.1.0-test",
        ]
        with mock.patch.object(sys, "argv", argv):
            with mock.patch.object(package_firmware.shutil, "copyfile", corrupt_copy):
                with self.assertRaisesRegex(SystemExit, "changed while packaging"):
                    package_firmware.main()
        self.assertFalse((self.output / "manifest.json").exists())

    def test_rejects_overlapping_flash_regions(self) -> None:
        (self.build / "bootloader" / "bootloader.bin").write_bytes(b"B" * 0x9000)
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlapping flash artifacts", result.stderr)

    def test_rejects_artifact_spill_beyond_assigned_partition(self) -> None:
        spill_cases = [
            (self.build / "partition_table" / "partition-table.bin", b"P", 0x1001),
            (self.build / "ota_data_initial.bin", b"O", 0x2001),
            (self.build / "libre_desk_display_spike.bin", b"A", 0x190001),
        ]
        for path, fill, size in spill_cases:
            with self.subTest(path=path.relative_to(self.build).as_posix()):
                original = path.read_bytes()
                try:
                    path.write_bytes(fill * size)
                    result = self.run_packager()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("assigned flash region", result.stderr)
                finally:
                    path.write_bytes(original)

    def test_rejects_negative_offset(self) -> None:
        self.write_flash_map({"-0x1": "libre_desk_display_spike.bin"})
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the 4 MiB device", result.stderr)

    def test_rejects_invalid_version(self) -> None:
        result = self.run_packager("bad version")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version must be", result.stderr)

    def test_rejects_unsafe_project_name_before_publication(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--output-dir",
                str(self.output),
                "--version",
                "0.1.0-test",
                "--project-name",
                "../libre_desk_mvp",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("project name must be", result.stderr)
        self.assertFalse((self.output / "manifest.json").exists())

    def test_rejects_control_characters_in_release_name(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--output-dir",
                str(self.output),
                "--version",
                "0.1.0-test",
                "--release-name",
                "Libre Desk\nMVP",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release name must be", result.stderr)
        self.assertFalse((self.output / "manifest.json").exists())

    def test_rejects_output_directory_that_contains_build_directory(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--output-dir",
                str(self.root),
                "--version",
                "0.1.0-test",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be disjoint", result.stderr)
        self.assertTrue((self.build / "flasher_args.json").is_file())

    def test_rejects_wrong_chip_identity(self) -> None:
        self.write_project_description(target="esp32s3")
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected target", result.stderr)

    def test_rejects_esptool_chip_mismatch(self) -> None:
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "libre_desk_display_spike.bin",
        }, chip="esp32s3")
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not an ESP32-C6 build", result.stderr)

    def test_rejects_wrong_idf_version(self) -> None:
        self.write_project_description(idf_version="v5.6.0")
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected git_revision", result.stderr)

    def test_rejects_version_that_does_not_match_built_image(self) -> None:
        result = self.run_packager("0.2.0-test")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected project_version", result.stderr)

    def test_rejects_incomplete_flash_layout(self) -> None:
        self.write_flash_map({"0x20000": "libre_desk_display_spike.bin"})
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires bootloader, partition table, and app", result.stderr)

    def test_rejects_obsolete_single_app_offset(self) -> None:
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x10000": "libre_desk_display_spike.bin",
        })
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0x20000", result.stderr)

    def test_rejects_missing_ota_data_initializer(self) -> None:
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0x20000": "libre_desk_display_spike.bin",
        })
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OTA data at 0xf000", result.stderr)

    def test_rejects_artifacts_swapped_between_required_offsets(self) -> None:
        swapped_maps = [
            {
                "0x0": "libre_desk_display_spike.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0xf000": "ota_data_initial.bin",
                "0x20000": "bootloader/bootloader.bin",
            },
            {
                "0x0": "bootloader/bootloader.bin",
                "0x8000": "ota_data_initial.bin",
                "0xf000": "partition_table/partition-table.bin",
                "0x20000": "libre_desk_display_spike.bin",
            },
        ]
        for flash_map in swapped_maps:
            with self.subTest(flash_map=flash_map):
                self.write_flash_map(flash_map)
                result = self.run_packager()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("artifact role mismatch", result.stderr)

    def test_rejects_wrong_flash_size(self) -> None:
        self.write_flash_map({
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "libre_desk_display_spike.bin",
        }, flash_size="8MB")
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the exact-board flash settings", result.stderr)

    def test_publish_restores_previous_release_on_replacement_failure(self) -> None:
        old_firmware = self.output / "firmware"
        old_firmware.mkdir(parents=True)
        (old_firmware / "old.bin").write_bytes(b"old firmware")
        (self.output / "manifest.json").write_text("old manifest", encoding="utf-8")
        (self.output / "release.json").write_text("old release", encoding="utf-8")

        with tempfile.TemporaryDirectory(dir=self.output) as temporary:
            staging = Path(temporary)
            next_dir = staging / "next"
            (next_dir / "firmware").mkdir(parents=True)
            (next_dir / "firmware" / "new.bin").write_bytes(b"new firmware")
            (next_dir / "manifest.json").write_text("new manifest", encoding="utf-8")
            (next_dir / "release.json").write_text("new release", encoding="utf-8")
            real_replace = Path.replace

            def fail_on_new_manifest(path: Path, target: Path) -> Path:
                if path.parent == next_dir and path.name == "manifest.json":
                    raise OSError("simulated publication failure")
                return real_replace(path, target)

            with mock.patch.object(Path, "replace", fail_on_new_manifest):
                with self.assertRaisesRegex(OSError, "simulated publication failure"):
                    package_firmware.publish(self.output, staging)

        self.assertEqual((self.output / "firmware" / "old.bin").read_bytes(), b"old firmware")
        self.assertEqual((self.output / "manifest.json").read_text(encoding="utf-8"), "old manifest")
        self.assertEqual((self.output / "release.json").read_text(encoding="utf-8"), "old release")


if __name__ == "__main__":
    unittest.main()
