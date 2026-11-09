#!/usr/bin/env python3
"""Regression tests for the public assertion adapter, not the SQL engine."""
from __future__ import annotations

import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run_public


class PublicHarnessTests(unittest.TestCase):
    def check_script(
        self, sql, expectations, sessions, *, transform=None, returncodes=None,
        statements=None,
    ):
        with tempfile.TemporaryDirectory(prefix="mvd-harness-") as directory:
            root = Path(directory)
            (root / "probe.test").write_text(sql + "\n", encoding="utf-8")
            (root / "probe.result").write_text(
                json.dumps(expectations), encoding="utf-8"
            )
            invocation = 0

            def respond(command, **options):
                nonlocal invocation
                marker = next(
                    arg.split("=", 1)[1] for arg in command
                    if arg.startswith("--ready-marker=")
                )
                output = ""
                for response in sessions[invocation]:
                    output += response
                    if response and not response.endswith("\n"):
                        output += "\n"
                    output += marker + "\n"
                if transform is not None:
                    output = transform(output, marker)
                code = 0 if returncodes is None else returncodes[invocation]
                invocation += 1
                return subprocess.CompletedProcess(command, code, output, "injected stderr")

            case = {"id": "probe"}
            if statements is not None:
                case["statements"] = statements
            with patch.object(run_public.subprocess, "run", side_effect=respond) as runner, \
                 patch.object(run_public.tempfile, "mkdtemp", return_value=str(root / "db")):
                failures = run_public.run_case(Path("unused"), case, root)
                return failures, runner.call_args_list

    def check_response(self, sql, expectation, output):
        failures, _ = self.check_script(sql, [expectation], [[output]])
        return failures

    def test_empty_expectation_does_not_hide_control_error(self):
        for sql in ("BEGIN WORK;", "COMMIT WORK;", "ROLLBACK WORK;",
                    "SET vector_mode=ann;", "CREATE TABLE t(id BIGINT PRIMARY KEY);"):
            with self.subTest(sql=sql):
                self.assertTrue(self.check_response(sql, {}, "ERROR UNSUPPORTED: injected\n"))
                self.assertFalse(self.check_response(sql, {}, "OK\n"))

    def test_column_names_are_compared_even_for_empty_results(self):
        expected = {"columns": ["id"], "rows": []}
        self.assertTrue(self.check_response("SELECT id FROM t;", expected, "COLUMNS\twrong\n"))
        self.assertFalse(self.check_response("SELECT id FROM t;", expected, "COLUMNS\tid\n"))

    def test_affected_rows_and_rows_are_not_ignored(self):
        self.assertTrue(self.check_response("DELETE FROM t;", {"affected_rows": 2}, "OK 1\n"))
        self.assertTrue(self.check_response(
            "SELECT id FROM t;", {"columns": ["id"], "rows": [[2]]}, "COLUMNS\tid\n1\n"
        ))

    def test_empty_expectation_requires_success_for_queries_and_dml(self):
        self.assertFalse(self.check_response("INSERT INTO t VALUES (1);", {}, "OK 1"))
        self.assertFalse(self.check_response("SELECT id FROM t;", {}, "COLUMNS\tid\n1"))
        self.assertTrue(self.check_response("SELECT id FROM t;", {}, "ERROR MISSING: t"))
        self.assertTrue(self.check_response("SELECT id FROM t;", {}, ""))
        self.assertTrue(self.check_response("INSERT INTO t VALUES (1);", {}, ""))

    def test_expected_errors_accept_nonempty_unfrozen_codes(self):
        for code in ("DUPLICATE_KEY", "UNSUPPORTED", "1062", "VENDOR_CODE"):
            with self.subTest(code=code):
                self.assertFalse(self.check_response(
                    "INSERT INTO t VALUES (1);", {"error": "duplicate_key"},
                    f"ERROR {code}: injected",
                ))
        self.assertFalse(self.check_response(
            "SELECT missing FROM t;", {"error": "unknown_column"},
            "ERROR COLUMN_NOT_FOUND: missing",
        ))

    def test_expected_errors_reject_success_empty_code_and_malformed_records(self):
        for output in ("OK", "OK 1", "", "ERROR", "ERROR : injected",
                       "ERROR   : injected", "ERROR CODE without colon",
                       "ERROR CODE: injected\nOK", "COLUMNS\tid"):
            with self.subTest(output=output):
                self.assertTrue(self.check_response(
                    "INSERT INTO t VALUES (1);", {"error": "duplicate_key"}, output,
                ))

    def test_expected_error_does_not_stop_following_sql(self):
        failures, calls = self.check_script(
            "INSERT INTO t VALUES (1);\nSELECT id FROM t;\nCOMMIT;",
            [{"error": "duplicate_key"}, {"columns": ["id"], "rows": [[1]]}, {}],
            [["ERROR DUPLICATE_KEY: injected", "COLUMNS\tid\n1", "OK"]],
        )
        self.assertEqual(failures, [])
        self.assertIn("COMMIT;\n", calls[0].kwargs["input"])
        self.assertFalse(calls[0].kwargs["check"])

    def test_unexpected_error_is_reported_without_desynchronizing_next_sql(self):
        failures, _ = self.check_script(
            "SELECT id FROM missing;\nSELECT id FROM t;\nCOMMIT;",
            [{}, {"columns": ["id"], "rows": [[1]]}, {}],
            [["ERROR TABLE_NOT_FOUND: missing", "COLUMNS\tid\n1", "OK"]],
        )
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("statement 1", failures[0])

    def test_restart_sends_eof_and_reuses_one_directory(self):
        query = {"columns": ["id"], "rows": [[1]]}
        failures, calls = self.check_script(
            "CREATE TABLE t(id BIGINT PRIMARY KEY);\nBEGIN;\nDELETE FROM t;\n"
            "--restart\nSELECT id FROM t;\n--restart\nSELECT id FROM t;",
            [{}, {}, {"affected_rows": 1}, query, query],
            [["OK", "OK", "OK 1"], ["COLUMNS\tid\n1"], ["COLUMNS\tid\n1"]],
            statements=5,
        )
        self.assertEqual(failures, [])
        self.assertEqual(len(calls), 3)
        data_dirs = []
        for call in calls:
            command = call.args[0]
            data_dirs.append(next(arg for arg in command if arg.startswith("--data-dir=")))
            self.assertIn("--column-header", command)
            self.assertIn("--vector-mode=exact", command)
            self.assertNotIn("--restart", call.kwargs["input"])
            self.assertEqual(call.kwargs["timeout"], 30)
        self.assertEqual(len(set(data_dirs)), 1)
        self.assertTrue(calls[0].kwargs["input"].endswith("DELETE FROM t;\n"))
        self.assertEqual(calls[1].kwargs["input"], "SELECT id FROM t;\n")

    def test_restart_does_not_ignore_failed_shutdown(self):
        with self.assertRaisesRegex(AssertionError, "database exited 7"):
            self.check_script(
                "BEGIN;\n--restart\nCOMMIT;", [{}, {}], [["OK"]],
                returncodes=[7],
            )

    def test_leading_consecutive_and_trailing_restarts(self):
        failures, calls = self.check_script(
            "--restart\n--restart\nBEGIN;\n--restart", [{}],
            [[], [], ["OK"], []],
        )
        self.assertEqual(failures, [])
        self.assertEqual([call.kwargs["input"] for call in calls],
                         ["", "", "BEGIN;\n", ""])

    def test_mode_directives_do_not_consume_assertions_and_survive_restart(self):
        query = {"columns": ["id"], "rows": [[1]]}
        failures, calls = self.check_script(
            "SELECT id FROM t;\n--mode ann\nSELECT id FROM t;\n--restart\n"
            "SELECT id FROM t;\n--mode exact\nSELECT id FROM t;\n--restart\n"
            "SELECT id FROM t;",
            [query] * 5,
            [["COLUMNS\tid\n1", "OK", "COLUMNS\tid\n1"],
             ["COLUMNS\tid\n1", "OK", "COLUMNS\tid\n1"], ["COLUMNS\tid\n1"]],
            statements=5,
        )
        self.assertEqual(failures, [])
        self.assertIn("--vector-mode=exact", calls[0].args[0])
        self.assertIn("--vector-mode=ann", calls[1].args[0])
        self.assertIn("--vector-mode=exact", calls[2].args[0])
        self.assertEqual(calls[0].kwargs["input"],
                         "SELECT id FROM t;\nSET vector_mode=ann;\nSELECT id FROM t;\n")
        self.assertIn("SET vector_mode=exact;\n", calls[1].kwargs["input"])

    def test_mode_failure_is_checked_without_consuming_sql_assertion(self):
        failures, _ = self.check_script(
            "--mode ann\nSELECT id FROM t;", [{"columns": ["id"], "rows": []}],
            [["ERROR UNSUPPORTED: mode", "COLUMNS\tid"]],
        )
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("directive SET vector_mode=ann;", failures[0])

    def test_unknown_directive_is_rejected(self):
        for directive in ("--mode typo", "--kill", "--restart now"):
            with self.subTest(directive=directive):
                with self.assertRaisesRegex(AssertionError, "unknown driver directive"):
                    self.check_script(directive, [], [])

    def test_sql_and_manifest_assertion_counts_are_checked(self):
        with self.assertRaisesRegex(AssertionError, "test/result length mismatch"):
            self.check_script("BEGIN;\n--restart\nCOMMIT;", [{}], [])
        with self.assertRaisesRegex(AssertionError, "manifest statement count mismatch"):
            self.check_script("BEGIN;", [{}], [], statements=2)

    def test_empty_query_is_framed_separately_from_following_status_and_rows(self):
        failures, _ = self.check_script(
            "SELECT id FROM t;\nDELETE FROM t;\nSELECT id FROM u;",
            [{"columns": ["id"], "rows": []}, {"affected_rows": 0},
             {"columns": ["id"], "rows": [[2]]}],
            [["COLUMNS\tid", "OK 0", "COLUMNS\tid\n2"]],
        )
        self.assertEqual(failures, [])

    def test_extra_or_missing_rows_do_not_shift_following_assertion(self):
        for first_rows, first_output in (([], "COLUMNS\tid\n99"),
                                         ([[1]], "COLUMNS\tid")):
            with self.subTest(rows=first_rows):
                failures, _ = self.check_script(
                    "SELECT id FROM t;\nSELECT id FROM u;",
                    [{"columns": ["id"], "rows": first_rows},
                     {"columns": ["id"], "rows": [[2]]}],
                    [[first_output, "COLUMNS\tid\n2"]],
                )
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("statement 1", failures[0])

    def test_column_order_and_actual_row_width_are_checked(self):
        self.assertTrue(self.check_response(
            "SELECT id,name FROM t;", {"columns": ["id", "name"], "rows": []},
            "COLUMNS\tname\tid",
        ))
        self.assertTrue(self.check_response(
            "SELECT id,name FROM t;", {}, "COLUMNS\tid\tname\n1",
        ))

    def test_empty_string_cell_is_not_confused_with_empty_result(self):
        self.assertFalse(self.check_response(
            "SELECT content FROM t;", {"columns": ["content"], "rows": [[""]]},
            "COLUMNS\tcontent\n\n",
        ))

    def test_unordered_comparison_preserves_duplicate_rows(self):
        expected = {"columns": ["id"], "rows": [[1], [1], [2]], "unordered": True}
        self.assertFalse(self.check_response(
            "SELECT id FROM t;", expected, "COLUMNS\tid\n2\n1\n1",
        ))
        self.assertTrue(self.check_response(
            "SELECT id FROM t;", expected, "COLUMNS\tid\n2\n1\n2",
        ))

    def test_bigint_unicode_and_float_tolerance_are_preserved(self):
        expected = {"columns": ["id", "name", "d"],
                    "rows": [[9007199254740993, "中文", 1.0]]}
        self.assertFalse(self.check_response(
            "SELECT id,name,d FROM t;", expected,
            "COLUMNS\tid\tname\td\n9007199254740993\t中文\t1.000005",
        ))
        self.assertTrue(self.check_response(
            "SELECT id,name,d FROM t;", expected,
            "COLUMNS\tid\tname\td\n9007199254740992\t中文\t1.0",
        ))

    def test_missing_marker_and_unframed_output_are_failures(self):
        for transform in (lambda output, marker: output.replace(marker + "\n", ""),
                          lambda output, marker: output + "unexpected output\n"):
            failures, _ = self.check_script(
                "BEGIN;", [{}], [["OK"]], transform=transform,
            )
            self.assertTrue(failures)
            self.assertTrue(any("without a ready marker" in failure for failure in failures))

    def test_extra_missing_and_empty_frames_are_failures(self):
        for frames in (["OK", "OK"], [], [""]):
            with self.subTest(frames=frames):
                failures, _ = self.check_script("BEGIN;", [{}], [frames])
                self.assertTrue(failures)

    def test_main_runs_remaining_cases_after_crash_or_timeout(self):
        with tempfile.TemporaryDirectory(prefix="mvd-harness-main-") as directory:
            root = Path(directory)
            manifest = {"cases": [{"id": "crash"}, {"id": "healthy"}, {"id": "timeout"}]}
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            output = io.StringIO()
            with patch("sys.argv", ["run_public.py", "unused", "--case-dir", str(root / "cases")]), \
                 patch.object(run_public, "run_case", side_effect=[
                     AssertionError("database exited 7"), [],
                     subprocess.TimeoutExpired("unused", 30),
                 ]) as runner, patch("sys.stdout", output):
                self.assertEqual(run_public.main(), 1)
            self.assertEqual(runner.call_count, 3)
            self.assertIn("FAIL crash", output.getvalue())
            self.assertIn("PASS healthy", output.getvalue())
            self.assertIn("FAIL timeout", output.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
