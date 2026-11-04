#!/usr/bin/env python3
"""Check complete MATCH result IDs across retrieval batch boundaries.

Requires PyMySQL and a dedicated instance. Creates/drops only
fts_match_batches. The expected IDs are derived from the input documents,
independently of the database tokenization and retrieval implementation.
"""
import argparse
import os

import pymysql

DATABASE = "fts_match_batches"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2891)
    parser.add_argument("--user", default="root")
    args = parser.parse_args()
    rows = {}
    for key in range(1, 2052):
        # Contiguous and overlapping runs straddle both input and output batches.
        terms = {"padding", "phasealpha" if key <= 512 else
                 "phasebeta" if key <= 1024 else "phasegamma"}
        if key % 3:
            terms.add("database")
        if key % 5:
            terms.add("audit")
        if key % 7 == 0:
            terms.add("search")
        rows[key] = terms if key % 17 else set()
    cases = [
        ("database", "", lambda terms: "database" in terms),
        ("audit", "", lambda terms: "audit" in terms),
        ("database audit", "", lambda terms: bool(terms & {"database", "audit"})),
        ("database database audit", "", lambda terms: bool(terms & {"database", "audit"})),
        ("missingword", "", lambda terms: False),
        ("database audit search", "",
         lambda terms: bool(terms & {"database", "audit", "search"})),
        ("missingword database audit", "",
         lambda terms: bool(terms & {"database", "audit"})),
        ("phasealpha phasebeta phasegamma", "",
         lambda terms: bool(terms & {"phasealpha", "phasebeta", "phasegamma"})),
        ("phasealpha phasegamma", "",
         lambda terms: bool(terms & {"phasealpha", "phasegamma"})),
        ("missingword database audit search", "",
         lambda terms: bool(terms & {"database", "audit", "search"})),
        ("+database -audit", " IN BOOLEAN MODE",
         lambda terms: "database" in terms and "audit" not in terms),
        ("database audit " + " ".join("word" + str(i) for i in range(300)),
         "", lambda terms: bool(terms & {"database", "audit"})),
    ]
    checked = 0
    with pymysql.connect(host=args.host, port=args.port, user=args.user,
                         password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                         charset="utf8mb4", autocommit=True) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            cursor.execute("USE " + DATABASE)
            for table, primary in [("simple_docs", "PRIMARY KEY(id)"),
                                   ("composite_docs", "PRIMARY KEY(id,part)")]:
                cursor.execute("CREATE TABLE " + table +
                               "(id INT,part VARCHAR(32),body TEXT," + primary + ")")
                cursor.executemany("INSERT INTO " + table + " VALUES(%s,%s,%s)",
                                   [(key, "key" + str(key),
                                     (" ".join(sorted(terms)) + " ") * (1 + key % 4)
                                     if terms else None)
                                    for key, terms in rows.items()])
                cursor.execute("ALTER TABLE " + table +
                               " ADD FULLTEXT INDEX ft(body) WITH PARSER beng")
                def verify(current):
                    nonlocal checked
                    for term, mode, predicate in cases:
                        match = "MATCH(body) AGAINST(%s" + mode + ")"
                        expected = sorted(key for key, terms in current.items() if predicate(terms))
                        for condition in [match, match + ">0", "0<" + match]:
                            cursor.execute("SELECT id FROM " + table + " WHERE " +
                                           condition + " ORDER BY id", (term,))
                            actual = [row[0] for row in cursor.fetchall()]
                            assert actual == expected, (table, term, condition, actual[:20], expected[:20])
                            checked += 1
                        cursor.execute("SELECT COUNT(*) FROM " + table + " WHERE " + match, (term,))
                        assert cursor.fetchone()[0] == len(expected), (table, term, "count")
                        cursor.execute("SELECT id FROM " + table + " WHERE " +
                                       match + " ORDER BY id LIMIT 509,517", (term,))
                        assert [row[0] for row in cursor.fetchall()] == expected[509:1026], (table, term, "limit")
                        checked += 2
                verify(rows)
                changed = {key: ({"database", "search"} if key % 11 == 0 else terms)
                           for key, terms in rows.items() if key % 13}
                cursor.execute("UPDATE " + table + " SET body='database search' WHERE MOD(id,11)=0")
                cursor.execute("DELETE FROM " + table + " WHERE MOD(id,13)=0")
                verify(changed)
                cursor.execute("START TRANSACTION")
                cursor.execute("UPDATE " + table + " SET body='audit' WHERE MOD(id,19)=0")
                transactional = {key: ({"audit"} if key % 19 == 0 else terms)
                                 for key, terms in changed.items()}
                verify(transactional)
                cursor.execute("ROLLBACK")
                verify(changed)
                cursor.execute("DROP TABLE " + table)
            cursor.execute("DROP DATABASE " + DATABASE)
    print("PASS complete MATCH batch-boundary results; cases=", checked,
          "input_rows_per_table=", len(rows))


if __name__ == "__main__":
    main()
