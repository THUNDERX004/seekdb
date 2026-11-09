#!/usr/bin/env python3
"""Regression tests for the public assertion adapter, not the SQL engine."""
from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run_public


class PublicHarnessTests(unittest.TestCase):
    def check_response(self, sql, expectation, output):
        with tempfile.TemporaryDirectory(prefix="mvd-harness-") as directory:
            root = Path(directory)
            (root / "probe.test").write_text(sql + "\n", encoding="utf-8")
            (root / "probe.result").write_text(
                json.dumps([expectation]), encoding="utf-8"
            )
            response = subprocess.CompletedProcess([], 0, output, "")
            with patch.object(run_public.subprocess, "run", return_value=response), \
                 patch.object(run_public.tempfile, "mkdtemp", return_value=str(root / "db")):
                return run_public.run_case(Path("unused"), {"id": "probe"}, root)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
