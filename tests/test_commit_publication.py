#!/usr/bin/env python3
"""Fail-stop and recovery tests for durable multi-table COMMIT publication.

Usage: python tests/test_commit_publication.py normal.exe fault.exe
The second binary must be built with MVD_TEST_FAULTS. Every process is owned
by this test and uses an isolated temporary data directory; no service is used.
Expected rows and complete protocol transcripts come from Python fixtures,
never from another query/binary. Small integer vectors have exact text/value
expectations, and every selected field and response marker is checked.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
import time
from dataclasses import dataclass, replace
from pathlib import Path


FAULT_ENV = "MVD_TEST_FAIL_COMMIT_PUBLICATION"
WAL_FAULT_ENV = "MVD_TEST_FAIL_WAL_APPEND"
MARKER = "__COMMIT_PUBLICATION_END__"
STARTUP = "__COMMIT_PUBLICATION_READY__"
TABLES = ("left_rows", "right_rows")
COLUMNS = ("id", "score", "tag", "body", "emb")
TIMEOUT = 8


@dataclass(frozen=True)
class Row:
    id: int
    score: int
    tag: str
    body: str
    emb: tuple[int, ...]


@dataclass(frozen=True)
class Statement:
    sql: str
    expected: tuple[str, ...] = ("OK",)


def vector_text(values):
    return "[" + ",".join(map(str, values)) + "]"


def literal(value):
    if isinstance(value, tuple):
        value = vector_text(value)
    if isinstance(value, str):
        return "'" + value.replace(chr(92), chr(92) * 2).replace("'", "''") + "'"
    return str(value)


def body_for(table, key, phase, size=4096):
    prefix = f"{table}:{key}:{phase}|'quoted'\\payload;"
    suffix = f"|end:{table}:{key}:{phase}"
    return prefix + "x" * (size - len(prefix) - len(suffix)) + suffix


def fixtures():
    before, after = {}, {}
    for index, table in enumerate(TABLES):
        stable = Row(-5, -(1 << 63) if index == 0 else (1 << 63) - 1,
                     f"stable-{index}", body_for(table, -5, "stable"),
                     (index, -1, 2, -3))
        before[table] = (
            stable,
            Row(1, 10 + index, f"old-{index}", body_for(table, 1, "old"),
                (index + 1, 2, 3, 4)),
            Row(2, 20 + index, f"delete-{index}", body_for(table, 2, "delete"),
                (index + 2, 3, 4, 5)),
        )
        after[table] = (
            stable,
            Row(1, -100 - index, f"updated-{index}",
                body_for(table, 1, "committed update", 8192), (9, index, -7, 6)),
            Row(3, 300 + index, f"inserted-{index}",
                body_for(table, 3, "committed insert", 6144), (-9, index, 7, -6)),
        )
    return before, after


def row_text(row):
    # This fixture uses only small integers in vectors, so canonical protocol
    # text is known independently and needs neither numeric tolerance nor parsing.
    return "\t".join(vector_text(getattr(row, name)) if name == "emb"
                     else str(getattr(row, name)) for name in COLUMNS)


def select(table, rows):
    return Statement(f"SELECT id,score,tag,body,emb FROM {table} ORDER BY id",
                     tuple(row_text(row) for row in sorted(rows, key=lambda row: row.id)))


def selects(state):
    return [select(table, state[table]) for table in TABLES]


def insert(table, rows):
    values = ",".join(
        "(" + ",".join(literal(getattr(row, name)) for name in COLUMNS) + ")"
        for row in rows
    )
    return Statement(f"INSERT INTO {table} VALUES {values}", (f"OK {len(rows)}",))


def update(table, row):
    assignments = ",".join(
        f"{name}={literal(getattr(row, name))}" for name in COLUMNS if name != "id"
    )
    return Statement(f"UPDATE {table} SET {assignments} WHERE id={row.id}", ("OK 1",))


def setup(before):
    steps = [Statement(f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,score BIGINT,"
                       "tag VARCHAR(32),body VARCHAR(8192),emb VECTOR(4))")
             for table in TABLES]
    # Separate autocommits must succeed even with the fault environment present:
    # the hook is restricted to transactions that write more than one table.
    steps += [insert(table, before[table]) for table in TABLES]
    return steps + selects(before)


def mutations(after):
    steps = []
    for table in TABLES:
        updated, inserted = after[table][1:]
        intermediate = replace(inserted, score=-999, tag="temporary",
                               body=body_for(table, 3, "before overwrite"), emb=(0, 0, 0, 0))
        steps += [
            update(table, updated),
            Statement(f"DELETE FROM {table} WHERE id=2", ("OK 1",)),
            insert(table, (intermediate,)),
            update(table, inserted),
        ]
    return steps


def expected_stdout(steps):
    return STARTUP + "\n" + "".join(
        "".join(line + "\n" for line in step.expected) + MARKER + "\n"
        for step in steps
    )


def check_stdout(actual, expected, label):
    if actual == expected:
        return
    position = next((index for index, (a, b) in enumerate(zip(actual, expected))
                     if a != b), min(len(actual), len(expected)))
    raise AssertionError(
        f"{label}: stdout differs at character {position}; "
        f"length {len(actual)} != {len(expected)}\n"
        f"actual:   {actual[max(0, position - 80):position + 160]!r}\n"
        f"expected: {expected[max(0, position - 80):position + 160]!r}"
    )


def run_process(executable, data_dir, steps, label, inject=False, expected_steps=None,
                wal_fault=False):
    environment = os.environ.copy()
    # Explicitly remove inherited injection state for normal/recovery runs.
    environment.pop(FAULT_ENV, None)
    environment.pop(WAL_FAULT_ENV, None)
    if inject:
        environment[FAULT_ENV] = "1"
    if wal_fault:
        environment[WAL_FAULT_ENV] = "1"
    sql = "".join(step.sql.rstrip().rstrip(";") + ";\n" for step in steps)
    try:
        result = subprocess.run(
            [str(executable), "--stdio=text", f"--data-dir={data_dir}",
             f"--ready-marker={MARKER}", f"--startup-marker={STARTUP}"],
            input=sql, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="strict", env=environment,
            timeout=TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills and reaps its own child on timeout. A timeout
        # is never accepted as the required fail-stop nonzero process exit.
        raise AssertionError(f"{label}: child did not exit within {TIMEOUT}s") from error
    fatal = expected_steps is not None
    if (fatal and result.returncode == 0) or (not fatal and result.returncode != 0):
        raise AssertionError(
            f"{label}: expected {'nonzero' if fatal else 'zero'} exit, "
            f"got {result.returncode}; stderr={result.stderr[-1200:]!r}"
        )
    if fatal and "restart required" not in result.stderr:
        raise AssertionError(f"{label}: missing fatal recovery diagnostic: {result.stderr!r}")
    # In the injected case the entire transcript must end BEFORE COMMIT's
    # response: no OK, no ready marker, no recoverable protocol error, and no
    # subsequent SELECT output from either a half-published or complete state.
    wanted = expected_steps if fatal else steps
    try:
        check_stdout(result.stdout, expected_stdout(wanted), label)
    except AssertionError as error:
        raise AssertionError(f"{error}\nstderr={result.stderr[-1200:]!r}") from None
    return result.returncode


def run_case(root, normal, writer, label, outcome, inject):
    data_dir = root / label
    before, after = fixtures()
    prefix = setup(before) + [Statement("BEGIN")] + mutations(after) + selects(after)
    if outcome in ("fatal", "torn-wal"):
        # Send COMMIT and both later SELECTs together, not after observing the
        # failure. This catches implementations that print an error and continue.
        steps = prefix + [Statement("COMMIT")] + selects(after)
        code = run_process(writer, data_dir, steps, label, inject=outcome == "fatal",
                           expected_steps=prefix, wal_fault=outcome == "torn-wal")
        durable = before if outcome == "torn-wal" else after
    else:
        finish = "ROLLBACK" if outcome == "rollback" else "COMMIT"
        durable = before if outcome == "rollback" else after
        steps = prefix + [Statement(finish)] + selects(durable)
        # Empty COMMIT/ROLLBACK must not publish old overlay rows or activate
        # the multi-writer hook, including immediately after a real rollback.
        steps += [Statement("COMMIT"), Statement("BEGIN"), Statement("ROLLBACK")]
        steps += selects(durable)
        code = run_process(writer, data_dir, steps, label, inject=inject)
    for restart in (1, 2):
        run_process(normal, data_dir, selects(durable), f"{label} restart={restart}")
    print(f"PASS {label}: writer exit={code}; both tables' complete fields verified "
          "through two normal-binary restarts", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("normal", type=Path)
    parser.add_argument("fault", type=Path)
    args = parser.parse_args()
    normal, fault = args.normal.resolve(), args.fault.resolve()
    for label, executable in (("normal", normal), ("fault", fault)):
        if not executable.is_file():
            parser.error(f"{label} executable does not exist: {executable}")
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="mvd-commit-publication-") as temporary:
        root = Path(temporary)
        run_case(root, normal, fault, "injected-publication-failure", "fatal", True)
        run_case(root, normal, fault, "fault-build-without-env", "commit", False)
        run_case(root, normal, fault, "rollback-with-fault-env", "rollback", True)
        run_case(root, normal, normal, "normal-build-ignores-fault-env", "commit", True)
        run_case(root, normal, fault, "torn-wal-failure", "torn-wal", False)
        before, _ = fixtures()
        data_dir = root / "noop-mutations"
        run_process(normal, data_dir, setup(before), "noop setup")
        wal_size = (data_dir / "mvd.wal").stat().st_size
        steps = []
        for table in TABLES:
            steps += [Statement(f"UPDATE {table} SET score=7 WHERE id=999", ("OK 0",)),
                      Statement(f"DELETE FROM {table} WHERE id=999", ("OK 0",))]
        steps = steps + [Statement("BEGIN")] + steps + [Statement("COMMIT")]
        run_process(fault, data_dir, steps + selects(before), "noop mutations",
                    inject=True, wal_fault=True)
        if (data_dir / "mvd.wal").stat().st_size != wal_size:
            raise AssertionError("no-op mutations must not append WAL records")
        for restart in (1, 2):
            run_process(normal, data_dir, selects(before), f"noop restart={restart}")
        print("PASS noop mutations: no WAL growth, no fault activation, two restarts", flush=True)
    print(f"PASS commit publication: 6 cases, 19 child processes, "
          f"{time.monotonic() - started:.3f}s total", flush=True)


if __name__ == "__main__":
    main()
