#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

CHECKER_PATH = Path(__file__).with_name("check_coverage.py")
SPEC = importlib.util.spec_from_file_location("glucose_coverage_checker", CHECKER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {CHECKER_PATH}")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class CoverageBranchParserTests(unittest.TestCase):
    def test_accepts_exact_complete_nonzero_branch_records(self) -> None:
        report = (
            "function protected called 1 returned 100% blocks executed 100%\n"
            "branch  0 taken 1 (fallthrough)\n"
            "branch  1 taken 2\n"
            "function next called 1 returned 100% blocks executed 100%\n"
        )
        CHECKER.require_complete_function_branches(report, "protected", 2)

    def test_rejects_never_executed_branch(self) -> None:
        report = (
            "function protected called 1 returned 100% blocks executed 80%\n"
            "branch  0 taken 1\n"
            "branch  1 never executed\n"
        )
        with self.assertRaisesRegex(SystemExit, "unexecuted branch direction"):
            CHECKER.require_complete_function_branches(report, "protected", 2)

    def test_rejects_zero_taken_branch(self) -> None:
        report = (
            "function protected called 1 returned 100% blocks executed 100%\n"
            "branch  0 taken 1\n"
            "branch  1 taken 0 (fallthrough)\n"
        )
        with self.assertRaisesRegex(SystemExit, "unexecuted branch direction"):
            CHECKER.require_complete_function_branches(report, "protected", 2)

    def test_rejects_missing_branch_record(self) -> None:
        report = (
            "function protected called 1 returned 100% blocks executed 100%\n"
            "branch  0 taken 1\n"
        )
        with self.assertRaisesRegex(SystemExit, "expected 2 branch records, found 1"):
            CHECKER.require_complete_function_branches(report, "protected", 2)


if __name__ == "__main__":
    unittest.main()
