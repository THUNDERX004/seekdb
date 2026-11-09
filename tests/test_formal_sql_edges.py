#!/usr/bin/env python3
"""Small, independent SQL contract regressions for the existing executable.

Run: python -X utf8 -B tests/test_formal_sql_edges.py build/minivectordb.exe

Each test owns an empty temporary database. Input is SQL and output uses the
executable's column-header/ready-marker options; there is no JSON transport.
Error codes are deliberately not pinned because mysqltest/README.md says their
mapping is not frozen. Unknown identifiers must fail, rather than become text.
The identifier regressions fail on baseline 3c1adfbf3 and pass after its fix.

Unfrozen details (additional MySQL backslash escapes, identifier reservation,
implicit casts, ambiguous aliases, and UPDATE no-op counts) are not oracles here.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ERROR = None
MARKER = "__FORMAL_SQL_EDGES_END__"


class FormalSqlEdges(unittest.TestCase):
    executable: Path | None = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run this script with the executable argument")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-formal-sql-edges-")
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)

    def check_sql(self, *steps):
        """Run every statement, then report all mismatches, including side effects."""
        process = subprocess.run(
            [str(self.executable), f"--data-dir={self.data_dir}",
             "--column-header", f"--ready-marker={MARKER}"],
            input="".join(sql + ";\n" for sql, _ in steps),
            text=True, encoding="utf-8", errors="strict", capture_output=True,
            timeout=10, check=False,
        )
        self.assertEqual(process.returncode, 0, msg=process.stderr)
        frames = process.stdout.split(MARKER + "\n")
        self.assertEqual(len(frames), len(steps) + 1, msg=repr(process.stdout))
        self.assertEqual(frames.pop(), "", msg=repr(process.stdout))
        failures = []
        for (sql, expected), frame in zip(steps, frames):
            # splitlines() would incorrectly split VARCHAR data at U+0085/U+2028.
            actual = frame.removesuffix("\n").split("\n")
            if expected is ERROR:
                matches = len(actual) == 1 and re.fullmatch(
                    r"ERROR [^\s:]+:.*", actual[0]
                )
            else:
                matches = actual == expected
            if not matches:
                wanted = "one ERROR with a nonempty code" if expected is ERROR else repr(expected)
                failures.append(f"SQL: {sql}\nexpected: {wanted}\nactual: {actual!r}")
        if failures:
            self.fail("\n\n".join(failures))

    def test_unknown_identifier_in_select_is_not_a_string(self):
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))", ["OK"]),
            ("INSERT INTO t VALUES (1,'missing'),(2,'keep')", ["OK 2"]),
            ("SELECT id FROM t WHERE s='missing' ORDER BY id", ["COLUMNS\tid", "1"]),
            ("SELECT id FROM t WHERE s=missing ORDER BY id", ERROR),
            ("SELECT id,s FROM t ORDER BY id", ["COLUMNS\tid\ts", "1\tmissing", "2\tkeep"]),
        )

    def test_unknown_identifier_in_multirow_insert_is_atomic(self):
        prior = ["COLUMNS\tid\ts", "1\tprior"]
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))", ["OK"]),
            ("INSERT INTO t VALUES (1,'base')", ["OK 1"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET s='prior' WHERE id=1", ["OK 1"]),
            ("INSERT INTO t VALUES (2,'valid'),(3,missing)", ERROR),
            ("SELECT id,s FROM t ORDER BY id", prior),
            ("COMMIT", ["OK"]),
            ("SELECT id,s FROM t ORDER BY id", prior),
        )

    def test_unknown_identifier_in_multirow_update_is_rejected(self):
        prior = ["COLUMNS\tid\ts", "1\told", "2\tprior"]
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))", ["OK"]),
            ("INSERT INTO t VALUES (1,'old'),(2,'keep')", ["OK 2"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET s='prior' WHERE id=2", ["OK 1"]),
            ("UPDATE t SET s=missing WHERE id>=1", ERROR),
            ("SELECT id,s FROM t ORDER BY id", prior),
            ("UPDATE t SET s='after' WHERE id=1", ["OK 1"]),
            ("COMMIT", ["OK"]),
            ("SELECT id,s FROM t ORDER BY id", ["COLUMNS\tid\ts", "1\tafter", "2\tprior"]),
        )

    def test_unknown_identifier_in_delete_cannot_remove_a_matching_literal(self):
        prior = ["COLUMNS\tid\ts", "1\tmissing", "2\tprior"]
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))", ["OK"]),
            ("INSERT INTO t VALUES (1,'missing'),(2,'keep')", ["OK 2"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET s='prior' WHERE id=2", ["OK 1"]),
            ("DELETE FROM t WHERE s=missing", ERROR),
            ("SELECT id,s FROM t ORDER BY id", prior),
            ("COMMIT", ["OK"]),
            ("SELECT id,s FROM t ORDER BY id", prior),
        )

    def test_malformed_numbers_cannot_become_varchar_literals(self):
        prior = ["COLUMNS\tid\ts", "1\tprior", "2\t1e+"]
        steps = []
        restart = []
        for index, token in enumerate(("1e+", "1e-", "1E", "1.2.3",
                                       "1+2", "1-2", "1e2e3")):
            table = f"t{index}"
            steps.extend([
                (f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,s VARCHAR(16))", ["OK"]),
                (f"INSERT INTO {table} VALUES (1,'base'),(2,'1e+')", ["OK 2"]),
                ("BEGIN", ["OK"]),
                (f"UPDATE {table} SET s='prior' WHERE id=1", ["OK 1"]),
                (f"INSERT INTO {table} VALUES (3,'valid'),(4,{token})", ERROR),
                (f"UPDATE {table} SET s={token} WHERE id=2", ERROR),
                (f"SELECT id FROM {table} WHERE s={token}", ERROR),
                (f"DELETE FROM {table} WHERE s={token}", ERROR),
                (f"SELECT id,s FROM {table} ORDER BY id", prior),
                ("COMMIT", ["OK"]),
                (f"SELECT id,s FROM {table} ORDER BY id", prior),
            ])
            restart.append((f"SELECT id,s FROM {table} ORDER BY id", prior))
        self.check_sql(*steps)
        self.check_sql(*restart)

    def test_unicode_length_counts_codepoints_and_failed_batch_leaves_no_rows(self):
        expected = ["COLUMNS\tid\ts", "1\t\U0001f600", "2\te", "3\t\u00e9"]
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1))", ["OK"]),
            ("INSERT INTO t VALUES (1,'\U0001f600'),(2,'e'),(3,'\u00e9')", ["OK 3"]),
            ("INSERT INTO t VALUES (4,'x'),(5,'e\u0301')", ERROR),
            ("UPDATE t SET s='e\u0301' WHERE id>=1", ERROR),
            ("SELECT id,s FROM t ORDER BY id", expected),
        )

    def test_unicode_order_uses_codepoints_and_primary_key_ties(self):
        rows = [(8, "\U00010000"), (3, "\ue000"), (7, "\U0001f600"),
                (4, "e\u0301"), (1, "\u00e9"), (9, "\u00e9"), (2, ""),
                (5, "\u0085"), (6, "\u2028"), (10, "\u2029")]
        ascending = sorted(rows, key=lambda row: (row[1], row[0]))
        # A stable second sort retains the required ascending PK order for ties.
        descending = sorted(sorted(rows), key=lambda row: row[1], reverse=True)
        steps = [
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2))", ["OK"]),
            ("INSERT INTO t VALUES " + ",".join(f"({key},'{value}')" for key, value in rows),
             [f"OK {len(rows)}"]),
        ]
        for order, ordered in (("s", ascending), ("s DESC", descending)):
            steps.append((f"SELECT id,s FROM t ORDER BY {order}",
                          ["COLUMNS\tid\ts"] + [f"{key}\t{value}" for key, value in ordered]))
        steps.extend([
            ("SELECT id FROM t WHERE s='\u00e9' ORDER BY id", ["COLUMNS\tid", "1", "9"]),
            ("SELECT id FROM t WHERE s>='\U00010000' ORDER BY s", ["COLUMNS\tid", "8", "7"]),
        ])
        self.check_sql(*steps)

    def test_quoted_apostrophe_and_delimiters_remain_string_data(self):
        text = "O'Brien;--/*literal*/"
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64))", ["OK"]),
            ("INSERT INTO t VALUES (1,'O''Brien;--/*literal*/'),(2,'')", ["OK 2"]),
            ("SELECT id,s FROM t ORDER BY id", ["COLUMNS\tid\ts", f"1\t{text}", "2\t"]),
            ("SELECT id FROM t WHERE s='O''Brien;--/*literal*/'", ["COLUMNS\tid", "1"]),
        )

    def test_projection_metadata_aliases_and_unprojected_order_keys(self):
        self.check_sql(
            ("CREATE TABLE t(s VARCHAR(1),id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(2))", ["OK"]),
            ("INSERT INTO t VALUES ('z',3,5,'[3,0]'),('a',2,5,'[0,0]'),"
             "('a',1,7,'[0,1]'),('a',4,5,'[0,-1]')", ["OK 4"]),
            ("SELECT s AS tag,id AS pk,s AS again FROM t ORDER BY n DESC,s LIMIT 3",
             ["COLUMNS\ttag\tpk\tagain", "a\t1\ta", "a\t2\ta", "a\t4\ta"]),
            ("SELECT * FROM t ORDER BY n LIMIT 3", ["COLUMNS\ts\tid\tn\tv",
             "a\t2\t5\t[0,0]", "z\t3\t5\t[3,0]", "a\t4\t5\t[0,-1]"]),
            ("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 3",
             ["COLUMNS\tid\td", "2\t0.000000", "1\t1.000000", "4\t1.000000"]),
        )

    def test_late_invalid_literals_preserve_prior_work_and_allow_commit(self):
        prior = ["COLUMNS\tid\ts\tv", "1\t\U0001f600\t[0]", "2\tp\t[1]"]
        self.check_sql(
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1),v VECTOR(1))", ["OK"]),
            ("INSERT INTO t VALUES (1,'b','[0]')", ["OK 1"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET s='\U0001f600' WHERE id=1", ["OK 1"]),
            ("INSERT INTO t VALUES (2,'p','[1]')", ["OK 1"]),
            ("INSERT INTO t VALUES (3,'x','[0]'),(4,'e\u0301','[0]')", ERROR),
            ("INSERT INTO t VALUES (3,'x','[0]'),(4,'y','[NaN]')", ERROR),
            ("INSERT INTO t VALUES (3,'x','[0]'),(4,'y','[3.5e38]')", ERROR),
            ("INSERT INTO t VALUES (3,'x','[0]'),(4,'y','[0,0]')", ERROR),
            ("UPDATE t SET s='x',v='[Infinity]' WHERE id>=1", ERROR),
            ("SELECT * FROM t ORDER BY id", prior),
            ("COMMIT", ["OK"]),
        )
        self.check_sql(("SELECT * FROM t ORDER BY id", prior))  # Clean restart.

    def test_multirow_changes_see_prior_values_and_rollback_together(self):
        original = ["COLUMNS\tid\ts\tn", "1\ta\t0", "2\ta\t0", "3\tz\t9"]
        self.check_sql(
            ("CREATE TABLE t(s VARCHAR(1),id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(1))", ["OK"]),
            ("INSERT INTO t VALUES ('a',1,0,'[2]'),('a',2,0,'[1]'),('z',3,9,'[0]')", ["OK 3"]),
            ("BEGIN", ["OK"]),
            ("UPDATE t SET s='b',n=7,v='[0]' WHERE s='a' AND n=0", ["OK 2"]),
            ("UPDATE t SET n=8,s='too long' WHERE id>=1", ERROR),
            ("UPDATE t SET n=8,v='[0,0]' WHERE id>=1", ERROR),
            ("UPDATE t SET n=8,id=4 WHERE id>=1", ERROR),
            ("DELETE FROM t WHERE id>=1 OR id=0", ERROR),
            ("SELECT id,s,n FROM t ORDER BY n DESC", ["COLUMNS\tid\ts\tn",
             "3\tz\t9", "1\tb\t7", "2\tb\t7"]),
            ("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0]') LIMIT 3",
             ["COLUMNS\tid", "1", "2", "3"]),
            ("DELETE FROM t WHERE s='b' AND id>=2", ["OK 1"]),
            ("SELECT id,s,n FROM t ORDER BY id", ["COLUMNS\tid\ts\tn", "1\tb\t7", "3\tz\t9"]),
            ("ROLLBACK", ["OK"]),
            ("SELECT id,s,n FROM t ORDER BY id", original),
        )
        self.check_sql(("SELECT id,s,n FROM t ORDER BY id", original))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"database executable does not exist: {executable}")
    FormalSqlEdges.executable = executable
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FormalSqlEdges)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
