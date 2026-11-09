#!/usr/bin/env python3
"""Pipe-boundary regressions: never append a hidden byte to complete a request.

These tests exercise the delivered executable, not a model of its scanner.
They prove the local documented SQL/stdio behavior only, not compatibility
with an unpublished official driver. Every process owns a temporary database.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest


READY = "__MVD_STDIO_STARTED__"
END = "__MVD_STDIO_END__"


class PipeSession:
    def __init__(self, executable: Path, directory: Path, *, marker=True):
        command = [str(executable), f"--data-dir={directory}",
                   f"--startup-marker={READY}"]
        if marker:
            command.append(f"--ready-marker={END}")
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0,
        )
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        try:
            if self.line() != READY:
                raise AssertionError("database did not emit its requested startup marker")
        except BaseException:
            self.close()
            raise

    def _read(self):
        try:
            for line in self.process.stdout:
                self.lines.put(line.decode("utf-8").rstrip("\r\n"))
        except BaseException as error:
            self.lines.put(error)
        finally:
            self.lines.put(None)

    def line(self):
        try:
            result = self.lines.get(timeout=3)
        except queue.Empty as error:
            raise AssertionError(
                "no response within 3s after a complete semicolon-terminated "
                "request; stdin is still open and no newline/EOF was supplied"
            ) from error
        if isinstance(result, BaseException):
            raise result
        if result is None:
            raise AssertionError("database closed stdout before the response completed")
        return result

    def send(self, *chunks):
        for chunk in chunks:
            payload = chunk.encode("utf-8") if isinstance(chunk, str) else chunk
            self.process.stdin.write(payload)
            self.process.stdin.flush()

    def response(self):
        output = []
        for _ in range(100):
            line = self.line()
            if line == END:
                return output
            output.append(line)
        raise AssertionError("response did not terminate within 100 lines")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.reader.join(timeout=5)
        if self.reader.is_alive():
            raise AssertionError("owned stdout reader failed to terminate")
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream is not None:
                stream.close()


class StdioContract(unittest.TestCase):
    executable = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run with an explicit executable")
        directory = tempfile.TemporaryDirectory(prefix="mvd-stdio-contract-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def pipe(self, **options):
        session = PipeSession(self.executable, self.directory, **options)
        self.addCleanup(session.close)
        return session

    def batch(self, payload, *options):
        result = subprocess.run(
            [str(self.executable), f"--data-dir={self.directory}", *options],
            input=payload.encode("utf-8"), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8"))
        return result.stdout.decode("utf-8").splitlines()

    def test_complete_request_needs_no_newline_or_eof(self):
        session = self.pipe()
        for request in ("BEGIN;", "ROLLBACK;", "COMMIT;"):
            session.send(request)
            self.assertEqual(session.response(), ["OK"], request)

    def test_default_protocol_flushes_at_semicolon(self):
        session = self.pipe(marker=False)
        session.send("BEGIN;")
        self.assertEqual(session.line(), "OK")
        session.send("ROLLBACK;")
        self.assertEqual(session.line(), "OK")

    def test_adjacent_statements_receive_separate_responses(self):
        session = self.pipe()
        session.send("BEGIN;ROLLBACK;COMMIT;")
        for _ in range(3):
            self.assertEqual(session.response(), ["OK"])

    def test_split_comments_quotes_and_utf8_do_not_split_statements(self):
        session = self.pipe()
        session.send("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64));")
        self.assertEqual(session.response(), ["OK"])
        statement = "INSERT INTO t VALUES(1,'中;文''x\\\\'),(2,\"q;\\\"z\");"
        for byte in statement.encode("utf-8"):
            session.send(bytes([byte]))
        self.assertEqual(session.response(), ["OK 2"])
        session.send("/", "* ignored ; '", " */ SELECT id FROM t ORDER BY id;")
        self.assertEqual(session.response(), ["1", "2"])
        session.send("-", "- ignored ; \"\nSELECT id FROM t WHERE id=2;")
        self.assertEqual(session.response(), ["2"])

    def test_error_response_does_not_need_following_byte(self):
        session = self.pipe()
        session.send("ALTER TABLE missing;")
        error = session.response()
        self.assertEqual(len(error), 1)
        self.assertTrue(error[0].startswith("ERROR "), error)
        session.send("BEGIN;ROLLBACK;")
        self.assertEqual(session.response(), ["OK"])
        self.assertEqual(session.response(), ["OK"])

    def test_comment_only_eof_has_no_response(self):
        for text in ("", " \t\n", "-- tail", "-- tail\n", "# tail",
                     "/* tail */", "/* ; ' \" */ ; -- tail",
                     "\ufeff/* comment */\n# tail\n"):
            with self.subTest(text=text):
                self.assertEqual(self.batch(text), [])

    def test_trailing_comments_do_not_generate_extra_frames(self):
        for tail in ("-- tail", "-- tail\n", "# tail", "/* tail */",
                     "/* ; ' */\n-- end\n"):
            with self.subTest(tail=tail):
                self.assertEqual(
                    self.batch("BEGIN;ROLLBACK;" + tail, f"--ready-marker={END}"),
                    ["OK", END, "OK", END],
                )

    def test_comments_and_empty_statements_do_not_add_responses(self):
        self.assertEqual(
            self.batch(";;/* ; */;# ;\n;BEGIN;-- ;\nROLLBACK;;;/* done */"),
            ["OK", "OK"],
        )

    def test_block_comment_delimiters_cannot_overlap(self):
        for text in ("/*/*/", "/*/;BEGIN;ROLLBACK;*/",
                     "/*/ ' \" ; BEGIN; ROLLBACK; */"):
            with self.subTest(text=text):
                self.assertEqual(self.batch(text), [])

    def test_block_comment_cannot_execute_embedded_mutations(self):
        self.assertEqual(
            self.batch("CREATE TABLE t(id BIGINT PRIMARY KEY);"
                       "INSERT INTO t VALUES(1);"),
            ["OK", "OK 1"],
        )
        self.assertEqual(
            self.batch("/*/;DELETE FROM t;*/SELECT id FROM t;"), ["1"],
        )
        self.assertEqual(self.batch("SELECT id FROM t;"), ["1"])

    def test_block_comment_is_preserved_across_pipe_chunks(self):
        session = self.pipe()
        session.send("CREATE TABLE t(id BIGINT PRIMARY KEY);")
        self.assertEqual(session.response(), ["OK"])
        session.send("INSERT INTO t VALUES(1);")
        self.assertEqual(session.response(), ["OK 1"])
        session.send("/", "*", "/;DELETE FROM t;*", "/SELECT id FROM t;")
        self.assertEqual(session.response(), ["1"])
        session.send("SELECT id FROM t;")
        self.assertEqual(session.response(), ["1"])

    def test_malformed_comment_or_string_still_returns_error(self):
        for text in ("/* unterminated", "BEGIN;/* unterminated",
                     "SELECT 'unterminated", "SELECT \"unterminated"):
            with self.subTest(text=text):
                output = self.batch(text)
                prefix = ["OK"] if text.startswith("BEGIN;") else []
                self.assertEqual(output[:-1], prefix)
                self.assertTrue(output[-1].startswith("ERROR SQL_SYNTAX:"), output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--case", help="substring selecting a focused regression")
    args = parser.parse_args()
    StdioContract.executable = args.executable.resolve(strict=True)
    names = unittest.defaultTestLoader.getTestCaseNames(StdioContract)
    if args.case:
        names = [name for name in names if args.case in name]
        if not names:
            parser.error("--case did not select any tests")
    suite = unittest.TestSuite(StdioContract(name) for name in names)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
