#!/usr/bin/env python3
"""Focused string-decoding regressions against an existing JSONL executable.

Run: python -X utf8 -B tests/test_string_fast_paths.py /path/to/minivectordb
Uses the bounded JSONL client and assertions from test_json_protocol. Invalid
input checks preserve local recovery semantics, not an official error-code map.
No compiler, benchmarks, external packages, or implementation hooks are needed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import unittest

from jsonl_client import same_json
from test_json_protocol import ExecutableCase, sql_string


def json_string(text):
    return json.dumps(text, ensure_ascii=False).encode("utf-8")


def escaped_json_string(text):
    """Encode every UTF-16 unit, including ASCII and surrogate pairs."""
    units = text.encode("utf-16-be")
    return b'"' + b"".join(
        f"\\u{int.from_bytes(units[i:i + 2], 'big'):04x}".encode("ascii")
        for i in range(0, len(units), 2)
    ) + b'"'


class StringFastPaths(ExecutableCase):
    def create(self, client):
        self.success(client.execute(
            "CREATE TABLE strings(id BIGINT PRIMARY KEY,s VARCHAR(8192))"
        ))

    def raw(self, client, payload):
        # Each malformed fixture is still exactly one physical JSONL request.
        self.assertNotIn(b"\n", payload)
        client.send_bytes(payload + b"\n")
        return client.read_message()

    def assert_contents(self, client, wanted):
        self.rows(client.execute("SELECT id,s FROM strings ORDER BY id"),
                  ["id", "s"], wanted)

    def reject_without_mutation(self, client, payload):
        self.failure(self.raw(client, payload))
        # Also catches stale parser state, duplicate replies and lost flushing.
        self.assert_contents(client, [[0, "keep"]])

    def test_sql_plain_literals_own_text_and_consume_exact_quote(self):
        client = self.client()
        self.create(client)
        cases = []
        for quote in ("'", '"'):
            opposite = '"' if quote == "'" else "'"
            values = [("Ab09_ " * (length // 6 + 1))[:length]
                      for length in (0, 1, 15, 16, 63, 64, 4097)]
            values += [";,:()[]{} -- # /* delimiter */ " + opposite,
                       "ascii-" * 32 + "é中文🌊e\u0301\u2028\u2029"]
            cases.extend((quote + value + quote, value) for value in values)
        # Multiple tokens in one statement detect borrowed/reused token storage.
        sql = "INSERT INTO strings VALUES" + ",".join(
            f"({index},{literal})" for index, (literal, _) in enumerate(cases)
        )
        self.affected(client.execute(sql), len(cases))
        self.assert_contents(client, [[i, value] for i, (_, value) in enumerate(cases)])
        for index, (literal, value) in enumerate(cases):
            with self.subTest(index=index, length=len(value)):
                self.rows(client.execute(
                    f"SELECT id,s FROM strings WHERE s={literal} AND id={index}"
                ), ["id", "s"], [[index, value]])
        client.shutdown()

    def test_sql_escape_fallback_preserves_prefix_and_suffix(self):
        client = self.client()
        self.create(client)
        cases = []
        for quote in ("'", '"'):
            for length in (0, 63, 4097):
                prefix = "p" * length
                bodies = [
                    (quote * 2, quote),
                    ("\\" + quote, quote),
                    (r"\n\r\t\\\q\0", "\n\r\t\\q0"),
                    (r"\'\"", "'\""),
                ]
                for encoded, decoded in bodies:
                    cases.append((quote + prefix + encoded + "tail" + quote,
                                  prefix + decoded + "tail"))
                # Escapes at the closing boundary must leave the next comma intact.
                cases.append((quote + prefix + "\\\\" + quote, prefix + "\\"))
                cases.append((quote + prefix + quote * 3, prefix + quote))
        self.affected(client.execute("INSERT INTO strings VALUES" + ",".join(
            f"({index},{literal})" for index, (literal, _) in enumerate(cases)
        )), len(cases))
        self.assert_contents(client, [[i, value] for i, (_, value) in enumerate(cases)])
        client.shutdown()

    def test_sql_unterminated_literals_fail_atomically_and_recover(self):
        client = self.client()
        self.create(client)
        self.affected(client.execute("INSERT INTO strings VALUES(0,'keep')"), 1)
        for quote in ("'", '"'):
            prefix = "ordinary" * 513
            endings = ("", prefix, prefix + "\\", prefix + "\\" + quote,
                       prefix + quote * 2, prefix + r"\n")
            for index, ending in enumerate(endings):
                with self.subTest(quote=quote, ending=index):
                    # No final quote/parenthesis: JSON framing must still respond.
                    sql = "INSERT INTO strings VALUES(1,'would-leak'),(2," + quote + ending
                    self.failure(client.execute(sql))
                    self.assert_contents(client, [[0, "keep"]])
        client.shutdown()

    def test_json_plain_and_unicode_escaped_strings_are_equivalent(self):
        client = self.client()
        self.create(client)
        values = ["", "Aa09 ;,:()[]{}#/-_ " * 241,
                  "ascii-prefix-" * 32 + "é中文🌊e\u0301\u2028\u2029",
                  "\x7f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"]
        self.affected(client.execute("INSERT INTO strings VALUES" + ",".join(
            f"({index},{sql_string(value)})" for index, value in enumerate(values)
        )), len(values))
        for index, value in enumerate(values):
            sql = f"SELECT id,s FROM strings WHERE s={sql_string(value)} AND id={index}"
            fields = [("id", value), ("op", "execute"), ("sql", sql),
                      ("search_mode", "exact")]
            for encode in (json_string, escaped_json_string):
                with self.subTest(index=index, encoding=encode.__name__):
                    # Exercise decoded keys, op/mode values, SQL and ID validation.
                    payload = b"{" + b",".join(
                        encode(key) + b":" + encode(item) for key, item in fields
                    ) + b"}"
                    response = self.raw(client, payload)
                    self.assertTrue(same_json(response.get("id"), value))
                    self.rows(response, ["id", "s"], [[index, value]])
        client.shutdown()

    def test_json_escape_fallback_preserves_controls_and_long_prefix(self):
        client = self.client()
        self.create(client)
        value = "prefix-" * 585 + '"\\/\b\f\n\r\t\x00\x1f-tail'
        # Keep actual controls in the SQL literal so JSON, not SQL escapes,
        # performs their decoding. Only SQL quote/backslash syntax is escaped.
        literal = "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"
        sql = f"INSERT INTO strings VALUES(1,{literal})"
        payload = (b'{"id":' + json_string(value) + b',"op":"execute","sql":'
                   + json_string(sql) + b"}").replace(b"/", b"\\/")
        response = self.raw(client, payload)
        self.assertTrue(same_json(response.get("id"), value))
        self.affected(response, 1)
        self.assert_contents(client, [[1, value]])
        client.shutdown()

    def test_json_duplicate_decoded_keys_are_rejected_before_execution(self):
        client = self.client()
        self.create(client)
        self.affected(client.execute("INSERT INTO strings VALUES(0,'keep')"), 1)
        sql = json_string("INSERT INTO strings VALUES(1,'would-leak')")
        base = b'{"id":7,"op":"execute","sql":' + sql
        long_key = "prefix" * 32 + "é🌊"
        duplicate = json_string(long_key) + b':1,' + escaped_json_string(long_key) + b':2'
        fixtures = [
            base + b',"s\\u0071l":' + sql + b"}",
            b'{"id":7,"op":"execute","s\\u0071l":' + sql + b',"sql":' + sql + b"}",
            b'{"op":"execute","sql":' + sql + b',"id":{' + duplicate + b"}}",
            base + b',"metadata":[{' + duplicate + b"}]}",
        ]
        for index, payload in enumerate(fixtures):
            with self.subTest(index=index):
                self.reject_without_mutation(client, payload)
        client.shutdown()

    def test_json_invalid_and_truncated_strings_reject_and_recover(self):
        client = self.client()
        self.create(client)
        self.affected(client.execute("INSERT INTO strings VALUES(0,'keep')"), 1)
        base = (b'{"id":7,"op":"execute","sql":'
                + json_string("INSERT INTO strings VALUES(1,'would-leak')")
                + b',"metadata":"' + b"ordinary" * 17)
        # Unknown fields must still validate strings. LF itself is a JSONL
        # delimiter; the controls below remain inside a single physical frame.
        invalid = [(f"control-{byte:02x}", bytes([byte]))
                   for byte in (0, 1, 8, 9, 11, 12, 13, 26, 31)]
        invalid += [
            ("unknown-escape", br"\x41"),
            ("bad-hex", br"\u12G4"),
            ("lone-high-surrogate", br"\ud800"),
            ("lone-low-surrogate", br"\udc00"),
            ("wrong-low-surrogate", br"\ud800\u0041"),
            ("reversed-surrogates", br"\udc00\ud800"),
            ("utf8-continuation", b"\x80"),
            ("utf8-overlong", b"\xc0\xaf"),
            ("utf8-surrogate", b"\xed\xa0\x80"),
            ("utf8-out-of-range", b"\xf4\x90\x80\x80"),
            ("utf8-bad-continuation", b"\xe2\x28\xa1"),
            ("utf8-truncated", b"\xe2\x82"),
        ]
        for label, suffix in invalid:
            with self.subTest(label=label):
                self.reject_without_mutation(client, base + suffix + b'"}')
        for suffix in (b"", b"\\", br"\u", br"\u123", br"\ud800\u", br"\ud800\udc0"):
            with self.subTest(truncated=suffix):
                self.reject_without_mutation(client, base + suffix)
        client.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path)
    parser.add_argument("--case", help="substring selecting test methods")
    args = parser.parse_args()
    StringFastPaths.argv = [str(args.executable.resolve(strict=True))]
    if args.tmp_root is not None:
        StringFastPaths.tmp_root = args.tmp_root.resolve(strict=True)
        if not StringFastPaths.tmp_root.is_dir():
            parser.error("--tmp-root must be an existing directory")
    names = unittest.defaultTestLoader.getTestCaseNames(StringFastPaths)
    names = [name for name in names if not args.case or args.case in name]
    if not names:
        parser.error("--case selected no tests")
    suite = unittest.TestSuite(StringFastPaths(name) for name in names)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
