#!/usr/bin/env python3
"""Independent SQL oracle for count-only natural-language MATCH unions.

Requires PyMySQL and a dedicated seekdb instance (default port 2891; port
2881 is refused). Drops/recreates ONLY fts_count_union_accounting, including
cleanup on failure. The password comes from SEEKDB_TEST_PASSWORD.

The 131 explicit ASCII-token documents and Python set membership define the
oracle; no server TOKENIZE output, recorded fixture, candidate output, or
baseline binary is used to derive expected results. BENG terms have at least
three letters and avoid stopwords. Query sizes straddle the short-union gate
(1/2/3 versus 4/8/9 distinct terms), including duplicate and absent terms.

Six existing OPT_PARAM rowsets_max_rows hint values request small batches.
Hints and SQL results do not prove effective storage iterator capacities,
optimizer branch selection, internal input/output counters, error accounting,
or concurrent behavior. Repeated SQL PREPARE execution checks rebinding on
one connection, not an instrumented internal rescan. Ranking/score coverage
remains in the existing ranked regression; this test has no recording mode.

Run: python unittest/storage/fts/test_count_union_accounting.py --port 2891
"""
import argparse
import os

DATABASE = "fts_count_union_accounting"
BATCH_SIZES = (1, 2, 3, 7, 16, 64)
TERMS = ("qalpha", "qbravo", "qcharlie", "qdelta", "qecho", "qfoxtrot",
         "qgolf", "qhotel", "qindia")


def documents():
    """Known posting lengths include 1, 2, 3, 7, 16, 64, 65, and 131."""
    result = {}
    for key in range(1, 132):
        terms = {"qshared"}
        if key <= 65:
            terms.add("qalpha")
        if key % 2 == 0:
            terms.add("qbravo")
        if 33 <= key <= 96:
            terms.add("qcharlie")
        if key <= 7:
            terms.add("qdelta")
        if 8 <= key <= 23:
            terms.add("qecho")
        if key <= 3:
            terms.add("qfoxtrot")
        if key in (65, 66):
            terms.add("qgolf")
        if key == 129:
            terms.add("qhotel")
        if key >= 125:
            terms.add("qindia")
        result[key] = terms
    return result


def query_cases():
    return (
        ("one-present", ("qalpha",)),
        ("one-singleton", ("qhotel",)),
        ("one-missing", ("qmissing",)),
        ("two-overlap", ("qalpha", "qbravo")),
        ("two-disjoint", ("qdelta", "qecho")),
        ("two-missing", ("qmissing", "qabsent")),
        ("three-overlap", ("qalpha", "qbravo", "qcharlie")),
        ("three-missing-tail", ("qalpha", "qbravo", "qmissing")),
        ("three-missing-head", ("qmissing", "qalpha", "qbravo")),
        ("four-fallback", TERMS[:4]),
        ("eight-fallback", TERMS[:8]),
        ("nine-fallback", TERMS),
        ("duplicate-single", ("qalpha", "qalpha", "qalpha")),
        ("duplicate-union", ("qalpha", "qbravo", "qalpha", "qbravo")),
        ("all-documents", ("qshared",)),
        ("nine-all-documents", ("qshared",) + TERMS[:8]),
    )


def expected_ids(docs, terms):
    wanted = set(terms)
    return sorted(key for key, present in docs.items() if present & wanted)


def check_rows(cursor, label, sql, params, expected):
    cursor.execute(sql, params)
    actual = list(cursor.fetchall())
    if actual != expected:
        raise AssertionError("{}: expected {!r}; got {!r}; SQL={!r}".format(
            label, expected, actual, sql))
    return 1


