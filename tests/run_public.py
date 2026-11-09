#!/usr/bin/env python3
"""Run the public MiniVectorDB cases against a built executable."""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def scalar_equal(actual: str, expected):
    if isinstance(expected, bool):
        return actual == ("1" if expected else "0")
    if isinstance(expected, int):
        try:
            return int(actual) == expected
        except ValueError:
            return False
    if isinstance(expected, float):
        try:
            value = float(actual)
        except ValueError:
            return False
        return math.isclose(value, expected, rel_tol=1e-5, abs_tol=1e-5)
    return actual == str(expected)


def rows_equal(actual: list[list[str]], expected: dict) -> bool:
    expected_rows = expected.get("rows", [])
    if "columns" in expected and any(
        len(row) != len(expected["columns"]) for row in actual
    ):
        return False
    if len(actual) != len(expected_rows):
        return False
    def matches(left, right):
        return len(left) == len(right) and all(
            scalar_equal(a, e) for a, e in zip(left, right)
        )
    if expected.get("unordered"):
        remaining = list(actual)
        for wanted in expected_rows:
            for index, got in enumerate(remaining):
                if matches(got, wanted):
                    remaining.pop(index)
                    break
            else:
                return False
        return not remaining
    return all(matches(a, e) for a, e in zip(actual, expected_rows))


def run_case(executable: Path, case: dict, case_dir: Path) -> list[str]:
    sql_lines = [
        line
        for line in (case_dir / f"{case['id']}.test").read_text(
            encoding="utf-8"
        ).splitlines()
        if line.strip()
    ]
    expected = json.loads(
        (case_dir / f"{case['id']}.result").read_text(encoding="utf-8")
    )
    if len(sql_lines) != len(expected):
        raise AssertionError("test/result length mismatch")

    data_dir = tempfile.mkdtemp(prefix="mvd-public-")
    try:
        process = subprocess.run(
            [str(executable), f"--data-dir={data_dir}", "--column-header"],
            input="\n".join(sql_lines) + "\n",
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
    finally:
        if os.name != "nt":
            shutil.rmtree(data_dir, ignore_errors=True)
    if process.returncode != 0:
        raise AssertionError(
            f"database exited {process.returncode}\nstderr:\n{process.stderr}"
        )

    failures: list[str] = []
    output = process.stdout.splitlines()
    cursor = 0
    for index, (sql, expectation) in enumerate(zip(sql_lines, expected), 1):
        keyword = sql.lstrip().split(None, 1)[0].lower()
        if keyword == "select":
            header = output[cursor].split("\t") if cursor < len(output) else []
            cursor += 1
            if not header or header[0] != "COLUMNS":
                failures.append(f"{case['id']} statement {index}: missing column metadata")
            elif "columns" in expectation and header[1:] != expectation["columns"]:
                failures.append(
                    f"{case['id']} statement {index}: column names mismatch\n"
                    f"SQL: {sql}\nactual={header[1:]!r}\nexpected={expectation['columns']!r}"
                )
            count = len(expectation.get("rows", []))
            actual = [line.split("\t") for line in output[cursor : cursor + count]]
            cursor += count
            if not rows_equal(actual, expectation):
                failures.append(
                    f"{case['id']} statement {index}: rows mismatch\n"
                    f"SQL: {sql}\nactual={actual!r}\nexpected={expectation!r}"
                )
        else:
            if cursor >= len(output):
                failures.append(
                    f"{case['id']} statement {index}: missing status line"
                )
                continue
            status = output[cursor]
            cursor += 1
            if "affected_rows" in expectation:
                wanted = f"OK {expectation['affected_rows']}"
                if status != wanted:
                    failures.append(
                        f"{case['id']} statement {index}: expected {wanted!r}, "
                        f"got {status!r}; SQL: {sql}"
                    )
            else:
                if status != "OK":
                    failures.append(
                        f"{case['id']} statement {index}: expected OK, got {status!r}"
                    )
    if cursor != len(output):
        failures.append(
            f"{case['id']}: {len(output) - cursor} extra output line(s): "
            f"{output[cursor:]!r}"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument(
        "--case-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "mysqltest" / "cases",
    )
    args = parser.parse_args()
    executable = args.executable.resolve()
    manifest = json.loads(
        (args.case_dir.parent / "manifest.json").read_text(encoding="utf-8")
    )
    failures: list[str] = []
    for case in manifest["cases"]:
        case_failures = run_case(executable, case, args.case_dir)
        if case_failures:
            failures.extend(case_failures)
            print(f"FAIL {case['id']}")
        else:
            print(f"PASS {case['id']}")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"PASS {len(manifest['cases'])} public cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
