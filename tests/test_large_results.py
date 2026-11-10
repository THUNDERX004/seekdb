#!/usr/bin/env python3
"""Check large SELECT responses byte-for-byte using file-backed subprocess I/O.

Run: python -X utf8 -B tests/test_large_results.py build/minivectordb.exe

Fixtures exceed 1 MiB, include raw protocol-looking text, and exercise both
disk scans and cache-resident sorting. These correctness checks do not impose
a process memory limit or assert that sorting has bounded memory. The Docker
resource suite supplies the hard memory limit regression for unordered output.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


START = b"__MVD_LARGE_RESULTS_STARTED__\n"
END = b"__MVD_LARGE_RESULTS_END__\n"
ROW_COUNT = 224
BATCH_SIZE = 16
CHUNK_SIZE = 64 * 1024
SESSION_TIMEOUT = 45


def literal(value):
    return ("'" + value.replace("\\", "\\\\").replace("'", "''")
            .replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t") + "'")


def payload(key, variant="base"):
    prefix = (f"{variant}:{key:04d}\t中文🙂\nraw\r\nquote='; slash=\\\u2028\u2029\n"
              + END.decode("ascii") + "COLUMNS\tlooks_like_metadata\n")
    return prefix + (f"{key:04d}|" + "x" * 123) * 128 + f"\tEND:{key}\n"


def row_bytes(key, variant="base"):
    return f"{key}\t{payload(key, variant)}\tg{key % 7}\n".encode("utf-8")


def compare_parts(stream, parts):
    for part in parts:
        for start in range(0, len(part), CHUNK_SIZE):
            expected = part[start:start + CHUNK_SIZE]
            position = stream.tell()
            actual = stream.read(len(expected))
            if actual != expected:
                offset = next((i for i, pair in enumerate(zip(actual, expected))
                               if pair[0] != pair[1]), min(len(actual), len(expected)))
                raise AssertionError(
                    f"output mismatch at byte {position + offset}: "
                    f"expected {expected[offset:offset + 100]!r}, "
                    f"received {actual[offset:offset + 100]!r}"
                )


def unordered_rows(keys, render=row_bytes, traversal=None):
    """Use only the leading numeric ID for framing; values may contain LF/END."""
    def check(stream):
        remaining = set(keys)
        for _ in range(len(remaining)):
            prefix = bytearray()
            for _ in range(22):
                byte = stream.read(1)
                prefix.extend(byte)
                if byte == b"\t" or not byte:
                    break
            try:
                key = int(prefix[:-1])
            except ValueError as error:
                raise AssertionError(f"missing numeric row ID: {bytes(prefix)!r}") from error
            if key not in remaining or prefix != f"{key}\t".encode("ascii"):
                raise AssertionError(f"unexpected or duplicate row ID: {bytes(prefix)!r}")
            remaining.remove(key)
            compare_parts(stream, (render(key)[len(prefix):],))
            if traversal is not None:
                traversal.append(key)
    return check


class Script:
    def __init__(self, sql, *, header, markers):
        self.sql, self.header = sql, header
        self.marker = END if markers else b""
        self.phases = []

    def request(self, sql, check):
        self.sql.write((sql.rstrip().rstrip(";") + ";\n").encode("utf-8"))
        self.phases.append((sql[:100], check))

    def command(self, sql, response=b"OK\n"):
        self.request(sql, lambda stream: compare_parts(stream, (response, self.marker)))

    def query(self, sql, columns, rows=()):
        def check(stream):
            if self.header:
                compare_parts(stream, (("COLUMNS\t" + "\t".join(columns) + "\n").encode(),))
            if callable(rows):
                rows(stream)
            else:
                compare_parts(stream, rows)
            compare_parts(stream, (self.marker,))
        self.request(sql, check)


def load_fixture(script):
    script.command("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(65535),"
                   "tag VARCHAR(8),v VECTOR(2))")
    script.command("CREATE TABLE empty(s VARCHAR(12),id BIGINT PRIMARY KEY)")
    for start in range(1, ROW_COUNT + 1, BATCH_SIZE):
        stop = min(start + BATCH_SIZE, ROW_COUNT + 1)
        values = ",".join(f"({key},{literal(payload(key))},'g{key % 7}','[{key},0]')"
                          for key in range(start, stop))
        script.command("INSERT INTO t VALUES " + values, f"OK {stop - start}\n".encode())


def tail(stream):
    size = stream.seek(0, 2)
    stream.seek(max(0, size - 4096))
    return stream.read().decode("utf-8", errors="replace")


class LargeResults(unittest.TestCase):
    executable = None
    tmp_root = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run with an explicit executable")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-large-results-", dir=self.tmp_root)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve(strict=True)

    def run_script(self, writer, *, cache_mib=1, header=True, markers=True, startup=True):
        command = [str(self.executable), "--stdio=text", f"--data-dir={self.directory}"]
        if header:
            command.append("--column-header")
        if markers:
            command.append("--ready-marker=" + END.decode("ascii").rstrip("\n"))
        if startup:
            command.append("--startup-marker=" + START.decode("ascii").rstrip("\n"))
        environment = os.environ.copy()
        environment["MVD_CACHE_LIMIT_MB"] = str(cache_mib)
        with tempfile.TemporaryFile() as sql, tempfile.TemporaryFile() as output, \
                tempfile.TemporaryFile() as errors:
            script = Script(sql, header=header, markers=markers)
            writer(script)
            sql.seek(0)
            try:
                result = subprocess.run(command, stdin=sql, stdout=output, stderr=errors,
                                        cwd=self.directory, env=environment,
                                        timeout=SESSION_TIMEOUT, check=False)
            except subprocess.TimeoutExpired:
                self.fail(f"session exceeded {SESSION_TIMEOUT}s; stderr={tail(errors)!r}")
            self.assertEqual(result.returncode, 0, tail(errors))
            output.seek(0)
            if startup:
                compare_parts(output, (START,))
            for phase, check in script.phases:
                try:
                    check(output)
                except AssertionError as error:
                    self.fail(f"{phase}: {error}; stderr={tail(errors)!r}")
            self.assertEqual(output.read(1), b"", "unexpected bytes after final response")

    def test_disk_backed_raw_output_and_limit_offset(self):
        self.run_script(load_fixture)

        def queries(script):
            traversal = []
            script.query("SELECT id AS pk,s AS body,tag FROM t", ("pk", "body", "tag"),
                         unordered_rows(range(1, ROW_COUNT + 1), traversal=traversal))
            # The limited response itself is still over 1 MiB. Compare its
            # window to the preceding scan, without assuming a hash-table order.
            for offset, limit, suffix in ((7, 131, "LIMIT 131 OFFSET 7"),
                                           (169, 9, "LIMIT 9 OFFSET 169"),
                                           (169, 9, "LIMIT 169,9")):
                def check_window(stream, offset=offset, limit=limit):
                    compare_parts(stream, (row_bytes(key)
                                           for key in traversal[offset:offset + limit]))
                script.query("SELECT id,s,tag FROM t " + suffix, ("id", "s", "tag"),
                             check_window)
            script.query("SELECT id,s,tag FROM t", ("id", "s", "tag"),
                         unordered_rows(range(1, ROW_COUNT + 1)))
        self.run_script(queries)

    def test_sorted_and_topk_output_with_cache_resident_rows(self):
        def queries(script):
            load_fixture(script)
            keys = sorted(range(1, ROW_COUNT + 1), key=lambda key: (key % 7, -key))
            # This covers output spilling after sorting small cache-resident
            # data; it deliberately makes no claim about sort memory bounds.
            script.query("SELECT id,s,tag FROM t ORDER BY tag,id DESC", ("id", "s", "tag"),
                         (row_bytes(key) for key in keys))
            script.query("SELECT id,s,tag FROM t ORDER BY tag,id DESC LIMIT 131 OFFSET 11",
                         ("id", "s", "tag"), (row_bytes(key) for key in keys[11:142]))
            script.query("SELECT id,s,L2_DISTANCE(v,'[0,0]') AS d FROM t "
                         "ORDER BY d LIMIT 131 OFFSET 7", ("id", "s", "d"),
                         (f"{key}\t{payload(key)}\t{key}.000000\n".encode("utf-8")
                          for key in range(8, 139)))
        self.run_script(queries, cache_mib=16)

    def test_empty_headers_and_markers_after_large_query(self):
        self.run_script(load_fixture)
        for header in (False, True):
            for markers in (False, True):
                with self.subTest(header=header, markers=markers):
                    def queries(script):
                        script.query("SELECT id,s,tag FROM t", ("id", "s", "tag"),
                                     unordered_rows(range(1, ROW_COUNT + 1)))
                        script.query("SELECT s AS body,id AS pk FROM t WHERE id<0",
                                     ("body", "pk"))
                        script.query("SELECT s,id FROM t LIMIT 0", ("s", "id"))
                        script.query(f"SELECT s,id FROM t LIMIT 5 OFFSET {ROW_COUNT + 1}",
                                     ("s", "id"))
                        script.query("SELECT * FROM empty", ("s", "id"))
                        script.command("COMMIT")
                    self.run_script(queries, header=header, markers=markers, startup=markers)

    def test_explicit_transaction_read_own_writes_and_rollback_restart(self):
        def changes(script, inserted, replaced, deleted):
            script.command("BEGIN")
            script.command(f"UPDATE t SET s={literal(payload(replaced, 'overlay'))} "
                           f"WHERE id={replaced}", b"OK 1\n")
            script.command(f"INSERT INTO t VALUES({inserted},"
                           f"{literal(payload(inserted, 'overlay'))},'g{inserted % 7}',"
                           f"'[{inserted},0]')", b"OK 1\n")
            script.command(f"DELETE FROM t WHERE id={deleted}", b"OK 1\n")
            keys = set(range(1, ROW_COUNT + 1)) - {deleted} | {inserted}
            script.query("SELECT id,s,tag FROM t", ("id", "s", "tag"),
                         unordered_rows(keys, lambda key: row_bytes(
                             key, "overlay" if key in (replaced, inserted) else "base")))

        def queries(script):
            load_fixture(script)
            changes(script, ROW_COUNT + 1, 7, 11)
            script.command("ROLLBACK")
            script.query("SELECT id,s,tag FROM t", ("id", "s", "tag"),
                         unordered_rows(range(1, ROW_COUNT + 1)))
            changes(script, ROW_COUNT + 2, 9, 13)
            # EOF with this transaction open must roll back its large result's
            # source overlay, just as an explicit ROLLBACK did above.
        self.run_script(queries)

        def restarted(script):
            script.query("SELECT id,s,tag FROM t", ("id", "s", "tag"),
                         unordered_rows(range(1, ROW_COUNT + 1)))
            script.query(f"SELECT id,s FROM t WHERE id>{ROW_COUNT}", ("id", "s"))
            script.command("BEGIN")
            script.command("ROLLBACK")
        self.run_script(restarted)

    def test_single_long_value_commit_and_repeated_select_restart(self):
        value = "large\t中文🙂\n" + "漢" * 60000 + "\r\n" + END.decode()
        encoded = value.encode("utf-8")
        copies = 8
        self.assertGreater(len(encoded) * copies, 1024 * 1024)

        def expected_row():
            yield b"1"
            for _ in range(copies):
                yield b"\t"
                yield encoded
            yield b"\n"

        def selects(script):
            for _ in range(2):
                columns = ("id",) + tuple(f"copy{index}" for index in range(copies))
                projection = ",".join(f"s AS copy{index}" for index in range(copies))
                script.query("SELECT id," + projection + " FROM wide", columns, expected_row())
            script.query("SELECT s AS body FROM wide LIMIT 0", ("body",))

        def create(script):
            script.command("CREATE TABLE wide(id BIGINT PRIMARY KEY,s VARCHAR(65535))")
            script.command("BEGIN")
            script.command(f"INSERT INTO wide VALUES(1,{literal(value)})", b"OK 1\n")
            selects(script)
            script.command("COMMIT")
            selects(script)
        self.run_script(create)
        self.run_script(selects)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    parser.add_argument("--case", help="substring selecting a focused check")
    args = parser.parse_args()
    LargeResults.executable = args.executable.resolve(strict=True)
    if args.tmp_root is not None:
        LargeResults.tmp_root = args.tmp_root.resolve(strict=True)
        if not LargeResults.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    names = unittest.defaultTestLoader.getTestCaseNames(LargeResults)
    if args.case:
        names = [name for name in names if args.case in name]
        if not names:
            parser.error("--case selected no tests")
    suite = unittest.TestSuite(LargeResults(name) for name in names)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