def verify_batch(cursor, docs, batch):
    select = "SELECT /*+ OPT_PARAM('rowsets_max_rows' " + str(batch) + ") */ "
    base = " FROM docs WHERE MATCH(body) AGAINST(%s)"
    checked = 0
    # Every window has a positive LIMIT and OFFSET. Windows cross batch edges,
    # straddle final partial batches, or start beyond all 131 document IDs.
    windows = ((1, 1), (max(1, batch - 1), batch + 1), (batch, batch),
               (batch + 1, 2), (130, 3), (131, 7))
    cases = query_cases()
    for index, (name, terms) in enumerate(cases):
        label = "batch={}/{}".format(batch, name)
        query = " ".join(terms)
        ids = expected_ids(docs, terms)
        count = [(len(ids),)]
        # Keep both unlimited count shapes genuinely free of LIMIT/OFFSET.
        checked += check_rows(cursor, label + "/ids", select + "id" + base
                              + " ORDER BY id", (query,), [(key,) for key in ids])
        checked += check_rows(cursor, label + "/count-star", select + "COUNT(*)"
                              + base, (query,), count)
        checked += check_rows(cursor, label + "/count-id", select + "COUNT(id)"
                              + base, (query,), count)
        checked += check_rows(cursor, label + "/derived-unlimited", select
                              + "COUNT(*) FROM (SELECT id" + base + ") unlimited",
                              (query,), count)
        offset, limit = windows[index % len(windows)]
        limited = ids[offset:offset + limit]
        suffix = " ORDER BY id LIMIT %s OFFSET %s"
        params = (query, limit, offset)
        checked += check_rows(cursor, label + "/limited-ids", select + "id"
                              + base + suffix, params, [(key,) for key in limited])
        checked += check_rows(cursor, label + "/derived-limited-ids", select
                              + "COUNT(*) FROM (SELECT id" + base + suffix
                              + ") limited", params, [(len(limited),)])
        # Also allow a pushed limit without ORDER BY/materialized key output.
        # Its cardinality is independent of which matching IDs are selected.
        checked += check_rows(cursor, label + "/derived-limited-constant", select
                              + "COUNT(*) FROM (SELECT 1 AS present" + base
                              + " LIMIT %s OFFSET %s) limited", params,
                              [(len(limited),)])

    prepared = select + "COUNT(*) FROM docs WHERE MATCH(body) AGAINST(?)"
    cursor.execute("PREPARE fts_count_union_check FROM %s", (prepared,))
    try:
        by_name = dict(cases)
        # Reuse one prepared statement after empty/all/duplicate inputs, ending
        # at its initial query to expose retained count/end-of-input state.
        sequence = ("three-overlap", "one-missing", "all-documents",
                    "duplicate-union", "one-singleton", "three-overlap")
        for index, name in enumerate(sequence):
            terms = by_name[name]
            cursor.execute("SET @fts_count_union_term=%s", (" ".join(terms),))
            checked += check_rows(
                cursor, "batch={}/prepared-{}/{}".format(batch, index, name),
                "EXECUTE fts_count_union_check USING @fts_count_union_term", (),
                [(len(expected_ids(docs, terms)),)])
    finally:
        cursor.execute("DEALLOCATE PREPARE fts_count_union_check")
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2891)
    parser.add_argument("--user", default="root")
    args = parser.parse_args()
    if args.port == 2881:
        parser.error("port 2881 is reserved; use a dedicated test instance, e.g. 2891")
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    # Refuse the production port before importing the optional driver or opening
    # any connection. No configuration file or credentials are printed.
    import pymysql

    docs = documents()
    inserted = [(key, (" ".join(sorted(terms)) + " ") * (1 + key % 3))
                for key, terms in sorted(docs.items())]
    checked = 0
    with pymysql.connect(host=args.host, port=args.port, user=args.user,
                         password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                         charset="utf8mb4", autocommit=True,
                         connect_timeout=10, read_timeout=180) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=120000000")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            try:
                cursor.execute("USE " + DATABASE)
                cursor.execute("CREATE TABLE docs (id INT NOT NULL PRIMARY KEY,body TEXT)")
                cursor.executemany("INSERT INTO docs(id,body) VALUES(%s,%s)", inserted)
                cursor.execute("ALTER TABLE docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng")
                # Confirm the precise controlled input, not a sampled hit count.
                checked += check_rows(cursor, "inserted-documents",
                                      "SELECT id,body FROM docs ORDER BY id", (), inserted)
                for batch in BATCH_SIZES:
                    checked += verify_batch(cursor, docs, batch)
            finally:
                cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
    print("PASS: {} documents; {} query/batch groups; {} prepared count executions; "
          "{} exact result comparisons; requested rowsets_max_rows={}".format(
              len(docs), len(query_cases()) * len(BATCH_SIZES), 6 * len(BATCH_SIZES),
              checked, BATCH_SIZES))
    print("SQL results do not establish iterator capacities, internal counters, "
          "or optimizer branch coverage.")


if __name__ == "__main__":
    main()
