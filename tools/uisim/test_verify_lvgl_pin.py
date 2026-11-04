#!/usr/bin/env python3
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from verify_lvgl_pin import (
    EXPECTED_COMPONENT_HASH,
    EXPECTED_TREE_DIGEST,
    PinError,
    canonical_tree_digest,
    verify_lvgl_pin,
)


EXACT_HEADER = """#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 5
#define LVGL_VERSION_PATCH 0
"""


def lock_text(version: str = "9.5.0", component_hash: str = EXPECTED_COMPONENT_HASH) -> str:
    return f"""dependencies:
  idf:
    version: 5.5.5
  lvgl/lvgl:
    component_hash: {component_hash}
    dependencies: []
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: {version}
direct_dependencies:
- lvgl/lvgl
"""


class PinVerifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "managed_components" / "lvgl__lvgl"
        self.source.mkdir(parents=True)
        (self.source / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
        (self.source / "lv_version.h").write_text(EXACT_HEADER, encoding="utf-8")
        (self.source / ".component_hash").write_text(EXPECTED_COMPONENT_HASH, encoding="ascii")
        (self.source / "src").mkdir()
        (self.source / "src" / "widget.c").write_text(
            "int fixture_widget(void) { return 1; }\n", encoding="utf-8"
        )
        (self.source / "widget-link.c").symlink_to("src/widget.c")
        self.lock = self.root / "dependencies.lock"
        self.lock.write_text(lock_text(), encoding="utf-8")
        self.expected_tree_digest = canonical_tree_digest(self.source)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def verify(
        self,
        source: Path | None = None,
        expected_tree_digest: str | None = None,
    ) -> None:
        verify_lvgl_pin(
            source or self.source,
            self.source,
            self.lock,
            expected_tree_digest=(
                self.expected_tree_digest
                if expected_tree_digest is None
                else expected_tree_digest
            ),
        )

    def test_exact_managed_source_passes(self) -> None:
        self.verify()

    def test_commented_definitions_do_not_count(self) -> None:
        (self.source / "lv_version.h").write_text(
            "// #define LVGL_VERSION_MAJOR 9\n"
            "// #define LVGL_VERSION_MINOR 5\n"
            "// #define LVGL_VERSION_PATCH 0\n",
            encoding="utf-8",
        )
        with self.assertRaises(PinError):
            self.verify()

    def test_numeric_prefixes_do_not_count(self) -> None:
        (self.source / "lv_version.h").write_text(
            "#define LVGL_VERSION_MAJOR 90\n"
            "#define LVGL_VERSION_MINOR 50\n"
            "#define LVGL_VERSION_PATCH 01\n",
            encoding="utf-8",
        )
        with self.assertRaises(PinError):
            self.verify()

    def test_duplicate_version_definition_fails(self) -> None:
        (self.source / "lv_version.h").write_text(
            EXACT_HEADER + "#define LVGL_VERSION_MAJOR 9\n", encoding="utf-8"
        )
        with self.assertRaises(PinError):
            self.verify()

    def test_wrong_lock_version_fails(self) -> None:
        self.lock.write_text(lock_text(version="9.5.1"), encoding="utf-8")
        with self.assertRaises(PinError):
            self.verify()

    def test_wrong_component_hash_fails(self) -> None:
        (self.source / ".component_hash").write_text("0" * 64, encoding="ascii")
        with self.assertRaises(PinError):
            self.verify()

    def test_wrong_source_directory_fails(self) -> None:
        other = self.root / "other-lvgl"
        other.mkdir()
        (other / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
        (other / "lv_version.h").write_text(EXACT_HEADER, encoding="utf-8")
        (other / ".component_hash").write_text(EXPECTED_COMPONENT_HASH, encoding="ascii")
        with self.assertRaises(PinError):
            self.verify(other)

    def test_source_content_mutation_fails_with_unchanged_metadata(self) -> None:
        sentinel = (self.source / ".component_hash").read_bytes()
        version = (self.source / "lv_version.h").read_bytes()
        source_file = self.source / "src" / "widget.c"
        source_file.write_text("int fixture_widget(void) { return 2; }\n", encoding="utf-8")

        self.assertEqual((self.source / ".component_hash").read_bytes(), sentinel)
        self.assertEqual((self.source / "lv_version.h").read_bytes(), version)
        with self.assertRaisesRegex(PinError, "source-tree digest mismatch"):
            self.verify()

    def test_source_path_rename_fails(self) -> None:
        (self.source / "src" / "widget.c").rename(self.source / "src" / "renamed.c")
        with self.assertRaisesRegex(PinError, "source-tree digest mismatch"):
            self.verify()

    def test_symlink_target_mutation_fails(self) -> None:
        link = self.source / "widget-link.c"
        link.unlink()
        link.symlink_to("CMakeLists.txt")
        with self.assertRaisesRegex(PinError, "source-tree digest mismatch"):
            self.verify()

    def test_embedded_digest_matches_current_managed_tree(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        managed_tree = repository / "firmware" / "managed_components" / "lvgl__lvgl"
        self.assertEqual(canonical_tree_digest(managed_tree), EXPECTED_TREE_DIGEST)


if __name__ == "__main__":
    unittest.main()
