#!/usr/bin/env python3
"""Independent executable-level regressions for the official JSONL contract.

Contract source: STDIO_PROTOCOL.md and protocol_check at official commit
f4d4e26653af823a7601668802f80e374ad6643c. No build or external packages needed.

Run (test options must precede the executable and its optional argv):
  python -B tests/test_json_protocol.py /path/to/minivectordb [binary arguments]
  python -B tests/test_json_protocol.py --robustness --timeout 15 /path/to/minivectordb

Default tests cover the valid protocol, SQL regressions, and the requested local
--stdio=text compatibility. --robustness additionally checks malformed/unknown
requests and bare stdin EOF; their recovery behavior is not fixed by the official
protocol, so those extras are deliberately excluded from the mandatory group.
Every process receives --data-dir=<isolated temporary directory>; no /data or
installed database is used. No logs, bytecode, or build artifacts are generated.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from jsonl_client import JsonlClient, same_json


def sql_string(value):
    """Use the SQL lexer's doubled quotes and MySQL-style backslash escapes."""
    escaped = (value.replace("\\", "\\\\").replace("'", "''")
               .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t"))
    return "'" + escaped + "'"


class ExecutableCase(unittest.TestCase):
    argv = None
    timeout = 10.0
    tmp_root = None

    def setUp(self):
        if self.argv is None:
            self.skipTest("run this file with an explicit executable path")
        directory = tempfile.TemporaryDirectory(prefix="mvd-jsonl-", dir=self.tmp_root)
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def client(self):
        client = JsonlClient(self.argv, self.directory, timeout=self.timeout)
        self.addCleanup(client.close)
        return client

    def success(self, response):
        self.assertIs(response.get("ok"), True, repr(response)[:1000])
        return response

    def failure(self, response):
        self.assertIs(response.get("ok"), False, repr(response)[:1000])
        error = response.get("error")
        self.assertIs(type(error), dict)
        self.assertIs(type(error.get("code")), str)
        self.assertTrue(error["code"], "error.code must be nonempty; mapping is not frozen")

    def affected(self, response, count):
        self.success(response)
        self.assertIs(type(response.get("affected_rows")), int)
        self.assertEqual(response["affected_rows"], count)

    def rows(self, response, columns, wanted):
        self.success(response)
        self.assertTrue(same_json(response.get("columns"), columns), "columns/order differ")
        actual = response.get("rows")
        self.assertIs(type(actual), list)
        self.assertEqual(len(actual), len(wanted), "row count differs")
        for index, (row, expected) in enumerate(zip(actual, wanted)):
            self.assertTrue(same_json(row, expected), f"typed row {index} differs")

    def numeric(self, actual, expected):
        # Official float tolerance; bool is never an acceptable numeric value.
        self.assertIn(type(actual), (int, float))
        self.assertTrue(math.isfinite(actual), "numeric result must be finite")
        self.assertTrue(math.isclose(actual, expected, abs_tol=1e-5, rel_tol=1e-5),
                        f"numeric value {actual!r} differs from {expected!r}")


class JsonProtocolContract(ExecutableCase):
    def test_ready_is_exact_and_flushed_before_any_input(self):
        client = self.client()  # Constructor waits for ready before any write.
        client.assert_quiet(timeout=0.05)
        client.shutdown()

    def test_ids_preserve_all_json_types_and_huge_signed_integers(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE ids(id BIGINT PRIMARY KEY)"))
        self.affected(client.execute("INSERT INTO ids VALUES(7)"), 1)
        huge = (1 << 255) + 9007199254740993
        identifiers = [
            0, -1, -(1 << 63), (1 << 63) - 1, huge, -huge, 1.25,
            "", "id;\t\n\\\"中文🌊", None, True, False, [], {},
            [huge, -huge, None, True, False, "数组🌊", {"nested": [1, False]}],
            {"整数": huge, "negative": -huge, "null": None,
             "boolean": False, "array": ["\t\n\\\"", {"x": True}]},
        ]
        for index, ident in enumerate(identifiers):
            with self.subTest(index=index, id_type=type(ident).__name__):
                response = client.execute("SELECT id FROM ids", ident=ident,
                                          ensure_ascii=bool(index % 2))
                self.rows(response, ["id"], [[7]])
        client.assert_quiet(timeout=0.05)
        client.shutdown(ident={"shutdown": [None, True, -huge, "再见🌊"]})

    def test_json_wrapper_executes_sql_without_a_semicolon(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE wrapped(id BIGINT PRIMARY KEY,s VARCHAR(64))"))
        self.affected(client.execute("INSERT INTO wrapped VALUES(1,'a;not another request')"), 1)
        self.rows(client.execute("SELECT s,id FROM wrapped ORDER BY id"),
                  ["s", "id"], [["a;not another request", 1]])
        client.shutdown()

    def test_semicolon_comments_and_multi_statement_atomic_rejection(self):
        client = self.client()
        self.success(client.execute(
            "CREATE TABLE endings(id BIGINT PRIMARY KEY,s VARCHAR(64)); /* trailing ; */"
        ))
        self.affected(client.execute(
            "INSERT INTO endings VALUES(1,'keep;me'); -- trailing ; quote ' \n"
        ), 1)
        self.rows(client.execute("SELECT id,s FROM endings; -- trailing comment"),
                  ["id", "s"], [[1, "keep;me"]])
        self.failure(client.execute(
            "INSERT INTO endings VALUES(2,'must not persist'); "
            "DELETE FROM endings WHERE id=1;"
        ))
        self.rows(client.execute("SELECT id,s FROM endings ORDER BY id"),
                  ["id", "s"], [[1, "keep;me"]])
        client.shutdown()

    def test_bigint_boundaries_are_exact_json_integers(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE ints(id BIGINT PRIMARY KEY,n BIGINT)"))
        expected = [
            [-(1 << 63), (1 << 63) - 1],
            [-9007199254740993, 9007199254740993],
            [0, 0],
            [9007199254740993, -9007199254740993],
            [(1 << 63) - 1, -(1 << 63)],
        ]
        values = ",".join(f"({ident},{number})" for ident, number in expected)
        self.affected(client.execute("INSERT INTO ints VALUES" + values), len(expected))
        self.rows(client.execute("SELECT id,n FROM ints ORDER BY id"), ["id", "n"], expected)
        client.shutdown()

    def test_empty_results_retain_column_names_and_order(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE empty_rows(id BIGINT PRIMARY KEY,s VARCHAR(8))"))
        self.rows(client.execute("SELECT s,id FROM empty_rows ORDER BY id"), ["s", "id"], [])
        self.affected(client.execute("INSERT INTO empty_rows VALUES(1,'value')"), 1)
        self.rows(client.execute("SELECT id,s FROM empty_rows WHERE id=2"), ["id", "s"], [])
        self.rows(client.execute("SELECT s,id FROM empty_rows LIMIT 0"), ["s", "id"], [])
        client.shutdown()

    def test_affected_rows_are_integer_counts(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE counts(id BIGINT PRIMARY KEY,n BIGINT)"))
        self.affected(client.execute("INSERT INTO counts VALUES(1,1),(2,2),(3,3)"), 3)
        self.affected(client.execute("UPDATE counts SET n=9 WHERE id>=2"), 2)
        self.affected(client.execute("DELETE FROM counts WHERE n=9"), 2)
        self.affected(client.execute("UPDATE counts SET n=0 WHERE id=99"), 0)
        self.affected(client.execute("DELETE FROM counts WHERE id=99"), 0)
        self.rows(client.execute("SELECT id,n FROM counts"), ["id", "n"], [[1, 1]])
        client.shutdown()

    def test_strings_are_escaped_once_in_one_physical_response_line(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE strings(id BIGINT PRIMARY KEY,s VARCHAR(512))"))
        values = [
            "", "tab\tvalue", "first\nsecond\r\nthird",
            "backslash\\ literal escapes \\n \\t \\r and trailing\\",
            "single ' doubled '' double \" and \\\"",
            "semicolon; -- quoted, not a comment; /* neither */",
            "你好🌊 café e\u0301 \u2028 \u2029",
            "all;\t\n\r'\"\\中文🌊",
        ]
        sql = "INSERT INTO strings VALUES" + ",".join(
            f"({index},{sql_string(value)})" for index, value in enumerate(values)
        )
        self.affected(client.execute(sql), len(values))
        self.rows(client.execute("SELECT id,s FROM strings ORDER BY id"),
                  ["id", "s"], [[index, value] for index, value in enumerate(values)])
        client.assert_quiet(timeout=0.05)
        client.shutdown()

    def test_vectors_are_arrays_and_distances_are_finite_numbers(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE vectors(id BIGINT PRIMARY KEY,v VECTOR(2))"))
        self.affected(client.execute(
            "INSERT INTO vectors VALUES(1,'[0,0]'),(2,'[3,4]'),"
            "(3,'[0.125,-0.5]'),(4,'[16777217,0]')"
        ), 4)
        response = self.success(client.execute("SELECT id,v FROM vectors ORDER BY id"))
        self.assertEqual(response.get("columns"), ["id", "v"])
        rows = response.get("rows")
        self.assertIs(type(rows), list)
        self.assertEqual(len(rows), 4)
        for row, expected in zip(rows, [(1, [0, 0]), (2, [3, 4]),
                                        (3, [0.125, -0.5]), (4, [16777216, 0])]):
            self.assertIs(type(row), list)
            self.assertEqual(len(row), 2)
            self.assertTrue(same_json(row[0], expected[0]))
            self.assertIs(type(row[1]), list)
            self.assertEqual(len(row[1]), 2)
            for actual, wanted in zip(row[1], expected[1]):
                self.numeric(actual, wanted)
        response = self.success(client.execute(
            "SELECT id,L2_DISTANCE(v,'[0,0]') AS distance "
            "FROM vectors WHERE id<=2 ORDER BY id"
        ))
        self.assertEqual(response.get("columns"), ["id", "distance"])
        self.assertIs(type(response.get("rows")), list)
        self.assertEqual(len(response["rows"]), 2)
        for row, ident, distance in zip(response["rows"], [1, 2], [0, 5]):
            self.assertIs(type(row), list)
            self.assertEqual(len(row), 2)
            self.assertTrue(same_json(row[0], ident))
            self.numeric(row[1], distance)
        client.shutdown()

    def test_search_mode_switches_per_request(self):
        client = self.client()
        self.success(client.execute(
            "CREATE TABLE modes(id BIGINT PRIMARY KEY,category VARCHAR(8),v VECTOR(2))"
        ))
        self.affected(client.execute(
            "INSERT INTO modes VALUES(1,'keep','[3,4]'),(2,'keep','[0,0]'),"
            "(3,'keep','[1,0]'),(4,'skip','[0,0]')"
        ), 4)
        self.success(client.execute("CREATE VECTOR INDEX mode_idx ON modes(v)"))
        sql = ("SELECT id FROM modes WHERE category='keep' "
               "ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2")
        for mode in ("exact", "ann", "exact", "ann", "exact"):
            with self.subTest(mode=mode):
                self.rows(client.execute(sql, mode=mode), ["id"], [[2], [3]])
        client.shutdown()

    def test_sql_errors_and_unsupported_null_do_not_end_session(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE errors(id BIGINT PRIMARY KEY,s VARCHAR(16))"))
        self.affected(client.execute("INSERT INTO errors VALUES(1,'original')"), 1)
        for index, sql in enumerate([
            "INSERT INTO errors VALUES(1,'duplicate')",
            "ALTER TABLE errors ADD extra BIGINT",
            "INSERT INTO errors VALUES(2,NULL)",
            "BEGIN;中",
            "SELECT id FROM errors WHERE id=中",
        ]):
            with self.subTest(sql=sql):
                self.failure(client.execute(sql, ident={"error": [index, None, True, "错误"]}))
                self.rows(client.execute("SELECT id,s FROM errors ORDER BY id"),
                          ["id", "s"], [[1, "original"]])
        client.shutdown()

    def test_shutdown_rolls_back_and_restart_preserves_committed_data(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE durable(id BIGINT PRIMARY KEY,s VARCHAR(32))"))
        self.affected(client.execute("INSERT INTO durable VALUES(1,'autocommit')"), 1)
        self.success(client.execute("BEGIN"))
        self.affected(client.execute("INSERT INTO durable VALUES(3,'committed')"), 1)
        self.success(client.execute("COMMIT"))
        self.success(client.execute("BEGIN"))
        self.affected(client.execute("UPDATE durable SET s='pending' WHERE id=1"), 1)
        self.affected(client.execute("INSERT INTO durable VALUES(2,'pending')"), 1)
        self.affected(client.execute("DELETE FROM durable WHERE id=3"), 1)
        self.rows(client.execute("SELECT id,s FROM durable ORDER BY id"),
                  ["id", "s"], [[1, "pending"], [2, "pending"]])
        client.shutdown(ident="shutdown;\n回滚🌊")
        restarted = self.client()  # Same explicit temporary data directory.
        self.rows(restarted.execute("SELECT id,s FROM durable ORDER BY id"),
                  ["id", "s"], [[1, "autocommit"], [3, "committed"]])
        restarted.shutdown()

    def test_shutdown_response_precedes_large_pending_overlay_release(self):
        client = self.client()
        self.success(client.execute(
            "CREATE TABLE pending(id BIGINT PRIMARY KEY,v VECTOR(16))"
        ))
        self.success(client.execute("BEGIN"))
        for start in range(0, 8000, 1000):
            rows = ",".join(
                f"({ident},'[{ident},1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]')"
                for ident in range(start, start + 1000)
            )
            self.affected(client.execute("INSERT INTO pending VALUES" + rows), 1000)
        started = time.monotonic()
        client.shutdown()
        self.assertLess(time.monotonic() - started, min(self.timeout, 5.0))
        restarted = self.client()
        self.rows(restarted.execute("SELECT id FROM pending ORDER BY id"),
                  ["id"], [])
        restarted.shutdown()

    def test_spooled_response_over_one_mib_preserves_escaping_and_row_boundaries(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE spool(id BIGINT PRIMARY KEY,s VARCHAR(12000))"))
        fragment = "tab\tline\ncr\rquote'\"slash\\;中文🌊\u2028\u2029|"
        body = (fragment * (9216 // len(fragment) + 1))[:9216]
        expected = [[index, f"{index:03d}:" + body] for index in range(130)]
        self.success(client.execute("BEGIN"))
        for ident, value in expected:
            self.affected(client.execute(f"INSERT INTO spool VALUES({ident},{sql_string(value)})"), 1)
        self.success(client.execute("COMMIT"))
        self.rows(client.execute("SELECT id,s FROM spool ORDER BY id"), ["id", "s"], expected)
        self.assertGreater(client.last_line_size, 1024 * 1024)

        response = self.success(client.execute("SELECT id,s FROM spool"))
        self.assertGreater(client.last_line_size, 1024 * 1024)
        self.assertTrue(same_json(response.get("columns"), ["id", "s"]))
        actual = response.get("rows")
        self.assertIs(type(actual), list)
        self.assertEqual(len(actual), len(expected))
        remaining = dict(expected)
        for index, row in enumerate(actual):
            self.assertIs(type(row), list)
            self.assertEqual(len(row), 2)
            self.assertIs(type(row[0]), int)
            self.assertIn(row[0], remaining, "unknown or duplicate row in unordered result")
            self.assertTrue(same_json(row[1], remaining.pop(row[0])),
                            f"unordered string in row {index} differs")
        self.assertFalse(remaining)
        self.rows(client.execute("SELECT id,s FROM spool ORDER BY id DESC LIMIT 3"),
                  ["id", "s"], expected[-1:-4:-1])
        self.rows(client.execute("SELECT s,id FROM spool WHERE id<0"), ["s", "id"], [])
        self.rows(client.execute("SELECT id,s FROM spool LIMIT 0"), ["id", "s"], [])
        client.shutdown()


class LegacyTextCompatibility(ExecutableCase):
    """Local compatibility requested alongside the official default protocol."""

    def test_explicit_stdio_text_retains_legacy_output(self):
        result = subprocess.run(
            [*self.argv, "--stdio=text", f"--data-dir={self.directory}"],
            input=("CREATE TABLE legacy(id BIGINT PRIMARY KEY,s VARCHAR(16));\n"
                   "INSERT INTO legacy VALUES(1,'legacy');\n"
                   "SELECT id,s FROM legacy;\n").encode("utf-8"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=self.timeout, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        self.assertEqual(result.stdout.decode("utf-8").splitlines(),
                         ["OK", "OK 1", "1\tlegacy"])


class OptionalRobustness(ExecutableCase):
    """Extra local recovery checks, not frozen official invalid-input semantics."""

    def test_unknown_operation_returns_error_and_accepts_next_request(self):
        client = self.client()
        self.failure(client.request({"id": [None, "unknown🌊"], "op": "unknown-operation"}))
        self.success(client.execute("BEGIN"))
        self.success(client.execute("ROLLBACK"))
        client.shutdown()

    def test_malformed_json_and_unwrapped_sql_do_not_poison_next_request(self):
        client = self.client()
        for raw in (b'{"id":7,"op":\n', b'{"id":8,"op":}\n', b"BEGIN;\n"):
            with self.subTest(raw=raw):
                client.send_bytes(raw)
                # An unrecoverable request ID has no specified echo value.
                self.failure(client.read_message())
                self.success(client.execute("BEGIN"))
                self.success(client.execute("ROLLBACK"))
        client.shutdown()

    def test_bare_eof_is_quiet_and_rolls_back_before_restart(self):
        client = self.client()
        self.success(client.execute("CREATE TABLE eof_rows(id BIGINT PRIMARY KEY,n BIGINT)"))
        self.affected(client.execute("INSERT INTO eof_rows VALUES(1,1)"), 1)
        self.success(client.execute("BEGIN"))
        self.affected(client.execute("UPDATE eof_rows SET n=2 WHERE id=1"), 1)
        self.affected(client.execute("INSERT INTO eof_rows VALUES(2,2)"), 1)
        client.end_of_input()
        restarted = self.client()
        self.rows(restarted.execute("SELECT id,n FROM eof_rows ORDER BY id"),
                  ["id", "n"], [[1, 1]])
        restarted.shutdown()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds per pipe operation")
    parser.add_argument("--tmp-root", type=Path, help="existing parent for temporary databases")
    parser.add_argument("--case", help="substring selecting test methods")
    parser.add_argument("--robustness", action="store_true", help="also run optional invalid-input/EOF checks")
    parser.add_argument("executable", type=Path)
    parser.add_argument("executable_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    executable = args.executable.resolve(strict=True)
    if not executable.is_file():
        parser.error("executable must be a file")
    if args.tmp_root is not None:
        args.tmp_root = args.tmp_root.resolve(strict=True)
        if not args.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    ExecutableCase.argv = [str(executable), *args.executable_args]
    ExecutableCase.timeout = args.timeout
    ExecutableCase.tmp_root = args.tmp_root
    classes = [JsonProtocolContract, LegacyTextCompatibility]
    if args.robustness:
        classes.append(OptionalRobustness)
    suite = unittest.TestSuite()
    for cls in classes:
        for name in unittest.defaultTestLoader.getTestCaseNames(cls):
            if not args.case or args.case in name:
                suite.addTest(cls(name))
    if not suite.countTestCases():
        parser.error("--case selected no tests")
    print("Mandatory: official valid protocol and requested SQL/text regressions. "
          + ("Optional robustness: enabled." if args.robustness else "Optional robustness: disabled."),
          flush=True)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
