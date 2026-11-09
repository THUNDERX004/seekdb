#!/usr/bin/env python3
"""Fail-stop and recovery tests for partial/durable catalog DDL publication.

Usage: python tests/test_catalog_publication.py normal.exe fault.exe
The second binary must enable MVD_TEST_FAULTS and both catalog failure hooks.
Only subprocesses owned by this test and fresh temporary directories are used.
Protocol transcripts, rows, catalog records and CRCs have independent Python
oracles; an implementation's successful output is never used as the answer.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


APPEND_FAULT = "MVD_TEST_FAIL_CATALOG_APPEND"
PUBLICATION_FAULT = "MVD_TEST_FAIL_CATALOG_PUBLICATION"
FAULTS = (APPEND_FAULT, PUBLICATION_FAULT)
MARKER = "__CATALOG_PUBLICATION_END__"
STARTUP = "__CATALOG_PUBLICATION_READY__"
TIMEOUT = 8
SCHEMA = "(id BIGINT PRIMARY KEY,s VARCHAR(64),v VECTOR(2))"
BEFORE = ("1\tbase-one\t[1,2]", "2\tstable-two\t[-3,4]")
PENDING = ("1\tpending-before-ddl\t[7,-8]", BEFORE[1])
AFTER = (PENDING[0], "2\texecuted-after-ddl\t[8,9]",
         "99\tinserted-after-ddl\t[9,10]")
AFTER_TABLE = ("7\tafter-created\t[2,3]",)


@dataclass(frozen=True)
class Statement:
    sql: str
    expected: tuple[str, ...] = ("OK",)


def select(table, rows):
    return Statement(f"SELECT id,s,v FROM {table} ORDER BY id", rows)


def missing_table(table):
    return select(table, (f"ERROR UNKNOWN_TABLE: unknown table '{table}'",))


def catalog_record(name, oid, indexed=False):
    """Encode this fixture's fixed schema, independently of the executable.

    The v1 little-endian format has a 24-byte header and CRC32 over the
    payload. Column tags are BIGINT=1, VARCHAR=2, VECTOR=3. Checking complete
    bytes also checks record boundaries, schema, OID, and final vector_index.
    """
    def string(value):
        encoded = value.encode("utf-8")
        return struct.pack("<I", len(encoded)) + encoded

    payload = struct.pack("<I", oid) + string(name) + struct.pack("<I", 3)
    for kind, column, length in ((1, "id", 0), (2, "s", 64), (3, "v", 2)):
        payload += struct.pack("<B", kind) + string(column)
        payload += struct.pack("<I", length)
    payload += struct.pack("<IIB", 0, 2, int(indexed))
    header = struct.pack("<IHHQII", 0x4341564D, 1, 0, len(payload),
                         zlib.crc32(payload) & 0xFFFFFFFF, 0)
    return header + payload


def check_equal(actual, expected, label):
    if actual == expected:
        return
    position = next((index for index, (a, b) in enumerate(zip(actual, expected))
                     if a != b), min(len(actual), len(expected)))
    raise AssertionError(
        f"{label}: differs at offset {position}; "
        f"actual length={len(actual)}, expected length={len(expected)}\n"
        f"actual:   {actual[max(0, position - 60):position + 120]!r}\n"
        f"expected: {expected[max(0, position - 60):position + 120]!r}"
    )


def expected_stdout(steps):
    return STARTUP + "\n" + "".join(
        "".join(line + "\n" for line in step.expected) + MARKER + "\n"
        for step in steps
    )


def run_process(executable, data_dir, steps, label, fault_env=None,
                stopped_before=None):
    environment = os.environ.copy()
    # Do not inherit unrelated fault injections from another test/task.
    for name in tuple(environment):
        if name.upper().startswith("MVD_TEST_"):
            environment.pop(name)
    if fault_env is not None:
        if fault_env not in FAULTS:
            raise ValueError(f"unknown catalog hook: {fault_env}")
        environment[fault_env] = "1"
    sql = "".join(step.sql.rstrip().rstrip(";") + ";\n" for step in steps)
    data_dir.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            [str(executable), f"--data-dir={data_dir}",
             f"--ready-marker={MARKER}", f"--startup-marker={STARTUP}"],
            cwd=data_dir, input=sql, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8",
            errors="strict", env=environment, timeout=TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills/reaps its child. A hang is not fail-stop success.
        raise AssertionError(f"{label}: child did not exit within {TIMEOUT}s") from error
    fatal = stopped_before is not None
    if (fatal and result.returncode == 0) or (not fatal and result.returncode != 0):
        raise AssertionError(
            f"{label}: expected {'nonzero' if fatal else 'zero'} exit, "
            f"got {result.returncode}; stderr={result.stderr[-1200:]!r}; "
            f"stdout={result.stdout[-1600:]!r}"
        )
    if fatal and "restart required" not in result.stderr:
        raise AssertionError(f"{label}: missing fatal diagnostic: {result.stderr!r}")
    if not fatal and "restart required" in result.stderr:
        raise AssertionError(f"{label}: unexpected fatal diagnostic: {result.stderr!r}")
    # In fatal cases no response for the target DDL is allowed: neither OK,
    # recoverable ERROR, nor ready marker. Later SQL is already in the pipe.
    wanted = stopped_before if fatal else steps
    try:
        check_equal(result.stdout, expected_stdout(wanted), f"{label} stdout")
    except AssertionError as error:
        raise AssertionError(f"{error}\nstderr={result.stderr[-1200:]!r}") from None
    return result.returncode


def run_case(root, normal, writer, ddl, label, fault_env=None, fatal=False):
    data_dir = root / label
    setup = [
        Statement("CREATE TABLE t" + SCHEMA),
        Statement("INSERT INTO t VALUES(1,'base-one','[1,2]'),"
                  "(2,'stable-two','[-3,4]')", ("OK 2",)),
        select("t", BEFORE),
    ]
    # Seed through a separate normal process; the fault writer's first DDL
    # must be the target, not an earlier setup CREATE TABLE.
    run_process(normal, data_dir, setup, f"{label} seed")
    baseline = catalog_record("t", 1)
    catalog_path = data_dir / "catalog.bin"
    check_equal(catalog_path.read_bytes(), baseline, f"{label} seed catalog")
    wal_before = (data_dir / "mvd.wal").read_bytes()

    prefix = [
        Statement("BEGIN"),
        Statement("UPDATE t SET s='pending-before-ddl',v='[7,-8]' WHERE id=1",
                  ("OK 1",)),
        select("t", PENDING),
    ]
    if ddl == "table":
        target = Statement("CREATE TABLE created" + SCHEMA)
        record = catalog_record("created", 2)
    elif ddl == "index":
        target = Statement("CREATE VECTOR INDEX ix ON t(v)")
        record = catalog_record("t", 1, indexed=True)
    else:
        raise ValueError(f"unknown DDL: {ddl}")
    suffix = [
        Statement("UPDATE t SET s='executed-after-ddl',v='[8,9]' WHERE id=2",
                  ("OK 1",)),
        Statement("INSERT INTO t VALUES(99,'inserted-after-ddl','[9,10]')",
                  ("OK 1",)),
        Statement("COMMIT"),
        Statement("CREATE TABLE after_ddl" + SCHEMA),
        Statement("INSERT INTO after_ddl VALUES(7,'after-created','[2,3]')",
                  ("OK 1",)),
        select("t", AFTER),
        select("after_ddl", AFTER_TABLE),
    ]
    if ddl == "table":
        suffix.append(select("created", ()))
    # One complete stdin batch includes prefix, failing target and suffix.
    # The suffix's COMMIT would persist the preceding UPDATE if executed.
    code = run_process(writer, data_dir, prefix + [target] + suffix, label,
                       fault_env=fault_env, stopped_before=prefix if fatal else None)
    torn = fatal and fault_env == APPEND_FAULT
    if fatal:
        if fault_env not in FAULTS:
            raise ValueError("fatal case requires a catalog hook")
        written = record[:len(record) // 2] if torn else record
        check_equal(catalog_path.read_bytes(), baseline + written,
                    f"{label} {'half-record' if torn else 'complete-record'} catalog")
        # No implicit commit of the pre-DDL UPDATE, nor later COMMIT, is allowed.
        check_equal((data_dir / "mvd.wal").read_bytes(), wal_before,
                    f"{label} WAL must not publish pending or later SQL")
        durable_catalog = baseline if torn else baseline + record
        rows = BEFORE
    else:
        after_oid = 3 if ddl == "table" else 2
        durable_catalog = baseline + record + catalog_record("after_ddl", after_oid)
        check_equal(catalog_path.read_bytes(), durable_catalog, f"{label} catalog")
        rows = AFTER

    recovery = [select("t", rows)]
    if ddl == "table":
        # Empty SELECT still returns a response marker; UNKNOWN_TABLE must
        # instead be an exact error. The distinction is not inferred from rows.
        recovery.append(missing_table("created") if torn else select("created", ()))
    recovery.append(missing_table("after_ddl") if fatal
                    else select("after_ddl", AFTER_TABLE))
    for restart in (1, 2):
        run_process(normal, data_dir, recovery, f"{label} restart={restart}")
        # Recovery must truncate a torn suffix, retain a complete record, and
        # preserve t's last vector_index bit and every other catalog byte.
        check_equal(catalog_path.read_bytes(), durable_catalog,
                    f"{label} restart={restart} catalog")
        if fatal:
            check_equal((data_dir / "mvd.wal").read_bytes(), wal_before,
                        f"{label} restart={restart} WAL")
    print(f"PASS {label}: writer exit={code}; complete transcript, catalog, "
          "and rows checked through two normal restarts", flush=True)


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
    with tempfile.TemporaryDirectory(prefix="mvd-catalog-publication-") as temporary:
        root = Path(temporary)
        for ddl in ("table", "index"):
            for hook, phase in ((APPEND_FAULT, "torn"),
                                (PUBLICATION_FAULT, "full")):
                run_case(root, normal, fault, ddl, f"{ddl}-{phase}-fail-stop",
                         fault_env=hook, fatal=True)
            run_case(root, normal, normal, ddl, f"{ddl}-normal-without-env")
            run_case(root, normal, fault, ddl, f"{ddl}-fault-build-without-env")
            for hook, phase in ((APPEND_FAULT, "torn"),
                                (PUBLICATION_FAULT, "full")):
                run_case(root, normal, normal, ddl, f"{ddl}-normal-ignores-{phase}",
                         fault_env=hook)
    print("PASS catalog publication: 12 cases, 48 child processes, "
          f"{time.monotonic() - started:.3f}s total", flush=True)


if __name__ == "__main__":
    main()
