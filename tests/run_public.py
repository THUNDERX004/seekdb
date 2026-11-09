#!/usr/bin/env python3
"""Run the public MiniVectorDB cases against a built executable."""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import tempfile
import uuid
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


def response_failures(sql: str, expectation: dict, output: list[str]) -> list[str]:
    if "error" in expectation:
        # Semantic-to-code mappings are not frozen in the public package yet.
        # Require exactly one protocol error with a nonempty symbolic/numeric code.
        if len(output) == 1 and re.fullmatch(r"ERROR ([^\s:]+):.*", output[0]):
            return []
        return [f"expected error {expectation['error']!r}, got {output!r}"]

    if sql.split(None, 1)[0].lower() == "select":
        if not output or not output[0].startswith("COLUMNS\t"):
            return [f"missing column metadata or unsuccessful SELECT: {output!r}"]
        columns = output[0].split("\t")[1:]
        rows = [line.split("\t") for line in output[1:]]
        failures = []
        if "columns" in expectation and columns != expectation["columns"]:
            failures.append(
                f"column names mismatch: actual={columns!r}, "
                f"expected={expectation['columns']!r}"
            )
        if any(len(row) != len(columns) for row in rows):
            failures.append(f"row width differs from column metadata: {rows!r}")
        if "rows" in expectation and not rows_equal(rows, expectation):
            failures.append(
                f"rows mismatch: actual={rows!r}, expected={expectation['rows']!r}"
            )
        if "affected_rows" in expectation:
            failures.append("expected affected_rows status, got query rows")
        return failures

    if "columns" in expectation or "rows" in expectation:
        return [f"expected query response, got {output!r}"]
    if "affected_rows" in expectation:
        wanted = f"OK {expectation['affected_rows']}"
        return [] if output == [wanted] else [f"expected {wanted!r}, got {output!r}"]
    if len(output) != 1 or not re.fullmatch(r"OK(?: [0-9]+)?", output[0]):
        return [f"expected successful status, got {output!r}"]
    return []


def run_case(executable: Path, case: dict, case_dir: Path) -> list[str]:
    test_path = (
        case_dir.parent / case["test"]
        if "test" in case else case_dir / f"{case['id']}.test"
    )
    result_path = (
        case_dir.parent / case["result"]
        if "result" in case else case_dir / f"{case['id']}.result"
    )
    expected = json.loads(result_path.read_text(encoding="utf-8"))
    # Each segment is one process lifetime. run() sends EOF and waits before
    # the next segment starts, including when a transaction is still open.
    segments = []
    requests = []
    mode = initial_mode = "exact"
    statement = 0
    for line_number, line in enumerate(
        test_path.read_text(encoding="utf-8").splitlines(), 1
    ):
        sql = line.strip()
        if not sql:
            continue
        if sql == "--restart":
            segments.append((initial_mode, requests))
            initial_mode, requests = mode, []
        elif sql.split() in (["--mode", "ann"], ["--mode", "exact"]):
            mode = sql.split()[1]
            requests.append((None, f"SET vector_mode={mode};"))
        elif sql.startswith("--"):
            raise AssertionError(
                f"{case['id']} line {line_number}: unknown driver directive {sql!r}"
            )
        else:
            statement += 1
            requests.append((statement, sql))
    segments.append((initial_mode, requests))
    if statement != len(expected):
        raise AssertionError(
            f"test/result length mismatch: {statement} SQL, {len(expected)} assertions"
        )
    if "statements" in case and statement != case["statements"]:
        raise AssertionError(
            f"manifest statement count mismatch: {statement} SQL, "
            f"expected {case['statements']}"
        )

    data_dir = tempfile.mkdtemp(prefix="mvd-public-")
    marker = f"__MVD_PUBLIC_{uuid.uuid4().hex}__"
    failures: list[str] = []
    try:
        for generation, (initial_mode, requests) in enumerate(segments, 1):
            process = subprocess.run(
                [
                    str(executable), f"--data-dir={data_dir}", "--column-header",
                    f"--ready-marker={marker}", f"--vector-mode={initial_mode}",
                ],
                input="".join(
                    (sql if sql.endswith(";") else sql + ";") + "\n"
                    for _, sql in requests
                ),
                text=True,
                encoding="utf-8",
                errors="strict",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
                check=False,
            )
            if process.returncode != 0:
                raise AssertionError(
                    f"{case['id']} process {generation}: database exited "
                    f"{process.returncode}\nstderr:\n{process.stderr}"
                )
            frames = []
            pending = []
            for line in process.stdout.splitlines():
                if line == marker:
                    frames.append(pending)
                    pending = []
                else:
                    pending.append(line)
            if pending:
                failures.append(
                    f"{case['id']} process {generation}: output without a ready marker: "
                    f"{pending!r}"
                )
            if len(frames) != len(requests):
                failures.append(
                    f"{case['id']} process {generation}: response count mismatch: "
                    f"{len(frames)} framed responses for {len(requests)} requests"
                )
            for position, (index, sql) in enumerate(requests):
                label = (
                    f"statement {index}" if index is not None else f"directive {sql}"
                )
                if position >= len(frames):
                    failures.append(f"{case['id']} {label}: missing response; SQL: {sql}")
                    continue
                expectation = expected[index - 1] if index is not None else {}
                failures.extend(
                    f"{case['id']} {label}: {failure}; SQL: {sql}"
                    for failure in response_failures(sql, expectation, frames[position])
                )
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)
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
        try:
            case_failures = run_case(executable, case, args.case_dir)
        except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
            case_failures = [f"{case['id']}: {error}"]
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
