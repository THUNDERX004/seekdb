#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Small public-API regression for scoped encoded-sort FTS construction.

Run: python unittest/storage/fts/test_encoded_fts_build.py --port 2891
Requires PyMySQL and an already running dedicated server; port 2881 is refused.
Creates/drops ONLY fts_encoded_build. Password: SEEKDB_TEST_PASSWORD.

Preserves the R37 sidecar's 22-document, two-collation independent stored-byte
and OR-membership checks. Complete rows are checked, not just counts. MATCH
scores must be finite and positive; exact BM25 values and collation-dependent
result order have no oracle here. No recording mode or auxiliary imports.
This does not prove encoded-sort execution, ordinary-index access, spill/error
behavior, or exhaustive collation equivalence. Production derivation is credited
separately in docs/developer-guide/{en,zh-CN}/competition-round37.md; this test
copies no production encoder/comparator implementation.
"""
import argparse
from decimal import Decimal
import math
import os
import sys

DATABASE = "fts_encoded_build"
IDS = ("", "\0", "\0a", "\0\0", "\x01", "\x02", "\t", "\n", "\r", "\x1f", "\x7f",
       " ", "  ", "a", "A", "a ", "é", "É", "中", "😀", "x\0y", "x\ty")


def require(condition, message):
    # Keep correctness gates active under python -O.
    if not condition:
        raise AssertionError(message)


def documents():
    for seq, key in enumerate(IDS):
        yield (key, seq, IDS[-seq - 1], bytes((0, 255, seq)),
               "payload 中 \0 " + str(seq),
               "backgroundword " + ("orbitword " * (1 + seq % 3) if seq % 3 else "")
               + ("amberword " if seq % 4 == 0 else ""))


def check_table(cursor, name, collation, rows):
    cursor.execute(
        "CREATE TABLE " + name + "(id VARCHAR(64) NOT NULL,seq INT NOT NULL,"
        "s VARCHAR(80),b VARBINARY(16),payload TEXT,body TEXT,"
        "PRIMARY KEY(id,seq),KEY ordinary_s(s,seq)) CHARACTER SET utf8mb4 COLLATE "
        + collation)
    cursor.executemany("INSERT INTO " + name + " VALUES(%s,%s,%s,%s,%s,%s)", rows)
    cursor.execute("ALTER TABLE " + name + " ADD FULLTEXT INDEX ft(body) WITH PARSER beng")
    match = "MATCH(body) AGAINST('orbitword amberword')"
    cursor.execute("SELECT id,seq,s,b,payload,body," + match + " AS score FROM " + name
                   + " WHERE " + match + " ORDER BY s DESC,id,seq")
    actual = cursor.fetchall()
    # Literal ASCII terms have length >= 3 and avoid stopwords. Expected data
    # comes solely from explicit input, never TOKENIZE or candidate output.
    expected = {row[1]: row for row in rows
                if set(row[5].split()) & {"orbitword", "amberword"}}
    seen = set()
    for row in actual:
        require(len(row) == 7 and type(row[1]) is int, name + ": malformed row")
        seq = row[1]
        require(seq in expected and seq not in seen, name + ": unexpected/duplicate key")
        seen.add(seq)
        original = tuple(value.encode("utf-8") if isinstance(value, str) else value
                         for value in expected[seq])
        require(tuple(row[:6]) == original, name + ": stored bytes differ for seq=" + str(seq))
        require(type(row[6]) in (float, Decimal, int), name + ": invalid score type")
        score = float(row[6])
        require(math.isfinite(score) and score > 0, name + ": nonpositive/nonfinite score")
    require(seen == set(expected), name + ": complete OR membership differs")
    return len(actual)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2891)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or args.port == 2881:
        parser.error("Use a dedicated port; 2881 is forbidden")
    import pymysql
    conversions = pymysql.converters.conversions.copy()

    def decimal_value(value):
        return Decimal(value.decode("ascii") if isinstance(value, bytes) else value)

    for kind in (pymysql.constants.FIELD_TYPE.DECIMAL,
                 pymysql.constants.FIELD_TYPE.NEWDECIMAL):
        conversions[kind] = decimal_value
    connection = pymysql.connect(
        host="127.0.0.1", port=args.port, user="root",
        password=os.getenv("SEEKDB_TEST_PASSWORD", ""), charset="utf8mb4",
        use_unicode=False, conv=conversions, autocommit=True,
        connect_timeout=10, read_timeout=180, write_timeout=180)
    created = False
    try:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE + " CHARACTER SET utf8mb4")
            created = True
            cursor.execute("USE " + DATABASE)
            rows = list(documents())
            checked = sum(check_table(cursor, name, collation, rows) for name, collation in (
                ("docs_bin", "utf8mb4_bin"), ("docs_ci", "utf8mb4_general_ci")))
    finally:
        pending_error = sys.exc_info()[0] is not None
        try:
            if created:
                with connection.cursor() as cursor:
                    cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
        except Exception:
            if not pending_error:
                raise
            print("Cleanup failed; preserving the original test error", file=sys.stderr)
        finally:
            try:
                connection.close()
            except Exception:
                if not pending_error:
                    raise
    print("PASS checks=1 selects=2 input_rows=44 complete_result_rows=" + str(checked))


if __name__ == "__main__":
    main()
