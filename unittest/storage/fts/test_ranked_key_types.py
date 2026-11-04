#!/usr/bin/env python3
"""Compare complete MATCH keys and scores across advance_to key representations.

Requires PyMySQL and a dedicated instance on port 2891. Creates/drops only
fts_ranked_key_types; no mutation or transaction scenarios are repeated here.
Use --record NEW_PATH only against the independently verified, unmodified
upstream/2026-competition binary at
5bce9e06d1281347ad61344e4b01fab9158c58a2. Never record on a candidate.
The default fixture is ranked_key_types_expected.json.gz beside this script.
The caller verifies the binary's provenance; SQL version strings cannot do so.
"""
import argparse
import gzip
import json
import math
import os
from pathlib import Path

import pymysql

DATABASE = "fts_ranked_key_types"
DOCUMENT_COUNT = 1537
QUERIES = (
    ("database", "natural"),
    ("database audit", "natural"),
    ("database audit tokenizer", "natural"),
    ("missingword database audit tokenizer", "natural"),
    ("tokenizer", "natural"),
    ("phasealpha phasegamma", "natural"),
    ("+database +tokenizer", "boolean"),
    ("+database -audit", "boolean"),
)


def corpus():
    # Include the integer-key transitions in sparse streams as well as dense
    # ones. The large gaps require seeks within and across retrieval batches.
    sparse_edges = {1, 2, 511, 512, 513, 768, 769, 770,
                    1024, 1025, 1535, 1536, 1537}
    bodies = []
    for number in range(1, DOCUMENT_COUNT + 1):
        terms = ["background"] * (1 + number % 9)
        for term, selected, frequency in (
            ("database", number % 3 != 0, 1 + number % 13),
            ("audit", number % 5 != 0, 1 + number % 7),
            ("tokenizer", number % 97 == 0 or number in sparse_edges,
             1 + number % 3),
            ("phasealpha", number <= 512, 1),
            ("phasebeta", 512 < number <= 1024, 2),
            ("phasegamma", number > 1024, 3),
        ):
            if selected:
                terms.extend([term] * frequency)
        bodies.append(" ".join(terms))
    return bodies


def key_tables():
    # Three sorted runs, including the exact UInt64 high-bit transition and
    # maximum, and both Int64 extrema with a run spanning -1, 0 and +1.
    unsigned = (list(range(512))
                + list(range((1 << 63) - 256, (1 << 63) + 256))
                + list(range((1 << 64) - 513, 1 << 64)))
    signed = (list(range(-(1 << 63), -(1 << 63) + 512))
              + list(range(-256, 256))
              + list(range((1 << 63) - 513, 1 << 63)))
    # Uniqueness precedes the optional trailing spaces. Do not rely on
    # VARBINARY distinguishing primary keys that differ only in trailing space.
    binary = [b"\x00id" + index.to_bytes(2, "big") + b"\x00"
              + b" " * (index % 3) for index in range(DOCUMENT_COUNT)]
    yield ("unsigned_docs", "id BIGINT UNSIGNED NOT NULL", ("id",),
           ("CAST(id AS CHAR)",), [(key,) for key in unsigned])
    yield ("signed_docs", "id BIGINT NOT NULL", ("id",),
           ("CAST(id AS CHAR)",), [(key,) for key in signed])
    yield ("binary_docs", "id VARBINARY(16) NOT NULL", ("id",),
           ("CAST(HEX(id) AS CHAR)",), [(key,) for key in binary])
    # Repeat the leading UInt64 key and use parts differing only by trailing
    # space. seq preserves uniqueness even under PAD SPACE comparison. Every
    # component is returned and used as a ranking tie breaker.
    composite = [(unsigned[index - index % 3],
                  b"\x00part\x00" + b" " * (index % 3), index % 3)
                 for index in range(DOCUMENT_COUNT)]
    yield ("composite_docs",
           "id BIGINT UNSIGNED NOT NULL,part VARBINARY(16) NOT NULL,"
           "seq INT NOT NULL", ("id", "part", "seq"),
           ("CAST(id AS CHAR)", "CAST(HEX(part) AS CHAR)",
            "CAST(seq AS CHAR)"), composite)


def result_row(row):
    key = list(row[:-1])
    if not all(isinstance(component, str) for component in key):
        raise TypeError("Expected string/HEX primary key: " + repr(key))
    score = float(row[-1])
    if not math.isfinite(score):
        raise AssertionError(("Non-finite score", key, score))
    return {"key": key, "score": score}


