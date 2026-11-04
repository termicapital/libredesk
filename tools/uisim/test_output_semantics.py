#!/usr/bin/env python3
from __future__ import annotations

import copy
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from verify_lvgl_pin import EXPECTED_COMPONENT_HASH, EXPECTED_TREE_DIGEST

from test_outputs import (
    EXPECTED_SCREENS,
    NO_NUMBER_RGB_SHA256,
    NO_NUMBER_SCREENS,
    SSID_LABEL_BOUNDS,
    SSID_LABEL_RGB_SHA256,
    TREND_ARROW_BOUNDS,
    TREND_ARROW_EXPECTED_BOUNDS,
    TREND_SCREEN_DIRECTIONS,
    dominant_visible_color,
    expected_artifact_files,
    foreground_mask,
    has_long_vertical_run,
    mask_centroid_y,
    region_sha256,
    translated_rgb,
    validate_artifact_set,
    verify_artifacts,
)


def canvas(width: int, height: int, color: tuple[int, int, int] = (5, 8, 10)) -> bytearray:
    return bytearray(color * (width * height))


def set_pixel(pixels: bytearray, width: int, x: int, y: int, color: tuple[int, int, int]) -> None:
    start = (y * width + x) * 3
    pixels[start : start + 3] = bytes(color)


