#!/usr/bin/env python3
"""Focused local stdio regressions; not evidence of formal admission.

Exercise the existing binary through real pipes. The public package does not
freeze its complete transport or numeric error mapping. Each test owns a fresh
temporary database; --tmp-root permits an explicitly authorized scratch root.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import queue
import subprocess
import tempfile
import unittest

from run_public import response_failures
from test_stdio_contract import END, READY, PipeSession


class Round6StreamContract(unittest.TestCase):
    executable = None
    tmp_root = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run with an explicit executable")
        directory = tempfile.TemporaryDirectory(
            prefix=".mvd-round6-stream-", dir=self.tmp_root,
        )
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name).resolve()

    def pipe(self, *, marker=True):
        session = PipeSession(self.executable, self.directory, marker=marker)
        self.addCleanup(session.close)
        return session

    def batch(self, payload, *options):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        result = subprocess.run(
            [str(self.executable), f"--data-dir={self.directory}", *options],
            input=payload, capture_output=True, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8"))
        return result.stdout.decode("utf-8").splitlines()

    def quiet(self, session):
        # Separate writes with an actual read window, rather than relying on
        # several immediately adjacent writes becoming separate reader chunks.
        try:
            unexpected = session.lines.get(timeout=0.025)
        except queue.Empty:
            return
        self.fail(f"premature output or closed pipe: {unexpected!r}")

    def create_text_table(self, session):
        session.send("CREATE TABLE r6(id BIGINT PRIMARY KEY,s VARCHAR(64));")
        self.assertEqual(session.response(), ["OK"])

    def test_every_split_of_crlf_comments_preserves_following_request(self):
        session = self.pipe()
        self.create_text_table(session)
        session.send("INSERT INTO r6 VALUES(1,'kept');")
        self.assertEqual(session.response(), ["OK 1"])
        for comment in (b"-- ; ' \"", b"# ; ' \""):
            payload = comment + b"\r\nSELECT id FROM r6;"
            for position in range(1, len(payload)):
                with self.subTest(comment=comment, split=position):
                    session.send(payload[:position])
                    self.quiet(session)
                    session.send(payload[position:])
                    self.assertEqual(session.response(), ["1"])

    def test_every_split_of_block_comment_keeps_embedded_sql_inert(self):
        session = self.pipe()
        self.create_text_table(session)
        session.send("INSERT INTO r6 VALUES(1,'kept');")
        self.assertEqual(session.response(), ["OK 1"])
        payload = b"/* ' \" ; DELETE FROM r6; */SELECT id FROM r6;"
        for position in range(1, len(payload)):
            with self.subTest(split=position):
                session.send(payload[:position])
                self.quiet(session)
                session.send(payload[position:])
                self.assertEqual(session.response(), ["1"])

    def test_delayed_quote_escape_and_utf8_boundaries_preserve_values(self):
        session = self.pipe()
        self.create_text_table(session)
        cases = (
            ("'中;文''x\\\\'", "中;文'x\\"),
            ('"q;\\"z"', 'q;"z'),
            ("'--#/*;*/'", "--#/*;*/"),
        )
        for key, (literal, expected) in enumerate(cases, 1):
            request = f"INSERT INTO r6 VALUES({key},{literal});".encode("utf-8")
            # Every prefix is incomplete until the final semicolon. This also
            # splits inside UTF-8, doubled quotes and backslash escape pairs.
            for byte in request[:-1]:
                session.send(bytes([byte]))
                self.quiet(session)
            session.send(request[-1:])
            self.assertEqual(session.response(), ["OK 1"])
            session.send(f"SELECT s FROM r6 WHERE id={key};")
            self.assertEqual(session.response(), [expected])

    def test_crlf_whitespace_may_arrive_after_response(self):
        session = self.pipe()
        session.send("BEGIN;")
        self.assertEqual(session.response(), ["OK"])
        session.send(b"\r")
        self.quiet(session)
        session.send(b"\n/* tail */")
        self.quiet(session)
        session.send("ROLLBACK;")
        self.assertEqual(session.response(), ["OK"])

    def test_default_records_continue_after_errors_with_open_stdin(self):
        session = self.pipe(marker=False)
        session.send("CREATE TABLE r6(id BIGINT PRIMARY KEY);")
        self.assertEqual(session.line(), "OK")
        session.send("INSERT INTO r6 VALUES(1);")
        self.assertEqual(session.line(), "OK 1")
        session.send("INSERT INTO r6 VALUES(1);")
        self.assertRegex(session.line(), r"^ERROR [A-Z_]+: .+")
        session.send("ALTER TABLE r6 ADD extra BIGINT;")
        self.assertRegex(session.line(), r"^ERROR [A-Z_]+: .+")
        session.send("SELECT id FROM r6;")
        self.assertEqual(session.line(), "1")
        self.quiet(session)

    def test_eof_comment_tails_do_not_add_response_frames(self):
        for tail in (b"-- ; ' \"", b"# ; ' \"\r\n",
                     b"/* ; ' \" */", b"\r\n; ;"):
            with self.subTest(tail=tail):
                self.assertEqual(
                    self.batch(b"BEGIN;ROLLBACK;" + tail,
                               f"--ready-marker={END}"),
                    ["OK", END, "OK", END],
                )

    def test_eof_malformed_tail_reports_one_error_after_prior_response(self):
        for tail in (b"/*", b"/* x *", b"SELECT '",
                     b"SELECT 'x\\", b'SELECT "x\\'):
            with self.subTest(tail=tail):
                output = self.batch(b"BEGIN;ROLLBACK;" + tail,
                                    f"--ready-marker={END}")
                self.assertEqual(output[:4], ["OK", END, "OK", END])
                self.assertEqual(len(output), 6, output)
                self.assertRegex(output[4], r"^ERROR SQL_SYNTAX: .+")
                self.assertEqual(output[5], END)

    def test_response_flags_match_public_assertions_without_frozen_error_map(self):
        requests = (
            ("CREATE TABLE r6(id BIGINT PRIMARY KEY,s VARCHAR(16));", {}),
            ("INSERT INTO r6 VALUES(1,'中文');", {"affected_rows": 1}),
            ("INSERT INTO r6 VALUES(1,'duplicate');", {"error": "duplicate_key"}),
            ("ALTER TABLE r6 ADD extra BIGINT;", {"error": "unsupported_sql"}),
            ("SELECT id,s FROM r6 WHERE id=2;",
             {"columns": ["id", "s"], "rows": []}),
            ("SELECT s,id FROM r6;",
             {"columns": ["s", "id"], "rows": [["中文", 1]]}),
        )
        # These are documented local CLI options. The public package freezes
        # assertions, not the complete transport or semantic-to-numeric map.
        for separated in (False, True):
            for numeric in (False, True):
                with self.subTest(separated=separated, numeric=numeric):
                    with tempfile.TemporaryDirectory(
                        prefix="flags-", dir=self.directory,
                    ) as directory:
                        def option(name, value):
                            return [name, value] if separated else [f"{name}={value}"]

                        command = [str(self.executable), "--column-header"]
                        for name, value in (("--data-dir", directory),
                                            ("--ready-marker", END),
                                            ("--startup-marker", READY),
                                            ("--vector-mode", "ann")):
                            command.extend(option(name, value))
                        if numeric:
                            command.append("--numeric-errors")
                        result = subprocess.run(
                            command, input="".join(sql for sql, _ in requests).encode(),
                            capture_output=True, timeout=10, check=False,
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)
                        lines = result.stdout.decode("utf-8").splitlines()
                        self.assertEqual(lines.pop(0), READY)
                        for sql, expectation in requests:
                            self.assertIn(END, lines, (sql, lines))
                            stop = lines.index(END)
                            frame, lines = lines[:stop], lines[stop + 1:]
                            self.assertEqual(response_failures(sql, expectation, frame), [])
                            if "error" in expectation:
                                pattern = (r"^ERROR [0-9]+: .+" if numeric
                                           else r"^ERROR [A-Z_]+: .+")
                                self.assertRegex(frame[0], pattern)
                        self.assertEqual(lines, [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    parser.add_argument("--case", help="substring selecting a focused test")
    args = parser.parse_args()
    Round6StreamContract.executable = args.executable.resolve(strict=True)
    if args.tmp_root is not None:
        Round6StreamContract.tmp_root = args.tmp_root.resolve(strict=True)
        if not Round6StreamContract.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    print("binary_sha256=" + hashlib.sha256(
        Round6StreamContract.executable.read_bytes(),
    ).hexdigest(), flush=True)
    names = unittest.defaultTestLoader.getTestCaseNames(Round6StreamContract)
    if args.case:
        names = [name for name in names if args.case in name]
        if not names:
            parser.error("--case selected no tests")
    suite = unittest.TestSuite(Round6StreamContract(name) for name in names)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
