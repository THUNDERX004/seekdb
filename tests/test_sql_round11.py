#!/usr/bin/env python3
"""Focused SQL name-resolution regressions for the round-11 review.

Run: python -X utf8 -B tests/test_sql_round11.py build/minivectordb.exe

Use the existing bounded Python session harness and a fresh temporary database
per test. Expected results are explicit SQL-contract assertions, independent of
the database parser, sorting implementation, and official mysqltest fixtures.
These tests reproduce name-resolution defects in the reviewed a4bc7ff build.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import tempfile
import unittest

from test_differential import TimedSession


class SqlRound11(unittest.TestCase):
    executable: Path | None = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("provide the database executable")
        directory = tempfile.TemporaryDirectory(prefix="mvd-sql-round11-")
        self.addCleanup(directory.cleanup)
        self.session = TimedSession(self.executable, Path(directory.name), 8)
        self.addCleanup(self.session.shutdown)

    def expect(self, sql: str, expected: list[str]):
        self.assertEqual(self.session.execute(sql), expected, sql)

    def expect_error(self, sql: str):
        actual = self.session.execute(sql)
        # mysqltest/README.md does not freeze symbolic/numeric error mappings.
        self.assertTrue(
            len(actual) == 1
            and re.fullmatch(r"ERROR [^\s:]+:.*", actual[0]) is not None,
            f"SQL: {sql}\nexpected: one ERROR with a nonempty code\nactual: {actual!r}",
        )

    def create_vectors(self):
        self.expect(
            "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(1))", ["OK"]
        )
        self.expect("INSERT INTO t VALUES(1,9,'[0]'),(2,2,'[2]')", ["OK 2"])

    def test_function_name_is_a_valid_order_by_alias(self):
        self.create_vectors()
        self.expect("SELECT id,n AS d FROM t ORDER BY d,id", ["2\t2", "1\t9"])
        self.expect(
            "SELECT id,L2_DISTANCE(v,'[0]') AS d FROM t ORDER BY d LIMIT 1",
            ["1\t0.000000"],
        )
        cases = (
            ("SELECT id,n AS l2_distance FROM t ORDER BY l2_distance,id",
             ["2\t2", "1\t9"]),
            ("SELECT id,n AS L2_DISTANCE FROM t ORDER BY l2_distance DESC,id LIMIT 1",
             ["1\t9"]),
            ("SELECT id,L2_DISTANCE(v,'[0]') AS l2_distance "
             "FROM t ORDER BY l2_distance LIMIT 1", ["1\t0.000000"]),
        )
        for sql, expected in cases:
            with self.subTest(sql=sql):
                self.expect(sql, expected)

    def test_function_name_is_a_valid_ordinary_column(self):
        self.expect(
            "CREATE TABLE t(id BIGINT PRIMARY KEY,l2_distance BIGINT)", ["OK"]
        )
        self.expect("INSERT INTO t VALUES(1,9),(2,2)", ["OK 2"])
        self.expect("SELECT * FROM t ORDER BY id", ["1\t9", "2\t2"])
        cases = (
            ("SELECT l2_distance FROM t ORDER BY id", ["9", "2"]),
            ("SELECT id FROM t ORDER BY l2_distance,id", ["2", "1"]),
        )
        for sql, expected in cases:
            with self.subTest(sql=sql):
                self.expect(sql, expected)

    def test_unaliased_distance_does_not_enable_direct_vector_order(self):
        self.create_vectors()
        self.expect(
            "SELECT id,L2_DISTANCE(v,'[0]') FROM t ORDER BY id",
            ["1\t0.000000", "2\t2.000000"],
        )
        self.expect_error("SELECT id FROM t ORDER BY v")
        self.expect_error(
            "SELECT id,L2_DISTANCE(v,'[0]') AS d FROM t ORDER BY v"
        )
        for suffix in ("", " DESC,n LIMIT 1", " LIMIT 0"):
            sql = "SELECT id,L2_DISTANCE(v,'[0]') FROM t ORDER BY v" + suffix
            with self.subTest(sql=sql):
                self.expect_error(sql)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    SqlRound11.executable = args.executable.resolve(strict=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SqlRound11)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
