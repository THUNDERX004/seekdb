#!/usr/bin/env python3
"""Compare complete SQL results against an independently recorded original.

Dedicated instance only: creates/drops fts_query_materialization_r11, port 2891.
--record NEW_PATH requires the official competition commit and original binary
SHA256. The caller must independently verify the running binary; SQL version
strings cannot prove provenance. Never record on a candidate. No auto-recording,
overwrite, unsupported-case skip, service startup, build or performance timing.
"""
import argparse
from decimal import Decimal
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import re

DATABASE = "fts_query_materialization_r11"
OFFICIAL = "5bce9e06d1281347ad61344e4b01fab9158c58a2"
DOCUMENT_COUNT = 2051
BOUNDARIES = (1, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2048, 2051)
MODES = (("default", None), ("batch1", 1), ("batch256", 256), ("batch513", 513))
STRICT_MODE = "STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION"


def letters(number):
    return "".join(chr(97 + (number // power) % 26) for power in (676, 26, 1))


def documents():
    result = []
    for number in range(1, DOCUMENT_COUNT + 1):
        terms = ["background"] * (1 + number % 9)
        for term, selected, frequency in (
            ("database", number % 3 != 0, 1 + number % 7),
            ("audit", number % 5 != 0, 1 + number % 3),
            ("search", number % 97 == 0, 2),
            ("phasealpha", number <= 512, 1),
            ("phasebeta", 512 < number <= 1024, 1),
            ("phasegamma", number > 1024, 1),
            ("数据库 搜索", number % 11 == 0, 1),
        ):
            if selected:
                terms.extend([term] * frequency)
        terms.extend("edge" + letters(index) for index, boundary in enumerate(BOUNDARIES)
                     if number <= boundary)
        result.append((number, " ".join(terms)))
    return result + [(DOCUMENT_COUNT + 1, ""), (DOCUMENT_COUNT + 2, None)]


def table_specs():
    # Integer extrema and UInt64 high-bit transition remain real internal keys.
    yield ("unsigned_docs", "id BIGINT UNSIGNED NOT NULL", "id",
           "CAST(id AS CHAR)",
           lambda n: ((1 << 64) - 1 if n == DOCUMENT_COUNT + 2 else
                      (1 << 63) - 1025 + n,))
    yield ("signed_docs", "id BIGINT NOT NULL", "id", "CAST(id AS CHAR)",
           lambda n: (-(1 << 63) if n == 1 else
                      (1 << 63) - 1 if n == DOCUMENT_COUNT + 2 else n - 1027,))
    yield ("binary_docs", "id VARBINARY(24) NOT NULL", "id", "HEX(id)",
           lambda n: (b"\0key" + n.to_bytes(4, "big") + b"\0" + b" " * (n % 3),))
    yield ("composite_docs", "id BIGINT NOT NULL,part VARBINARY(16) NOT NULL,seq INT NOT NULL",
           "id,part,seq", "CAST(id AS CHAR),HEX(part),seq",
           lambda n: (n // 3, b"\0part\0" + b" " * (n % 3), n % 3))


def query_cases(table, projection, order):
    def match(query, boolean=False, alias=""):
        column = alias + ".body" if alias else "body"
        return "MATCH(" + column + ") AGAINST(%s" + (" IN BOOLEAN MODE" if boolean else "") + ")"

    terms = ["database", "database audit", "database database audit search",
             "missingword database audit", "missingword", "", None,
             "phasealpha phasebeta phasegamma", "数据库 搜索"]
    terms += ["edge" + letters(index) for index in range(len(BOUNDARIES))]
    # >256 unique tokens exercises the unchanged TaaT algorithm and its errors.
    terms.append("database " + " ".join("absent" + letters(i) for i in range(270)))
    for index, term in enumerate(terms):
        predicate = match(term)
        for label, selected in (("count_star", "COUNT(*)"),
                                ("count_one", "COUNT(1)"),
                                ("sum_one", "SUM(1)"),
                                ("sum_nullable_constant", "SUM(CAST(NULL AS SIGNED))")):
            yield (label + "/" + str(index), "SELECT " + selected + " FROM " + table
                   + " WHERE " + predicate, (term,), False, ())
    term = "database audit"
    predicate = match(term)
    base = " FROM " + table + " WHERE " + predicate
    yield ("id_output", "SELECT " + projection + base + " ORDER BY " + order,
           (term,), False, ())
    yield ("id_expression", "SELECT " + projection + ",id+7,MOD(id,13)" + base
           + " ORDER BY " + order, (term,), False, ())
    yield ("column_aggregates", "SELECT COUNT(id),SUM(id),MIN(id),MAX(id),COUNT(DISTINCT id)"
           + base, (term,), False, ())
    for label, extra in (("key_filter", " AND MOD(id,7)=0"),
                         ("body_filter", " AND LENGTH(body)>100"),
                         ("expression_filter", " AND MOD(id+LENGTH(body),7)>2")):
        yield (label, "SELECT COUNT(*)" + base + extra, (term,), False, ())
    yield ("group_by", "SELECT MOD(id,7),COUNT(*),SUM(id)" + base
           + " GROUP BY MOD(id,7) ORDER BY MOD(id,7)", (term,), False, ())
    yield ("score_projection", "SELECT " + projection + "," + predicate + " AS score"
           + base + " ORDER BY " + order, (term, term), False, ())
    yield ("score_filter", "SELECT COUNT(*) FROM " + table + " WHERE " + predicate
           + ">0.5", (term,), False, ())
    for offset, limit in ((0, 0), (0, 1), (0, 257), (255, 257), (511, 513),
                          (1023, 514), (2047, 513), (8192, 1)):
        suffix = " LIMIT " + str(offset) + "," + str(limit)
        # A value-free derived table can push LIMIT/OFFSET into the scan.
        yield ("limited_count/" + suffix, "SELECT COUNT(*) FROM (SELECT 1 AS one"
               + base + suffix + ") limited", (term,), False, ())
        yield ("limited_ids/" + suffix, "SELECT " + projection + base
               + " ORDER BY " + order + suffix, (term,), False, ())
        yield ("ranked/" + suffix, "SELECT " + projection + "," + predicate + " AS score"
               + base + " ORDER BY score DESC," + order + suffix,
               (term, term), False, ())
    for term in ("database", "missingword"):
        predicate = match(term)
        base = " FROM " + table + " WHERE " + predicate
        yield ("exists/" + term, "SELECT EXISTS(SELECT 1" + base + ")", (term,), False, ())
        yield ("not_exists/" + term, "SELECT NOT EXISTS(SELECT 1" + base + ")", (term,), False, ())
        yield ("scalar_subquery/" + term, "SELECT (SELECT COUNT(*)" + base + ")",
               (term,), False, ())
        yield ("derived_sum/" + term, "SELECT SUM(one) FROM (SELECT 1 AS one"
               + base + ") derived", (term,), False, ())
        # Assignment below a parent statement forces the existing scalar plan
        # path (ob_code_generator.cpp). Sum is independent of traversal order.
        yield ("scalar_assignment/" + term,
               "SELECT SUM(n) FROM (SELECT @fts_r11:=@fts_r11+1 AS n" + base + ") assigned",
               (term,), True, ("SET @fts_r11=0",))
    yield ("correlated_rescan",
           "SELECT o.id,(SELECT COUNT(*) FROM " + table + " i WHERE "
           + match(term, alias="i") + " AND i.id=o.id) FROM " + table
           + " o WHERE MOD(o.id,97)=0 ORDER BY "
           + ",".join("o." + key for key in order.split(",")), ("database audit",), False, ())
    yield ("in_subquery", "SELECT " + projection + " FROM " + table
           + " WHERE id IN (SELECT id FROM " + table + " WHERE " + match(term)
           + ") ORDER BY " + order, ("database audit",), False, ())
    union_base = " FROM " + table + " WHERE " + match("database audit")
    yield ("union_counts", "SELECT COUNT(*)" + union_base + " UNION ALL SELECT SUM(1)"
           + union_base, ("database audit", "database audit"), False, ())
    for term in ("+database -audit", "+database +search"):
        yield ("boolean/" + term, "SELECT COUNT(*) FROM " + table + " WHERE "
               + match(term, True), (term,), False, ())
    long_boolean = "+database " + " ".join("absent" + letters(i) for i in range(270))
    yield ("boolean_token_limit_error", "SELECT COUNT(*) FROM " + table + " WHERE "
           + match(long_boolean, True), (long_boolean,), True, ())
    yield ("warning_expression", "SELECT CAST('not-a-number' AS SIGNED),COUNT(*) FROM "
           + table + " WHERE " + match(term), ("database audit",), False, ())


def cases():
    for parser in ("beng", "ik"):
        for table, columns, order, projection, keys in table_specs():
            name = parser + "_" + table
            for mode, size in MODES:
                for label, sql, params, allow_error, before in query_cases(name, projection, order):
                    if size is not None:
                        sql = sql.replace("SELECT ", "SELECT /*+ OPT_PARAM('rowsets_max_rows' "
                                          + str(size) + ") */ ", 1)
                    yield {"id": parser + "/" + table + "/" + mode + "/" + label,
                           "sql": sql, "params": list(params), "allow_error": allow_error,
                           "before": list(before)}


def cell(value):
    if isinstance(value, bytes):
        return {"bytes": value.hex()}
    if isinstance(value, Decimal):
        return {"decimal": str(value)}
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("Non-finite SQL result")
        return {"float": repr(value)}
    if value is None or type(value) in (str, int, bool):
        return value
    raise TypeError(type(value).__name__)


def run_query(cursor, case):
    import pymysql
    for statement in case["before"]:
        cursor.execute(statement)
    try:
        cursor.execute(case["sql"], tuple(case["params"]))
        columns = [[cell(value) for value in column] for column in cursor.description]
        rows = [[cell(value) for value in row] for row in cursor.fetchall()]
    except pymysql.MySQLError as error:
        code = error.args[0] if error.args else None
        if (not case["allow_error"] or not isinstance(code, int)
                or code <= 0 or 2000 <= code < 3000):
            raise
        return {"error_number": code}
    cursor.execute("SHOW WARNINGS")
    return {"columns": columns, "rows": rows,
            "warnings": [[cell(value) for value in row] for row in cursor.fetchall()]}


def capture(options, workload):
    import pymysql
    from pymysql.constants import FIELD_TYPE
    conversions = pymysql.converters.conversions.copy()

    def decimal_value(value):
        # Preserve raw binary cells while decoding decimal protocol text.
        return Decimal(value.decode("ascii") if isinstance(value, bytes) else value)

    conversions[FIELD_TYPE.DECIMAL] = decimal_value
    conversions[FIELD_TYPE.NEWDECIMAL] = decimal_value
    options = dict(options, conv=conversions)
    results = []
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("SET SESSION sql_mode=%s", (STRICT_MODE,))
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE)
            try:
                cursor.execute("USE " + DATABASE)
                for parser in ("beng", "ik"):
                    for table, columns, order, projection, keys in table_specs():
                        name = parser + "_" + table
                        cursor.execute("CREATE TABLE " + name + "(" + columns
                                       + ",body TEXT,PRIMARY KEY(" + order + "))")
                        rows = [keys(n) + (body,) for n, body in documents()]
                        cursor.executemany("INSERT INTO " + name + " VALUES("
                                           + ",".join(["%s"] * len(rows[0])) + ")", rows)
                        cursor.execute("ALTER TABLE " + name
                                       + " ADD FULLTEXT INDEX ft(body) WITH PARSER " + parser)
                        prefix = parser + "/" + table + "/"
                        for case in workload:
                            if case["id"].startswith(prefix):
                                results.append({"id": case["id"], "result": run_query(cursor, case)})
            finally:
                cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
    return results


def compare(expected, actual, path="root"):
    if type(expected) is not type(actual):
        raise AssertionError((path, type(expected).__name__, type(actual).__name__))
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError((path, list(expected), list(actual)))
        if set(expected) == {"float"}:
            left, right = float(expected["float"]), float(actual["float"])
            if not math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-12):
                raise AssertionError((path, left, right))
        else:
            for key in expected:
                compare(expected[key], actual[key], path + "." + key)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise AssertionError((path, len(expected), len(actual)))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "[" + str(index) + "]")
    elif expected != actual:
        raise AssertionError((path, expected, actual))


def workload_hash(workload):
    definition = {"cases": workload, "documents": documents(), "strict_mode": STRICT_MODE,
                  "tables": [(name, columns, order, projection,
                              [[cell(value) for value in keys(n)] for n, _ in documents()])
                             for name, columns, order, projection, keys in table_specs()]}
    return hashlib.sha256(json.dumps(definition, ensure_ascii=True, allow_nan=False,
                                     separators=(",", ":")).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, choices=(2891,), default=2891)
    parser.add_argument("--user", default="root")
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument("--record", type=Path, metavar="NEW_PATH")
    destination.add_argument("--expected", type=Path,
                             default=Path(__file__).with_name("query_materialization_expected.json.gz"))
    parser.add_argument("--record-source-commit")
    parser.add_argument("--record-source-binary-sha256")
    args = parser.parse_args()
    workload = list(cases())
    signature = workload_hash(workload)
    if args.record is not None:
        if args.record_source_commit != OFFICIAL:
            parser.error("--record requires --record-source-commit " + OFFICIAL)
        if not re.fullmatch(r"[0-9a-fA-F]{64}", args.record_source_binary_sha256 or ""):
            parser.error("--record requires the independently verified original binary SHA256")
        if os.path.lexists(args.record) or not args.record.parent.is_dir():
            parser.error("Recording requires a new path in an existing directory")
    else:
        if args.record_source_commit or args.record_source_binary_sha256:
            parser.error("Recording provenance options require --record")
        # A missing/corrupt/unrelated fixture fails before any connection.
        expected = json.loads(gzip.decompress(args.expected.read_bytes()))
        if (expected.get("format") != 1 or expected.get("workload_sha256") != signature
                or expected.get("original", {}).get("commit") != OFFICIAL
                or not re.fullmatch(r"[0-9a-f]{64}", expected.get("original", {}).get("binary_sha256", ""))):
            raise ValueError("Wrong workload or missing official-original fixture provenance")
        if [case["id"] for case in expected["results"]] != [case["id"] for case in workload]:
            raise ValueError("Incomplete or reordered fixture case list")
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""),
                          charset="utf8mb4", use_unicode=False, autocommit=True,
                          connect_timeout=10, read_timeout=180, write_timeout=180), workload)
    if args.record is not None:
        payload = {"format": 1, "workload_sha256": signature,
                   "original": {"commit": OFFICIAL,
                                "binary_sha256": args.record_source_binary_sha256.lower()},
                   "results": actual}
        raw = (json.dumps(payload, ensure_ascii=True, allow_nan=False,
                          separators=(",", ":")) + "\n").encode()
        with args.record.open("xb") as output:
            output.write(gzip.compress(raw, mtime=0))
        print("RECORDED complete official-original results", args.record)
    else:
        compare(expected["results"], actual)
        print("PASS complete rows, IDs, expressions, counts, warnings and SQL errors")
    print("cases=", len(actual))


if __name__ == "__main__":
    main()
