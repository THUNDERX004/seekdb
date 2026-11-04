#!/usr/bin/env python3
"""Compare complete case-copy TOKENIZE results with the official original.

Requires PyMySQL. Executes only SELECT/SHOW and session SET statements.
Use --record NEW_PATH only on the independently verified, unmodified
upstream/2026-competition binary at
5bce9e06d1281347ad61344e4b01fab9158c58a2; never record on a candidate.
Recording is explicit and refuses existing paths, including dangling symlinks.
The default fixture is casecopy_expected.json.gz beside this script.
The caller verifies binary provenance; SQL version strings cannot prove it.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re

import pymysql


UTF8_COLLATIONS = ("utf8mb4_bin", "utf8mb4_general_ci", "utf8mb4_unicode_ci")
PARSERS = (("beng", None), ("ik", "smart"), ("ik", "max_word"))
OUTPUTS = ("default", "all")
STRICT_MODE = "STRICT_ALL_TABLES"
CASE_TABLE = (Path(__file__).resolve().parents[3]
              / "src/oblib/lib/charset/ob_ctype_utf8_tab.h")


def default_case_changes():
    # Use the engine's table, not Python's version-dependent upper()/lower().
    source = CASE_TABLE.read_text(encoding="utf-8")
    registry = re.search(
        r"ob_unicase_pages_default\[256\]\s*=\s*\{(.*?)\};", source, re.S)
    default = re.search(
        r"ObUnicaseInfo\s+ob_unicase_default\s*=\s*"
        r"\{\s*0xFFFF\s*,\s*ob_unicase_pages_default\s*\};", source)
    if registry is None or default is None:
        raise ValueError("Unsupported default case-table declaration")
    pages = [name.strip() for name in registry.group(1).split(",")
             if name.strip()]
    if len(pages) != 256:
        raise ValueError("Expected 256 default case-table pages")
    changes = []
    for page_number, name in enumerate(pages):
        if name == "NULL":
            continue
        table = re.search(r"ObUnicaseInfoChar\s+" + re.escape(name)
                          + r"\[\]\s*=\s*\{(.*?)\};", source, re.S)
        if table is None:
            raise ValueError("Missing case-table page: " + name)
        entries = re.findall(
            r"\{\s*0x([0-9a-f]+)\s*,\s*0x([0-9a-f]+)\s*,"
            r"\s*0x([0-9a-f]+)\s*\}", table.group(1), re.I)
        if len(entries) != 256:
            raise ValueError("Expected 256 entries in " + name)
        for offset, (upper, lower, _) in enumerate(entries):
            point = page_number * 256 + offset
            upper, lower = int(upper, 16), int(lower, 16)
            if upper != point or lower != point:
                if any(not scalar(value) for value in (point, upper, lower)):
                    raise ValueError("Non-scalar case mapping")
                changes.append([point, upper, lower])
    if not changes:
        raise ValueError("No default case changes found")
    return changes


def scalar(point):
    return 0 <= point <= 0x10FFFF and not 0xD800 <= point <= 0xDFFF


def unicode_documents(changes):
    base = [
        ("null", None),
        ("empty", ""),
        ("ascii", "The quick BROWN fox HELLO hello lower 123 xyz"),
        ("nul-only", "\0\0\0"),
        ("nul-edges", "\0HELLO\0World\0"),
        ("controls", "ABC\0DEF\tGHI\nJKL\rMNO\x01PQR\x7fEND"),
        ("delimiters", " ,.!?--_ +#@& ABC café 数据库 \t\n"),
        ("case-equivalent", "CAFÉ café CAFE cafe ÉÉÉ ééé EEE eee"),
        ("composed", "CAFÉ Cafe\u0301 A\u030a \u00c5 \u212b Straße STRASSE"),
        ("cjk", "数据库 全文索引 中英文 OceanBase ＡＢＣ１２３ abc123"),
        ("supplementary", "ABC😀DEF 𠀀中文乁 ABC\U00010400\U00010428DEF"),
        ("short-tokens", "a an THE i xy XYZ abc ABC"),
        ("length-edges", " ".join("X" * length for length in (1, 2, 3, 83, 84, 85))),
        ("long-token", "A" * 4097 + " HELLO end"),
        ("repeated", "HELLO hello CAFÉ café 数据库 " * 40),
    ]
    for label, text in base:
        yield label, text, UTF8_COLLATIONS, True

    # BENG keeps these characters inside an ASCII-delimited alphanumeric token
    # even when IK classifies the character itself as a delimiter.
    for point, upper, lower in changes:
        char = chr(point)
        text = ("Ab" + char + "Zz " + char * 3 + " Ab" + chr(upper)
                + chr(lower) + "Zz 中" + char + "国")
        yield "case/U+" + format(point, "04X"), text, ("utf8mb4_bin",), False

    # All mappings also participate in grouping under both CI collations.
    for start in range(0, len(changes), 32):
        text = " ".join("Ab" + chr(point) + "Zz Ab" + chr(lower) + "Zz"
                        for point, _, lower in changes[start:start + 32])
        yield "case-batch/" + str(start), text, UTF8_COLLATIONS, False

    shrinking = [(point, lower) for point, _, lower in changes
                 if len(chr(lower).encode("utf-8")) < len(chr(point).encode("utf-8"))]
    for point, lower in shrinking:
        char = chr(point)
        for suffix, text in (
            ("middle", "Ab" + char + "CD" + char + "EF " + chr(lower) * 3),
            ("repeat", "Ab" + char * 24 + "Zz " + "Ab" + chr(lower) * 24 + "Zz"),
            ("nul", "Ab" + char + "\0CD" + char + "EF\0END"),
        ):
            yield "shrink/U+" + format(point, "04X") + "/" + suffix, text, UTF8_COLLATIONS, False

    # Encoding-width transitions, default-table page edges, Unicode block
    # boundaries used by IK, surrogates' valid neighbours, and every plane end.
    edges = {
        0, 0x20, 0x30, 0x39, 0x41, 0x5A, 0x61, 0x7A, 0x7F, 0x80,
        0x7FF, 0x800, 0x10FF, 0x1100, 0x11FF, 0x1200,
        0x3000, 0x3040, 0x309F, 0x30A0, 0x30FF, 0x3130, 0x318F,
        0x31F0, 0x31FF, 0x3400, 0x4DBF, 0x4E00, 0x9FFF,
        0xAC00, 0xD7AF, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDFFF,
        0xE000, 0xF900, 0xFAFF, 0xFDD0, 0xFDEF, 0xFEFF,
        0xFF00, 0xFF10, 0xFF19, 0xFF21, 0xFF3A, 0xFF41, 0xFF5A,
        0xFF5F, 0xFFEF, 0xFFFD, 0xFFFF, 0x10000,
        0x10400, 0x10428, 0x104B0, 0x104D8, 0x1E900, 0x1E922,
        0x1F600, 0x20000, 0x2FA1F, 0x10FFFF,
    }
    for point, _, _ in changes:
        edges.update((point & ~255, (point & ~255) + 255))
    for plane in range(17):
        edges.update(((plane << 16) + 0xFFFE, (plane << 16) + 0xFFFF))
    points = sorted({point + delta for point in edges for delta in (-1, 0, 1)
                     if scalar(point + delta)})
    for point in points:
        char = chr(point)
        yield ("boundary/U+" + format(point, "04X"),
               "Ab" + char + "Zz " + char * 3 + " 中" + char + "国",
               UTF8_COLLATIONS, False)


def fallback_documents():
    sources = (
        ("latin1", "latin-1", ("latin1_bin", "latin1_swedish_ci"),
         ["", "Hello WORLD cafe CAFÉ ÄÖÜ ß", "ABC\0DEF\0END",
          "Éé" * 40 + " HELLO hello", "a an THE abc ABC"]),
        ("gbk", "gbk", ("gbk_bin", "gbk_chinese_ci"),
         ["", "Hello WORLD 数据库 中文 全文索引", "ＡＢＣ１２３ abcXYZ",
          "甲\0Hello\0中文", "Ωω" * 24 + " HELLO hello"]),
        ("utf16", "utf-16-be", ("utf16_bin", "utf16_general_ci"),
         ["", "Hello WORLD 数据库", "ABC\0DEF\0中文", "İΩKÅ ABC xyz",
          "AbİCDΩEFKGHÅIJ", "ABC😀DEF 𠀀中文", "ＡＢＣ１２３ abcXYZ"]),
    )
    for charset, encoding, collations, texts in sources:
        for index, text in enumerate(texts):
            yield charset + "/" + str(index), text.encode(encoding), charset, collations


def malformed_documents():
    invalid = (
        "80", "bf", "c0af", "c180", "c2", "df", "e0", "e08080",
        "e19f", "e4b8", "e228a1", "eda080", "edbfbf",
        "f0", "f0808080", "f09f98", "f4908080", "f5808080",
        "f888808080", "fc8480808080", "fe", "ff",
    )
    for hex_value in invalid:
        raw = bytes.fromhex(hex_value)
        for label, payload in (
            ("only", raw),
            ("middle", b"ABC" + raw + b"DEF HELLO"),
            ("tail", b"HELLO\0ABC" + raw),
        ):
            yield "invalid/" + hex_value + "/" + label, payload


def json_cell(value):
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError:
            return {"bytes_hex": value.hex()}
    if isinstance(value, str):
        try:
            return {"json": json.loads(value)}
        except ValueError:
            return {"text": value}
    if value is None:
        return None
    raise TypeError("Unexpected TOKENIZE result type: " + type(value).__name__)


def warning_cell(value):
    # Preserve all diagnostic bytes; non-UTF8 fallback results need not be UTF8.
    return {"bytes_hex": value.hex()} if isinstance(value, bytes) else value


def query(cursor, sql, params, allow_error):
    try:
        cursor.execute(sql, params)
        rows = [[json_cell(value) for value in row] for row in cursor.fetchall()]
    except pymysql.MySQLError as error:
        code = error.args[0] if error.args else None
        # Client/transport failures must never become expected SQL errors.
        if (not allow_error or not isinstance(code, int)
                or code <= 0 or 2000 <= code < 3000):
            raise
        return {"error_number": code}
    cursor.execute("SHOW WARNINGS")
    warnings = [[warning_cell(value) for value in row] for row in cursor.fetchall()]
    return {"rows": rows, "warnings": warnings}


def capture(options, changes):
    result = {
        "format": 1,
        "default_case_changes": changes,
        "case_map_sha256": hashlib.sha256(
            json.dumps(changes, separators=(",", ":")).encode("ascii")).hexdigest(),
        "cases": [],
    }
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("SET SESSION sql_mode=%s", (STRICT_MODE,))

            def append(label, payload, charset, collation, parser, ik_mode,
                       output, form="convert", sql_mode=STRICT_MODE, allow_error=False):
                params = []
                if payload is None:
                    expression = "CAST(NULL AS CHAR CHARACTER SET " + charset + ")"
                elif form == "literal":
                    # Hex is generated locally. An introducer avoids a prior
                    # CONVERT sanitizing or rejecting malformed source bytes.
                    expression = "_" + charset + " X'" + payload.hex() + "'"
                else:
                    expression = "CONVERT(UNHEX(%s) USING " + charset + ")"
                    params.append(payload.hex())
                expression += " COLLATE " + collation
                params.append(parser)
                config = [] if output == "implicit" else [{"output": output}]
                if ik_mode is not None:
                    config.append({"additional_args": [{"ik_mode": ik_mode}]})
                sql = "SELECT TOKENIZE(" + expression + ",%s"
                if config:
                    sql += ",%s"
                    params.append(json.dumps(config, separators=(",", ":")))
                sql += ")"
                result["cases"].append({
                    "id": label, "input_hex": None if payload is None else payload.hex(),
                    "charset": charset, "collation": collation,
                    "parser": parser, "ik_mode": ik_mode, "output": output,
                    "input_form": form, "sql_mode": sql_mode,
                    "result": query(cursor, sql, tuple(params), allow_error),
                })

            for label, text, collations, implicit in unicode_documents(changes):
                payload = None if text is None else text.encode("utf-8")
                for collation in collations:
                    for parser, ik_mode in PARSERS:
                        for output in (("implicit",) + OUTPUTS if implicit else OUTPUTS):
                            append(label, payload, "utf8mb4", collation,
                                   parser, ik_mode, output)

            # Minimal builds can reject the charset before reaching either
            # parser. Preserve that original SQL error as part of the fixture.
            for label, payload, charset, collations in fallback_documents():
                for collation in collations:
                    for parser, ik_mode in PARSERS:
                        for output in OUTPUTS:
                            append(label, payload, charset, collation, parser,
                                   ik_mode, output, allow_error=True)

            for sql_mode in (STRICT_MODE, ""):
                cursor.execute("SET SESSION sql_mode=%s", (sql_mode,))
                for label, payload in malformed_documents():
                    for form in ("convert", "literal"):
                        for parser, ik_mode in PARSERS:
                            for output in OUTPUTS:
                                append(label, payload, "utf8mb4", "utf8mb4_bin",
                                       parser, ik_mode, output, form, sql_mode, True)
    return result


def compare(expected, actual, path="root"):
    # Do not sort token arrays, drop frequencies, hash outputs or allow tolerance.
    if type(expected) is not type(actual):
        raise AssertionError((path, type(expected).__name__, type(actual).__name__))
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError((path, list(expected), list(actual)))
        for key in expected:
            compare(expected[key], actual[key], path + "." + key)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise AssertionError((path, len(expected), len(actual)))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "[" + str(index) + "]")
    elif expected != actual:
        raise AssertionError((path, expected, actual))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2891)
    parser.add_argument("--user", default="root")
    parser.add_argument("--record", type=Path, metavar="NEW_PATH",
                        help="Record only from the verified official original; "
                             "refuse to overwrite any existing path.")
    parser.add_argument("--expected", type=Path,
                        default=Path(__file__).with_name("casecopy_expected.json.gz"))
    args = parser.parse_args()
    if args.record is not None:
        if os.path.lexists(args.record):
            parser.error("Refusing to overwrite recorded results: " + str(args.record))
        if not args.record.parent.is_dir():
            parser.error("Recording parent directory does not exist: " + str(args.record.parent))
    else:
        # Missing/corrupt fixtures fail before connecting to the server.
        expected = json.loads(gzip.decompress(args.expected.read_bytes()))
    changes = default_case_changes()
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                          charset="utf8mb4", use_unicode=False, autocommit=True,
                          connect_timeout=10, read_timeout=180, write_timeout=180),
                     changes)
    if args.record is not None:
        payload = (json.dumps(actual, ensure_ascii=True, allow_nan=False,
                              separators=(",", ":")) + "\n").encode("utf-8")
        compressed = gzip.compress(payload, mtime=0)
        with args.record.open("xb") as output:
            output.write(compressed)
        print("RECORDED complete official-original fixture at", args.record)
    else:
        compare(expected, actual)
        print("PASS complete TOKENIZE results, order, frequencies and SQL errors")
    print("cases=", len(actual["cases"]), "default_case_changes=", len(changes),
          "sql_errors=", sum("error_number" in case["result"] for case in actual["cases"]))


if __name__ == "__main__":
    main()
