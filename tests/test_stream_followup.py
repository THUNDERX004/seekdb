#!/usr/bin/env python3
"""Local INSERT arity, statement atomicity, and subprocess restart regressions."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import tempfile
import unittest

from test_stdio_contract import PipeSession


ERROR = object()


class StreamFollowup(unittest.TestCase):
    executable = None
    tmp_root = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run with an explicit executable")
        directory = tempfile.TemporaryDirectory(
            prefix=".mvd-stream-followup-", dir=self.tmp_root,
        )
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve(strict=True)

    def pipe(self, directory):
        session = PipeSession(self.executable, directory)
        self.addCleanup(session.close)
        return session

    def exchange(self, session, steps, *, phase):
        # Read every frame before checking it, with stdin still open. Subtests
        # keep a wrong response from hiding later COMMIT/restart assertions.
        session.send("".join(sql for sql, _ in steps))
        frames = [session.response() for _ in steps]
        for index, ((sql, expected), frame) in enumerate(zip(steps, frames)):
            with self.subTest(phase=phase, step=index, sql=sql):
                evidence = f"{phase}: {sql}; frames={frames!r}"
                if expected is ERROR:
                    self.assertEqual(len(frame), 1, evidence)
                    self.assertRegex(frame[0], r"^ERROR [^\s:]+: .+", evidence)
                else:
                    self.assertEqual(frame, expected, evidence)

    def stop(self, session):
        # Close the input and wait for this process to exit normally before a
        # fresh Popen opens the same directory. This is a real process restart.
        session.process.stdin.close()
        returncode = session.process.wait(timeout=10)
        session.reader.join(timeout=5)
        self.assertFalse(session.reader.is_alive(), "stdout reader did not exit")
        stderr = session.process.stderr.read().decode("utf-8", errors="replace")
        self.assertEqual(returncode, 0, stderr)
        session.close()

    def rejected_then_restart(self, name, create, seed, invalid, select, rows):
        directory = self.directory / name
        session = self.pipe(directory)
        self.exchange(session, (
            (create, ["OK"]),
            ("BEGIN;", ["OK"]),
            (seed, ["OK 1"]),
            (invalid, ERROR),
            (select, rows),
            ("COMMIT;", ["OK"]),
            (select, rows),
        ), phase="before restart")
        self.stop(session)
        restarted = self.pipe(directory)
        self.exchange(restarted, ((select, rows),), phase="after restart")
        self.stop(restarted)

    def test_short_row_cannot_hide_duplicate_insert_column(self):
        self.rejected_then_restart(
            "pk_only",
            "CREATE TABLE t(id BIGINT PRIMARY KEY);",
            "INSERT INTO t VALUES(1);",
            "INSERT INTO t(id,id) VALUES(2);",
            "SELECT id FROM t ORDER BY id;",
            ["1"],
        )

    def test_short_duplicate_list_on_multicolumn_table(self):
        self.rejected_then_restart(
            "multicolumn",
            "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);",
            "INSERT INTO t VALUES(1,10);",
            "INSERT INTO t(n,id,n) VALUES(20,2);",
            "SELECT id,n FROM t ORDER BY id;",
            ["1\t10"],
        )

    def test_malformed_later_row_preserves_prior_transaction_insert(self):
        cases = (
            ("short_later_row", "INSERT INTO t(id,n) VALUES(2,20),(3);"),
            ("long_later_row", "INSERT INTO t(n,id) VALUES(20,2),(30,3,300);"),
        )
        for name, invalid in cases:
            with self.subTest(case=name):
                self.rejected_then_restart(
                    name,
                    "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);",
                    "INSERT INTO t(id,n) VALUES(1,10);",
                    invalid,
                    "SELECT id,n FROM t ORDER BY id;",
                    ["1\t10"],
                )

    def test_full_duplicate_lists_and_too_many_values_remain_errors(self):
        cases = (
            ("pk_full_duplicate", False,
             "INSERT INTO t(id,id) VALUES(2,3);"),
            ("pk_too_many", False,
             "INSERT INTO t(id) VALUES(2,3);"),
            ("multicolumn_full_duplicate", True,
             "INSERT INTO t(n,id,n) VALUES(20,2,30);"),
            ("multicolumn_too_many", True,
             "INSERT INTO t(n,id) VALUES(20,2,30);"),
        )
        for name, multicolumn, invalid in cases:
            with self.subTest(case=name):
                if multicolumn:
                    create = "CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);"
                    seed = "INSERT INTO t VALUES(1,10);"
                    select = "SELECT id,n FROM t ORDER BY id;"
                    rows = ["1\t10"]
                else:
                    create = "CREATE TABLE t(id BIGINT PRIMARY KEY);"
                    seed = "INSERT INTO t VALUES(1);"
                    select = "SELECT id FROM t ORDER BY id;"
                    rows = ["1"]
                self.rejected_then_restart(
                    name, create, seed, invalid, select, rows,
                )

    def test_valid_reordered_explicit_columns_commit_and_restart(self):
        directory = self.directory / "valid_reordered"
        session = self.pipe(directory)
        rows = ["1\t10\tone", "2\t20\ttwo", "3\t30\tthree"]
        select = "SELECT id,n,s FROM t ORDER BY id;"
        self.exchange(session, (
            ("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(16));",
             ["OK"]),
            ("BEGIN;", ["OK"]),
            ("INSERT INTO t(s,n,id) VALUES('two',20,2),('one',10,1);", ["OK 2"]),
            ("INSERT INTO t(n,id,s) VALUES(30,3,'three');", ["OK 1"]),
            (select, rows),
            ("COMMIT;", ["OK"]),
            (select, rows),
        ), phase="before restart")
        self.stop(session)
        restarted = self.pipe(directory)
        self.exchange(restarted, ((select, rows),), phase="after restart")
        self.stop(restarted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    args = parser.parse_args()
    StreamFollowup.executable = args.executable.resolve(strict=True)
    if args.tmp_root is not None:
        StreamFollowup.tmp_root = args.tmp_root.resolve(strict=True)
        if not StreamFollowup.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    print("binary=" + str(StreamFollowup.executable), flush=True)
    print("binary_sha256=" + hashlib.sha256(
        StreamFollowup.executable.read_bytes(),
    ).hexdigest(), flush=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(StreamFollowup)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
