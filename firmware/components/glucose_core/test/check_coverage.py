#!/usr/bin/env python3
"""Fail the native gate when glucose-core state-machine coverage regresses."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import NoReturn


def fail(message: str) -> NoReturn:
    raise SystemExit(f"coverage check failed: {message}")


def function_block(report: str, function_name: str) -> str:
    marker = f"function {function_name} "
    start = report.find(marker)
    if start < 0:
        fail(f"missing gcov function record for {function_name}")
    next_function = report.find("\nfunction ", start + len(marker))
    return report[start:] if next_function < 0 else report[start:next_function]


def require_complete_function_branches(
    report: str,
    function_name: str,
    expected_records: int,
) -> None:
    block = function_block(report, function_name)
    branch_records = re.findall(
        r"^branch\s+\d+\s+(?:taken\s+[^\n]+|never executed)$",
        block,
        re.MULTILINE,
    )
    if len(branch_records) != expected_records:
        fail(
            f"{function_name}: expected {expected_records} branch records, "
            f"found {len(branch_records)}"
        )
    for record in branch_records:
        if "never executed" in record or re.search(r"\btaken 0(?:%|\s|$)", record):
            fail(f"unexecuted branch direction in {function_name}: {record}")


def metric(output: str, source_name: str, label: str) -> float:
    file_marker = re.search(rf"File '.*?/{re.escape(source_name)}'\n", output)
    if file_marker is None:
        fail(f"missing gcov output for {source_name}")
    following = output[file_marker.end() :]
    match = re.search(rf"^{re.escape(label)}:([0-9]+(?:\.[0-9]+)?)%", following, re.MULTILINE)
    if match is None:
        fail(f"missing {label!r} metric for {source_name}")
    return float(match.group(1))


def main() -> int:
    if len(sys.argv) != 3:
        fail("usage: check_coverage.py COVERAGE_BUILD COMPONENT_DIR")
    build_dir = Path(sys.argv[1]).resolve()
    component_dir = Path(sys.argv[2]).resolve()
    object_dir = build_dir / "CMakeFiles" / "glucose_core.dir"
    report_dir = build_dir / "gcov-report"
    report_dir.mkdir(parents=True, exist_ok=True)
    notes_files = sorted(object_dir.glob("*.gcno"))
    if not notes_files:
        fail(f"no coverage notes under {object_dir}")

    combined_output: list[str] = []
    for notes_file in notes_files:
        result = subprocess.run(
            ["gcov", "-b", "-c", str(notes_file)],
            cwd=report_dir,
            check=False,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            fail(result.stderr.strip() or result.stdout.strip() or f"gcov failed for {notes_file}")
        combined_output.append(result.stdout)
    output = "\n".join(combined_output)

    core_lines = metric(output, "glucose_core.c", "Lines executed")
    core_branches_executed = metric(output, "glucose_core.c", "Branches executed")
    core_branches_taken = metric(output, "glucose_core.c", "Taken at least once")
    if core_lines < 90.0:
        fail(f"glucose_core.c line coverage {core_lines:.2f}% is below 90%")
    if core_branches_executed < 100.0:
        fail(f"glucose_core.c branch-site coverage {core_branches_executed:.2f}% is below 100%")
    if core_branches_taken < 90.0:
        fail(f"glucose_core.c taken-branch coverage {core_branches_taken:.2f}% is below 90%")

    domain_report_path = report_dir / "domain.c.gcov"
    if not domain_report_path.is_file():
        fail(f"missing {domain_report_path}")
    domain_report = domain_report_path.read_text(encoding="utf-8")
    freshness_functions = {
        "glucose_freshness_config_is_valid": 8,
        "glucose_freshness_classify": 10,
        "glucose_state_allows_number": 4,
    }
    for function_name, expected_records in freshness_functions.items():
        require_complete_function_branches(domain_report, function_name, expected_records)

    print(
        "coverage-ok "
        f"core-lines={core_lines:.2f}% "
        f"core-branch-sites={core_branches_executed:.2f}% "
        f"core-branches-taken={core_branches_taken:.2f}% "
        "freshness-branches=100%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
