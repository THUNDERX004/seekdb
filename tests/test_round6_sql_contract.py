#!/usr/bin/env python3
"""Focused parser contract checks against an existing MiniVectorDB executable.

Run: python -X utf8 -B tests/test_round6_sql_contract.py build/minivectordb.exe

Each test uses a fresh temporary database. No service, build, or network access
is needed. The task requires VARCHAR(n), BEGIN/COMMIT/ROLLBACK, rejection of
unsupported statements, and autocommit outside explicit transactions. Error
codes are not pinned because mysqltest/README.md leaves their mapping unfrozen.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ERROR = object()
MARKER = "__MVD_ROUND6_SQL_CONTRACT_END__"


class SqlContract(unittest.TestCase):
    executable: Path | None = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run this script with an existing executable")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-round6-sql-")
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)

    def check_sql(self, steps, *, data_dir=None):
        process = subprocess.run(
            [str(self.executable), "--stdio=text", f"--data-dir={data_dir or self.data_dir}",
             "--column-header", f"--ready-marker={MARKER}"],
            input="".join(sql + ";\n" for sql, _ in steps),
            encoding="utf-8", errors="strict", capture_output=True,
            timeout=10, check=False,
        )
        self.assertEqual(process.returncode, 0, msg=process.stderr)
        frames = process.stdout.split(MARKER + "\n")
        self.assertEqual(len(frames), len(steps) + 1, msg=repr(process.stdout))
        self.assertEqual(frames.pop(), "", msg=repr(process.stdout))
        failures = []
        for (sql, expected), frame in zip(steps, frames):
            actual = frame.removesuffix("\n").split("\n")
            if expected is ERROR:
                matches = len(actual) == 1 and re.fullmatch(
                    r"ERROR [^\s:]+:.*", actual[0]
                )
                wanted = "one ERROR with a nonempty code"
            else:
                matches = actual == expected
                wanted = repr(expected)
            if not matches:
                failures.append(
                    f"SQL: {sql}\nexpected: {wanted}\nactual: {actual!r}"
                )
        if failures:
            self.fail("\n\n".join(failures))

    def test_invalid_transaction_words_do_not_disable_autocommit(self):
        for index, invalid in enumerate((
            "START",
            "START WORK",
            "BEGIN WORK WORK",
            "BEGIN TRANSACTION TRANSACTION",
            "BEGIN WORK TRANSACTION",
            "START TRANSACTION TRANSACTION",
        )):
            # A separate data directory also makes the restart assertion local.
            data_dir = self.data_dir / str(index)
            with self.subTest(sql=invalid):
                errors = []
                for steps in (
                    [
                        ("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT)", ["OK"]),
                        (invalid, ERROR),
                        ("INSERT INTO t VALUES (1,10)", ["OK 1"]),
                        ("ROLLBACK", ["OK"]),
                        ("SELECT id,n FROM t ORDER BY id",
                         ["COLUMNS\tid\tn", "1\t10"]),
                    ],
                    [
                        ("SELECT id,n FROM t ORDER BY id",
                         ["COLUMNS\tid\tn", "1\t10"]),
                    ],
                ):
                    try:
                        self.check_sql(steps, data_dir=data_dir)
                    except AssertionError as error:
                        errors.append(str(error))
                if errors:
                    self.fail("\n\n".join(errors))

    def test_varchar_requires_length_before_creating_catalog_entry(self):
        errors = []
        for steps in (
            [
                ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR)", ERROR),
                ("INSERT INTO t VALUES (1,'x')", ERROR),
                ("SELECT id FROM t", ERROR),
            ],
            [
                ("SELECT id FROM t", ERROR),
                ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1))", ["OK"]),
                ("INSERT INTO t VALUES (2,'y')", ["OK 1"]),
                ("SELECT id,s FROM t ORDER BY id",
                 ["COLUMNS\tid\ts", "2\ty"]),
            ],
        ):
            try:
                self.check_sql(steps)
            except AssertionError as error:
                errors.append(str(error))
        if errors:
            self.fail("\n\n".join(errors))

    def test_supported_transaction_controls_and_invalid_tails(self):
        committed = ["COLUMNS\tid\tn", "1\t11", "2\t20"]
        self.check_sql([
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT)", ["OK"]),
            ("INSERT INTO t VALUES (1,10)", ["OK 1"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET n=11 WHERE id=1", ["OK 1"]),
            ("COMMIT WORK WORK", ERROR),
            ("ROLLBACK WORK WORK", ERROR),
            ("INSERT INTO t VALUES (2,20)", ["OK 1"]),
            ("COMMIT", ["OK"]),
            ("BEGIN invalid", ERROR),
            ("BEGIN", ["OK"]),
            ("DELETE FROM t WHERE id>=1", ["OK 2"]),
            ("ROLLBACK", ["OK"]),
            ("SELECT id,n FROM t ORDER BY id", committed),
        ])
        self.check_sql([("SELECT id,n FROM t ORDER BY id", committed)])

    def test_valid_transaction_variants_keep_commit_and_rollback_semantics(self):
        self.check_sql([
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT)", ["OK"]),
            ("BEGIN WORK", ["OK"]),
            ("INSERT INTO t VALUES (1,10)", ["OK 1"]),
            ("COMMIT WORK", ["OK"]),
            ("START TRANSACTION", ["OK"]),
            ("UPDATE t SET n=20 WHERE id=1", ["OK 1"]),
            ("ROLLBACK WORK", ["OK"]),
            ("BEGIN TRANSACTION", ["OK"]),
            ("UPDATE t SET n=30 WHERE id=1", ["OK 1"]),
            ("END WORK", ["OK"]),
            ("SELECT id,n FROM t ORDER BY id", ["COLUMNS\tid\tn", "1\t30"]),
        ])
        self.check_sql([
            ("SELECT id,n FROM t ORDER BY id", ["COLUMNS\tid\tn", "1\t30"]),
        ])

    def test_supported_unicode_length_and_finite_float32_literals(self):
        rows = ["COLUMNS\tid\ts\tv", "1\t\U0001f600\t[16777216]"]
        self.check_sql([
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1),v VECTOR(1))",
             ["OK"]),
            ("INSERT INTO t VALUES (1,'\U0001f600','[16777217]')", ["OK 1"]),
            ("INSERT INTO t VALUES (2,'x','[0]'),(3,'e\u0301','[0]')", ERROR),
            ("UPDATE t SET v='[3.5e38]' WHERE id=1", ERROR),
            ("SELECT id,s,v FROM t ORDER BY id", rows),
            ("SELECT id FROM t WHERE s>='\ue000' AND s<'\U0010ffff'",
             ["COLUMNS\tid", "1"]),
        ])
        self.check_sql([("SELECT id,s,v FROM t ORDER BY id", rows)])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--case", help="substring selecting focused test methods")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"database executable does not exist: {executable}")
    SqlContract.executable = executable
    names = unittest.defaultTestLoader.getTestCaseNames(SqlContract)
    if args.case:
        names = [name for name in names if args.case in name]
        if not names:
            parser.error("--case did not select any tests")
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite(SqlContract(name) for name in names)
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
