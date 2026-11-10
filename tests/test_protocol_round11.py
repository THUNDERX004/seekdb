#!/usr/bin/env python3
"""Independent byte-pipe checks for the documented local protocol.

These tests do not invent an official stdout encoding, error-code mapping,
signal contract, or driver. The raw CRLF, control-Z, and output-byte checks
exercise the local UTF-8/raw-value behavior; failures on Windows do not prove
that the Linux submission or an unpublished official driver fails.

Every test owns an isolated temporary database. No source/build edits or
fixture generation are performed. Current defects are ordinary failures,
not skipped tests or expected failures.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest


START = b"__MVD_R11_STARTED__"
END = b"__MVD_R11_END__"
CREATE = b"CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64));"


def record_lines(data):
    """Remove one record terminator, never all CR/LF or Unicode separators."""
    if not data:
        return []
    if not data.endswith(b"\n"):
        raise AssertionError(f"unterminated output record: {data!r}")
    return [line[:-1] if line.endswith(b"\r") else line
            for line in data[:-1].split(b"\n")]


class BytePipe:
    def __init__(self, executable, directory):
        self.process = subprocess.Popen(
            [str(executable), f"--data-dir={directory}",
             "--startup-marker=" + START.decode(),
             "--ready-marker=" + END.decode()],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0, cwd=directory,
        )
        self.chunks = queue.Queue()
        self.pending = bytearray()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while data := self.process.stdout.read(4096):
                self.chunks.put(data)
        except BaseException as error:
            self.chunks.put(error)
        finally:
            self.chunks.put(None)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def line(self):
        while b"\n" not in self.pending:
            try:
                data = self.chunks.get(timeout=3)
            except queue.Empty as error:
                raise AssertionError("no response while stdin remains open") from error
            if isinstance(data, BaseException):
                raise data
            if data is None:
                raise AssertionError(f"unexpected stdout EOF: {self.pending!r}")
            self.pending.extend(data)
        stop = self.pending.index(b"\n") + 1
        line = bytes(self.pending[:stop])
        del self.pending[:stop]
        return record_lines(line)[0]

    def frame(self):
        result = []
        for _ in range(100):
            line = self.line()
            if line == END:
                return result
            result.append(line)
        raise AssertionError("no response marker within 100 records")

    def quiet(self):
        if self.pending:
            raise AssertionError(f"unexpected buffered output: {self.pending!r}")
        try:
            data = self.chunks.get(timeout=0.02)
        except queue.Empty:
            return
        raise AssertionError(f"unexpected output or EOF: {data!r}")

    def finish(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=5)
        self.reader.join(timeout=3)
        if self.reader.is_alive():
            raise AssertionError("stdout reader survived normal process exit")
        remaining = bytes(self.pending)
        self.pending.clear()
        while not self.chunks.empty():
            data = self.chunks.get_nowait()
            if isinstance(data, BaseException):
                raise data
            if data is not None:
                remaining += data
        return code, remaining, self.process.stderr.read()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.reader.join(timeout=3)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()
        if self.reader.is_alive():
            raise AssertionError("owned stdout reader did not exit")


class ProtocolRound11(unittest.TestCase):
    executable = None
    tmp_root = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run with an explicit executable")
        temporary = tempfile.TemporaryDirectory(
            prefix="mvd-protocol-r11-", dir=self.tmp_root,
        )
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve(strict=True)

    def batch(self, sql, *options):
        result = subprocess.run(
            [str(self.executable), f"--data-dir={self.directory}", *options],
            input=sql, capture_output=True, timeout=5, cwd=self.directory,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def frames(self, sql):
        output = self.batch(sql, "--ready-marker=" + END.decode())
        frames, pending = [], []
        for line in record_lines(output):
            if line == END:
                frames.append(pending)
                pending = []
            else:
                pending.append(line)
        self.assertEqual(pending, [], output)
        return frames

    def pipe(self):
        session = BytePipe(self.executable, self.directory)
        self.addCleanup(session.close)
        self.assertEqual(session.line(), START)
        session.quiet()
        return session

    def stop(self, session):
        code, remaining, stderr = session.finish()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(remaining, b"")
        session.close()

    def test_default_empty_result_and_startup_do_not_add_stdout_records(self):
        output = self.batch(CREATE + b"SELECT s FROM t;COMMIT;")
        self.assertEqual(record_lines(output), [b"OK", b"OK"])

    def test_ready_marker_frames_empty_result_and_error_then_continues(self):
        session = self.pipe()
        session.send(CREATE + b"SELECT s FROM t;ALTER TABLE t ADD x BIGINT;COMMIT;")
        self.assertEqual(session.frame(), [b"OK"])
        self.assertEqual(session.frame(), [])
        error = session.frame()
        self.assertEqual(len(error), 1)
        self.assertRegex(error[0], rb"^ERROR [^\s:]+: .+")
        self.assertEqual(session.frame(), [b"OK"])
        session.quiet()
        self.stop(session)

    def test_column_metadata_describes_empty_query_without_official_encoding_claim(self):
        output = self.batch(
            CREATE + b"SELECT s,id FROM t;",
            "--column-header", "--startup-marker=" + START.decode(),
            "--ready-marker=" + END.decode(),
        )
        self.assertEqual(record_lines(output),
                         [START, b"OK", END, b"COLUMNS\ts\tid", END])

    def test_byte_chunks_utf8_quotes_comments_and_adjacent_requests(self):
        session = self.pipe()
        session.send(CREATE)
        self.assertEqual(session.frame(), [b"OK"])
        sql = "/* ; ' */INSERT INTO t VALUES(1,'中😀;文''x\\\\');".encode()
        for byte in sql[:-1]:
            session.send(bytes([byte]))
            session.quiet()
        session.send(sql[-1:])
        self.assertEqual(session.frame(), [b"OK 1"])
        session.send(b"# ; '\r\nSELECT s FROM t;-- ; \"\r\nCOMMIT;")
        self.assertEqual(session.frame(), ["中😀;文'x\\".encode()])
        self.assertEqual(session.frame(), [b"OK"])
        self.stop(session)

    def test_eof_restart_rolls_back_only_uncommitted_changes(self):
        session = self.pipe()
        requests = (
            (CREATE, [b"OK"]),
            ("INSERT INTO t VALUES(1,'中文😀');", [b"OK 1"]),
            (b"BEGIN;", [b"OK"]),
            (b"UPDATE t SET s='temporary' WHERE id=1;", [b"OK 1"]),
            (b"INSERT INTO t VALUES(2,'uncommitted');", [b"OK 1"]),
        )
        for sql, expected in requests:
            session.send(sql)
            self.assertEqual(session.frame(), expected)
        self.stop(session)
        restarted = self.pipe()
        restarted.send(b"SELECT id,s FROM t ORDER BY id;")
        self.assertEqual(restarted.frame(), ["1\t中文😀".encode()])
        self.stop(restarted)

    def test_local_input_preserves_raw_crlf_inside_string_literal(self):
        # Compare numeric IDs so this does not depend on multiline row framing.
        frames = self.frames(
            CREATE + b"INSERT INTO t VALUES(1,'a\r\nb');"
            b"SELECT id FROM t WHERE s='a\\r\\nb';"
            b"SELECT id FROM t WHERE s='a\\nb';"
        )
        self.assertEqual(frames, [[b"OK"], [b"OK 1"], [b"1"], []])

    def test_local_control_z_in_comment_is_not_transport_eof(self):
        # This is a UTF-8 byte in a comment, followed by complete ASCII SQL.
        frames = self.frames(b"BEGIN;# \x1a\nROLLBACK;COMMIT;")
        self.assertEqual(frames, [[b"OK"], [b"OK"], [b"OK"]])

    def test_local_raw_output_preserves_literal_lf_besides_record_terminator(self):
        output = self.batch(
            CREATE + b"INSERT INTO t VALUES(1,'a\\nb');SELECT s FROM t;",
        )
        # Allow either platform record terminator, but not mutation of row data.
        self.assertTrue(output.startswith((b"OK\n", b"OK\r\n")), output)
        eol = b"\r\n" if output.startswith(b"OK\r\n") else b"\n"
        self.assertEqual(output, b"OK" + eol + b"OK 1" + eol + b"a\nb" + eol)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    parser.add_argument("--case", help="substring selecting a focused check")
    args = parser.parse_args()
    ProtocolRound11.executable = args.executable.resolve(strict=True)
    if args.tmp_root is not None:
        ProtocolRound11.tmp_root = args.tmp_root.resolve(strict=True)
        if not ProtocolRound11.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    print("binary=" + str(ProtocolRound11.executable), flush=True)
    print("binary_sha256=" + hashlib.sha256(
        ProtocolRound11.executable.read_bytes(),
    ).hexdigest(), flush=True)
    print("os_name=" + os.name, flush=True)
    names = unittest.defaultTestLoader.getTestCaseNames(ProtocolRound11)
    if args.case:
        names = [name for name in names if args.case in name]
        if not names:
            parser.error("--case selected no tests")
    suite = unittest.TestSuite(ProtocolRound11(name) for name in names)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