class OutputSemanticHelpersTests(unittest.TestCase):
    def test_vertical_segment_is_detected_but_horizontal_dash_is_not(self) -> None:
        width = height = 24
        vertical = canvas(width, height)
        for y in range(4, 18):
            for x in range(9, 12):
                set_pixel(vertical, width, x, y, (125, 137, 145))
        self.assertTrue(has_long_vertical_run(bytes(vertical), width, 0, 0, 24, 24, 10))

        horizontal = canvas(width, height)
        for y in range(10, 13):
            for x in range(4, 20):
                set_pixel(horizontal, width, x, y, (125, 137, 145))
        self.assertFalse(has_long_vertical_run(bytes(horizontal), width, 0, 0, 24, 24, 10))

    def test_dominant_visible_color_ignores_background(self) -> None:
        pixels = canvas(4, 4)
        for x, y in ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1)):
            set_pixel(pixels, 4, x, y, (230, 52, 66))
        for x, y in ((1, 1), (2, 1)):
            set_pixel(pixels, 4, x, y, (125, 137, 145))
        self.assertEqual(dominant_visible_color(bytes(pixels)), (230, 52, 66))

    def test_translation_uses_background_for_exposed_pixels(self) -> None:
        pixels = bytes((10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120))
        translated = translated_rgb(pixels, 2, 2, 1, -1, (5, 8, 10))
        self.assertEqual(
            translated,
            bytes((5, 8, 10, 70, 80, 90, 5, 8, 10, 5, 8, 10)),
        )

    def test_foreground_mask_ignores_background_and_pixels_outside_bounds(self) -> None:
        pixels = canvas(4, 3)
        set_pixel(pixels, 4, 0, 0, (240, 240, 240))
        set_pixel(pixels, 4, 1, 1, (240, 240, 240))
        set_pixel(pixels, 4, 3, 2, (100, 110, 120))
        mask = foreground_mask(bytes(pixels), 4, (1, 1, 4, 3), (5, 8, 10))
        self.assertEqual(mask, frozenset({(1, 1), (3, 2)}))
        self.assertEqual(mask_centroid_y(mask), 1.5)

    def test_mask_centroid_rejects_an_empty_mask(self) -> None:
        with self.assertRaisesRegex(SystemExit, "empty foreground mask"):
            mask_centroid_y(frozenset())

    def test_region_sha256_hashes_only_the_requested_decoded_rgb_region(self) -> None:
        pixels = canvas(3, 2)
        colors = (
            (10, 11, 12),
            (20, 21, 22),
            (30, 31, 32),
            (40, 41, 42),
            (50, 51, 52),
            (60, 61, 62),
        )
        for index, color in enumerate(colors):
            set_pixel(pixels, 3, index % 3, index // 3, color)
        bounds = (1, 0, 3, 2)
        expected_bytes = bytes(colors[1] + colors[2] + colors[4] + colors[5])
        expected_hash = hashlib.sha256(expected_bytes).hexdigest()
        self.assertEqual(region_sha256(bytes(pixels), 3, bounds), expected_hash)

        changed_outside = bytearray(pixels)
        set_pixel(changed_outside, 3, 0, 0, (90, 91, 92))
        self.assertEqual(region_sha256(bytes(changed_outside), 3, bounds), expected_hash)

        changed_inside = bytearray(pixels)
        set_pixel(changed_inside, 3, 1, 0, (90, 91, 92))
        self.assertNotEqual(region_sha256(bytes(changed_inside), 3, bounds), expected_hash)

    def test_no_number_rgb_hash_map_covers_all_12_no_number_screens(self) -> None:
        self.assertEqual(set(NO_NUMBER_RGB_SHA256), NO_NUMBER_SCREENS)
        self.assertEqual(len(NO_NUMBER_RGB_SHA256), 12)
        for digest in NO_NUMBER_RGB_SHA256.values():
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_trend_fixture_map_covers_every_direction_and_uses_expected_screens(self) -> None:
        self.assertEqual(
            set(TREND_SCREEN_DIRECTIONS),
            {
                "rising",
                "rising_slightly",
                "stable",
                "falling_slightly",
                "falling",
                "rising_fast",
                "falling_fast",
                "unknown",
            },
        )
        self.assertEqual(len(set(TREND_SCREEN_DIRECTIONS.values())), 8)
        self.assertLessEqual(set(TREND_SCREEN_DIRECTIONS.values()), set(EXPECTED_SCREENS))
        self.assertEqual(len(TREND_ARROW_BOUNDS), 4)
        self.assertEqual(set(TREND_ARROW_EXPECTED_BOUNDS), set(TREND_SCREEN_DIRECTIONS))
        self.assertIsNone(TREND_ARROW_EXPECTED_BOUNDS["unknown"])
        for direction, bounds in TREND_ARROW_EXPECTED_BOUNDS.items():
            if direction != "unknown":
                self.assertIsNotNone(bounds)
                self.assertEqual(len(bounds), 4)

    def test_ssid_label_hash_reference_is_a_complete_sha256(self) -> None:
        self.assertEqual(SSID_LABEL_BOUNDS, (28, 121, 212, 141))
        self.assertRegex(SSID_LABEL_RGB_SHA256, r"^[0-9a-f]{64}$")


class ManifestValidationTests(unittest.TestCase):
    @staticmethod
    def manifest() -> dict[str, object]:
        return {
            "schema": 1,
            "screen_width": 240,
            "screen_height": 240,
            "lvgl_component_hash": EXPECTED_COMPONENT_HASH,
            "lvgl_tree_sha256": EXPECTED_TREE_DIGEST,
            "contact_sheet": {
                "file": "contact-sheet.png",
                "width": 1020,
                "height": 1524,
                "sha256": "0" * 64,
            },
            "screens": [
                {
                    "name": name,
                    "file": f"{name}.png",
                    "sha256": "0" * 64,
                    "shows_value": True,
                }
                for name in EXPECTED_SCREENS
            ],
        }

    @staticmethod
    def populate(directory: Path) -> None:
        for file_name in expected_artifact_files():
            (directory / file_name).write_bytes(b"placeholder")

    def test_expected_artifact_set_is_exactly_37_screens_and_three_support_files(self) -> None:
        expected = expected_artifact_files()
        self.assertEqual(len(EXPECTED_SCREENS), 37)
        self.assertEqual(len(expected), 40)
        self.assertEqual(
            expected,
            {f"{name}.png" for name in EXPECTED_SCREENS}
            | {"contact-sheet.png", "index.html", "manifest.json"},
        )

    def test_clock_font_is_enabled_in_host_and_firmware_configs(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        host_config = (repo_root / "tools/uisim/lv_conf.h").read_text(encoding="utf-8")
        firmware_config = (repo_root / "firmware/sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("#define LV_FONT_MONTSERRAT_24 1", host_config)
        self.assertIn("CONFIG_LV_FONT_MONTSERRAT_24=y", firmware_config)

    def test_artifact_set_rejects_both_extra_and_missing_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.populate(directory)
            validate_artifact_set(directory)

            extra = directory / "unexpected.txt"
            extra.write_text("unexpected", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "unexpected artifact set"):
                validate_artifact_set(directory)

            extra.unlink()
            (directory / f"{EXPECTED_SCREENS[0]}.png").unlink()
            with self.assertRaisesRegex(SystemExit, "unexpected artifact set"):
                validate_artifact_set(directory)

    def test_wrong_lvgl_tree_identity_is_rejected_before_artifact_reads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.populate(directory)
            manifest = self.manifest()
            manifest["lvgl_tree_sha256"] = "0" * 64
            with mock.patch("test_outputs.decode_png") as decoder:
                with self.assertRaisesRegex(SystemExit, "LVGL source identity"):
                    verify_artifacts(directory, manifest)
                decoder.assert_not_called()

    def test_malicious_screen_paths_are_rejected_before_any_artifact_read(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.populate(directory)
            malicious_paths = (
                str((directory.parent / "outside.png").resolve()),
                "../outside.png",
                "nested/boot.png",
                "nested\\boot.png",
                "provisioning.png",
            )
            for malicious_path in malicious_paths:
                with self.subTest(path=malicious_path):
                    manifest = copy.deepcopy(self.manifest())
                    manifest["screens"][0]["file"] = malicious_path  # type: ignore[index]
                    with mock.patch("test_outputs.decode_png") as decoder:
                        with self.assertRaisesRegex(SystemExit, "manifest path"):
                            verify_artifacts(directory, manifest)
                        decoder.assert_not_called()

    def test_malicious_contact_paths_are_rejected_before_any_artifact_read(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.populate(directory)
            malicious_paths = (
                str((directory.parent / "outside.png").resolve()),
                "../contact-sheet.png",
                "nested/contact-sheet.png",
                "nested\\contact-sheet.png",
                "boot.png",
            )
            for malicious_path in malicious_paths:
                with self.subTest(path=malicious_path):
                    manifest = copy.deepcopy(self.manifest())
                    manifest["contact_sheet"]["file"] = malicious_path  # type: ignore[index]
                    with mock.patch("test_outputs.decode_png") as decoder:
                        with self.assertRaisesRegex(SystemExit, "manifest path"):
                            verify_artifacts(directory, manifest)
                        decoder.assert_not_called()

    def test_exact_name_symlink_escaping_directory_is_rejected_before_read(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "screens"
            directory.mkdir()
            self.populate(directory)
            external = root / "outside.png"
            external.write_bytes(b"secret")
            screen_path = directory / f"{EXPECTED_SCREENS[0]}.png"
            screen_path.unlink()
            screen_path.symlink_to(external)

            with mock.patch("test_outputs.decode_png") as decoder:
                with self.assertRaisesRegex(SystemExit, "escapes screen directory"):
                    verify_artifacts(directory, self.manifest())
                decoder.assert_not_called()


if __name__ == "__main__":
    unittest.main()
