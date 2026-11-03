#!/usr/bin/env python3
"""Verify shared string comparison against digests from unmodified seekdb.

The digest covers every cell of the complete result, including comparison
signs, sorted IDs and group membership. Run --record only on the baseline.
Requires PyMySQL and a dedicated instance; only fts_collation_regression is
created/dropped. Credentials come from SEEKDB_TEST_PASSWORD.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random

import pymysql

DATABASE = "fts_collation_regression"


def values():
    texts = ["", " ", "  ", "a", "A", "a ", "A  ", "a\x00", "a\x00 ",
             "\x00", "\x01", "\x1f", "\x7f", "0", "9", "_", "~", "abc",
             "ABC", "abd", "abc ", "abcd", "café", "CAFE", "Cafe\u0301",
             "é", "É", "e", "ß", "ss", "İ", "i", "ı", "Ω", "ω", "Ж", "ж",
             "数据库", "数据", "全文索引", "全文索引 ", "中A", "中a", "中a ",
             "Ａ", "ａ", "😀", "🧪", "𠀀", "�", "a" * 1024,
             "a" * 1023 + "b", "数据库" * 200]
    # Include the edges of the weight tables and UTF-8 length boundaries.
    for point in [0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
                  0x10000, 0x10ffff]:
        texts.extend([chr(point), "a" + chr(point), chr(point) + " "])
    return texts


def capture(options):
    result = []
    def query(cursor, name, sql):
        cursor.execute(sql)
        rows = cursor.fetchall()
        payload = json.dumps(rows, ensure_ascii=True, separators=(",", ":"),
                             default=str).encode("ascii")
        result.append({"name": name, "rows": len(rows),
                       "sha256": hashlib.sha256(payload).hexdigest()})
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            cursor.execute("USE " + DATABASE)
            for collation in ["utf8mb4_general_ci", "utf8mb4_unicode_ci", "utf8mb4_bin"]:
                cursor.execute("CREATE TABLE strings(id INT PRIMARY KEY,s VARCHAR(4096) "
                               "COLLATE " + collation + ")")
                data = [(index, text) for index, text in enumerate(values())] + [(999, None)]
                cursor.executemany("INSERT INTO strings VALUES(%s,%s)", data)
                query(cursor, collation + "/pairs",
                      "SELECT a.id,b.id,STRCMP(a.s,b.s),a.s=b.s,a.s<b.s,a.s<=>b.s "
                      "FROM strings a CROSS JOIN strings b ORDER BY a.id,b.id")
                # Duplicates span input batches and sorter runs.
                rng = random.Random(141)
                duplicates = [(1000 + index, rng.choice(values())) for index in range(3500)]
                cursor.executemany("INSERT INTO strings VALUES(%s,%s)", duplicates)
                for order in ["ASC", "DESC"]:
                    query(cursor, collation + "/sort/" + order,
                          "SELECT id FROM strings ORDER BY s " + order + ",id")
                    query(cursor, collation + "/topn/" + order,
                          "SELECT id FROM strings ORDER BY s " + order + ",id LIMIT 113")
                query(cursor, collation + "/groups",
                      "SELECT MIN(id),COUNT(*),SUM(id) FROM strings GROUP BY s ORDER BY MIN(id)")
                query(cursor, collation + "/distinct",
                      "SELECT COUNT(DISTINCT s) FROM strings")
                cursor.execute("CREATE INDEX string_key ON strings(s(128))")
                query(cursor, collation + "/index",
                      "SELECT id FROM strings FORCE INDEX(string_key) WHERE s='cafe' ORDER BY id")
                query(cursor, collation + "/index_order",
                      "SELECT id FROM strings FORCE INDEX(string_key) ORDER BY s,id")
                cursor.execute("DROP TABLE strings")
            cursor.execute("DROP DATABASE " + DATABASE)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2891)
    parser.add_argument("--user", default="root")
    parser.add_argument("--record", type=Path)
    args = parser.parse_args()
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                          charset="utf8mb4", autocommit=True))
    fixture = Path(__file__).with_name("collation_expected.json")
    if args.record:
        args.record.write_text(json.dumps(actual, indent=2) + "\n", encoding="utf-8")
        print("RECORDED", args.record, "cases=", len(actual))
    else:
        expected = json.loads(fixture.read_text(encoding="utf-8"))
        if actual != expected:
            for before, after in zip(expected, actual):
                if before != after:
                    print("EXPECTED", before, "ACTUAL", after)
            raise AssertionError("Shared string comparison changed")
        print("PASS collation_cases=", len(actual),
              "checked_result_rows=", sum(item["rows"] for item in actual))


if __name__ == "__main__":
    main()
