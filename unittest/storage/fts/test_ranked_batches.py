#!/usr/bin/env python3
"""Compare ordered MATCH scores and ranked LIMIT results with an original snapshot.

Use --record only on the unmodified competition binary. A dedicated instance
is required; this test creates and drops only fts_ranked_batches.
"""
import argparse
import gzip
import json
import math
import os
from pathlib import Path

import pymysql

DATABASE = "fts_ranked_batches"


def corpus():
    rows = []
    for key in range(1, 3074):
        terms = ["background"] * (1 + key % 9)
        # Dense, sparse and disjoint posting streams cross multiple batches.
        for term, selected, freq in [
            ("database", key % 3 != 0, 1 + key % 13),
            ("audit", key % 5 != 0, 1 + key % 7),
            ("tokenizer", key % 97 == 0, 1 + key % 3),
            ("phasealpha", key <= 512, 1),
            ("phasebeta", 1024 < key <= 1536, 2),
            ("phasegamma", key > 2560, 3),
        ]:
            if selected:
                terms.extend([term] * freq)
        rows.append((key, "part" + str(key), " ".join(terms) if key % 41 else None))
    return rows


def capture(options):
    result = []
    terms = ["database", "database audit", "database audit tokenizer",
             "missingword database audit tokenizer", "tokenizer",
             "phasealpha phasegamma", "phasealpha phasebeta phasegamma",
             "missingword", "+database -audit"]
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            cursor.execute("USE " + DATABASE)
            for table, primary in [("simple_docs", "PRIMARY KEY(id)"),
                                   ("composite_docs", "PRIMARY KEY(id,part)")]:
                cursor.execute("CREATE TABLE " + table +
                               "(id INT,part VARCHAR(32),body TEXT," + primary + ")")
                cursor.executemany("INSERT INTO " + table + " VALUES(%s,%s,%s)", corpus())
                cursor.execute("ALTER TABLE " + table +
                               " ADD FULLTEXT INDEX ft(body) WITH PARSER beng")
                def check(stage):
                    for term in terms:
                        mode = " IN BOOLEAN MODE" if term.startswith("+") else ""
                        match = "MATCH(body) AGAINST(%s" + mode + ")"
                        for suffix in [" ORDER BY id", " ORDER BY score DESC,id LIMIT 1",
                                       " ORDER BY score DESC,id LIMIT 20",
                                       " ORDER BY score DESC,id LIMIT 509,517",
                                       " ORDER BY score DESC,id LIMIT 2047,513",
                                       " LIMIT 20"]:
                            sql = "SELECT id," + match + " AS score FROM " + table + " WHERE " + match + suffix
                            cursor.execute(sql, (term, term))
                            result.append([table, stage, term, suffix, list(cursor.fetchall())])
                check("built")
                cursor.execute("UPDATE " + table + " SET body='database tokenizer audit' WHERE MOD(id,17)=0")
                cursor.execute("DELETE FROM " + table + " WHERE MOD(id,23)=0")
                check("mutated")
                cursor.execute("START TRANSACTION")
                cursor.execute("UPDATE " + table + " SET body='phasebeta tokenizer' WHERE MOD(id,29)=0")
                check("transaction")
                cursor.execute("ROLLBACK")
                check("rolled_back")
                cursor.execute("DROP TABLE " + table)
            cursor.execute("DROP DATABASE " + DATABASE)
    return result


def compare(expected, actual, path="root"):
    if isinstance(expected, float):
        assert math.isclose(expected, actual, rel_tol=1e-12, abs_tol=1e-12), (path, expected, actual)
    elif isinstance(expected, (list, tuple)):
        assert len(expected) == len(actual), (path, len(expected), len(actual))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "[" + str(index) + "]")
    else:
        assert expected == actual, (path, expected, actual)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2891)
    parser.add_argument("--user", default="root")
    parser.add_argument("--record", type=Path)
    parser.add_argument("--expected", type=Path, default=Path(__file__).with_name("ranked_batches_expected.json.gz"))
    args = parser.parse_args()
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                          charset="utf8mb4", autocommit=True, read_timeout=180))
    if args.record:
        if args.record.exists():
            raise RuntimeError("Refusing to overwrite recorded results")
        args.record.write_bytes(gzip.compress((json.dumps(actual, ensure_ascii=True, separators=(",", ":")) + "\n").encode(), mtime=0))
        print("RECORDED original ranked results", len(actual))
    else:
        compare(json.loads(gzip.decompress(args.expected.read_bytes())), actual)
        print("PASS complete ordered IDs and scores; cases=", len(actual),
              "result_rows=", sum(len(case[-1]) for case in actual))


if __name__ == "__main__":
    main()