def capture(options):
    result = {"format": 1, "documents_per_table": DOCUMENT_COUNT, "cases": []}
    bodies = corpus()
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            try:
                cursor.execute("USE " + DATABASE)
                for table, columns, key_columns, projections, keys in key_tables():
                    key_order = ",".join(key_columns)
                    cursor.execute("CREATE TABLE " + table + "(" + columns
                                   + ",body TEXT,PRIMARY KEY(" + key_order + "))")
                    placeholders = ",".join(["%s"] * (len(key_columns) + 1))
                    cursor.executemany("INSERT INTO " + table + " VALUES("
                                       + placeholders + ")",
                                       [key + (body,) for key, body in zip(keys, bodies)])
                    cursor.execute("ALTER TABLE " + table
                                   + " ADD FULLTEXT INDEX ft(body) WITH PARSER beng")
                    ranked = " ORDER BY score DESC," + key_order
                    orders = (
                        " ORDER BY " + key_order,
                        ranked,
                        ranked + " LIMIT 1",
                        ranked + " LIMIT 20",
                        # Both pages span the 512/1024 boundaries, including
                        # the last few results of a 1,025-hit posting stream.
                        ranked + " LIMIT 509,517",
                        ranked + " LIMIT 1023,514",
                    )
                    for query, mode in QUERIES:
                        sql_mode = " IN BOOLEAN MODE" if mode == "boolean" else ""
                        match = "MATCH(body) AGAINST(%s" + sql_mode + ")"
                        # ORDER BY names the raw PK columns, never their
                        # displayed string casts (which would sort lexically).
                        sql = ("SELECT " + ",".join(projections) + "," + match
                               + " AS score FROM " + table + " WHERE " + match)
                        for order in orders:
                            cursor.execute(sql + order, (query, query))
                            result["cases"].append({
                                "table": table,
                                "key_columns": list(key_columns),
                                "query": query,
                                "mode": mode,
                                "order": order.strip(),
                                "rows": [result_row(row) for row in cursor.fetchall()],
                            })
            finally:
                cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
    return result


def compare_score(expected, actual, path):
    if (type(expected) not in (int, float) or type(actual) not in (int, float)
            or not math.isfinite(expected) or not math.isfinite(actual)
            or not math.isclose(expected, actual, rel_tol=1e-12, abs_tol=1e-12)):
        raise AssertionError((path, expected, actual))


def compare(expected, actual, path="root"):
    # Only score fields get tolerance. Keys, metadata, types, lengths and row
    # order must match exactly, including every byte represented by HEX.
    if type(expected) is not type(actual):
        raise AssertionError((path, type(expected).__name__, type(actual).__name__))
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError((path, list(expected), list(actual)))
        for key in expected:
            child_path = path + "." + key
            if key == "score":
                compare_score(expected[key], actual[key], child_path)
            else:
                compare(expected[key], actual[key], child_path)
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
    parser.add_argument("--port", type=int, choices=(2891,), default=2891)
    parser.add_argument("--user", default="root")
    parser.add_argument("--record", type=Path, metavar="NEW_PATH",
                        help="Record all rows only from the verified official original; "
                             "refuse to overwrite any existing path.")
    parser.add_argument("--expected", type=Path,
                        default=Path(__file__).with_name("ranked_key_types_expected.json.gz"))
    args = parser.parse_args()
    if args.record is not None:
        # Fail before touching SQL on an accidental reuse, and use exclusive
        # creation below to protect against races as well as existing files.
        if os.path.lexists(args.record):
            parser.error("Refusing to overwrite recorded results: " + str(args.record))
        if not args.record.parent.is_dir():
            parser.error("Recording parent directory does not exist: "
                         + str(args.record.parent))
    else:
        # Missing or invalid expected results fail before creating a database.
        expected = json.loads(gzip.decompress(args.expected.read_bytes()))
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                          charset="utf8mb4", autocommit=True,
                          connect_timeout=10, read_timeout=180, write_timeout=180))
    if args.record is not None:
        payload = (json.dumps(actual, ensure_ascii=True, allow_nan=False,
                              separators=(",", ":")) + "\n").encode("utf-8")
        compressed = gzip.compress(payload, mtime=0)
        with args.record.open("xb") as output:
            output.write(compressed)
        outcome = "RECORDED complete official-original fixture at " + str(args.record)
    else:
        compare(expected, actual)
        outcome = "PASS complete ordered keys and scores"
    print(outcome, "cases=", len(actual["cases"]),
          "result_rows=", sum(len(case["rows"]) for case in actual["cases"]))


if __name__ == "__main__":
    main()
