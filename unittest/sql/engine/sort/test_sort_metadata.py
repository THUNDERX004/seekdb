#!/usr/bin/env python3
"""Complete SQL result differential for shared sorting, on a dedicated instance.

No server/build/native/performance actions. Recording is explicit, refuses
overwrite, and requires independently verified official-original provenance.
The caller owns binary verification and plan/path coverage; hints alone are not
proof. Run the same script and fixture against the candidate afterwards.
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
import sys

OFFICIAL = "5bce9e06d1281347ad61344e4b01fab9158c58a2"
DATABASE = "sort_metadata_r13"
ROW_COUNT = 1027
MODES = (("default", None, None), ("batch1", 1, None),
         ("batch256", 256, None), ("batch513", 513, None),
         ("encode_off", 256, False))
STRICT_MODE = "STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION"
DDL = [
    "CREATE TABLE n(id BIGINT PRIMARY KEY,g INT,k BIGINT,u BIGINT UNSIGNED,"
    "d DECIMAL(38,12),s VARCHAR(64) COLLATE utf8mb4_general_ci,b VARBINARY(32),"
    "payload TEXT,KEY ik(k),KEY ig(g),KEY igk(g,k),KEY iu(u))",
]
for _parser in ("beng", "ik"):
    DDL.append("CREATE TABLE ft_" + _parser + "(id BIGINT PRIMARY KEY,body TEXT,"
               "FULLTEXT INDEX ft(body) WITH PARSER " + _parser + ")")


def documents():
    strings = (None, "", "a", "A", "a ", "é", "É", "中", "😀", "İ", "ß", "\0x")
    binaries = (None, b"", b"\0", b"\0\0", b"a", b"a\0", b"a ", b"\xff")
    for i in range(1, ROW_COUNT + 1):
        yield (i, None if i % 17 == 0 else i % 7,
               None if i % 11 == 0 else (-(1 << 63) if i == 1 else
                   (1 << 63) - 1 if i == 2 else i % 19 - 9),
               None if i % 13 == 0 else (1 << 64) - 1 if i == 3 else (1 << 63) + i % 23,
               None if i % 23 == 0 else Decimal(i % 31 - 15) / Decimal(1000000000000),
               strings[i % len(strings)], binaries[i % len(binaries)],
               ("payload 中 \0 " + str(i % 5)) * (1 + i % 4))


def fts_documents():
    for i in range(1, ROW_COUNT + 1):
        yield (i, " ".join(["database"] * (1 + i % 5)
                          + (["audit"] * (1 + i % 3) if i % 3 else [])
                          + (["数据库 搜索"] if i % 7 else [])
                          + ["background"] * (1 + i % 11)))


def queries():
    selected = "id,g,k,u,d,s,HEX(b) AS bh,payload"
    # Unique last key makes complete row order deterministic even for collations.
    orders = ("-id", "k ASC,id DESC", "k DESC,id ASC", "g ASC,k DESC,id ASC",
              "g DESC,k ASC,id DESC", "s ASC,id ASC", "s DESC,id DESC",
              "b ASC,id ASC", "b DESC,id DESC", "d DESC,u ASC,id DESC",
              "g ASC,s DESC,k ASC,id DESC", "g DESC,b ASC,d DESC,id ASC",
              "g ASC,k DESC,u ASC,d DESC,id ASC",
              "g DESC,s ASC,b DESC,d ASC,u DESC,id ASC",
              "(k IS NULL) ASC,k ASC,id DESC", "(s IS NULL) DESC,s DESC,id ASC")
    for index, order in enumerate(orders):
        yield ("full/" + str(index), "SELECT " + selected + " FROM n ORDER BY " + order)
        yield ("topn/" + str(index), "SELECT " + selected + " FROM n ORDER BY " + order
               + " LIMIT 255,257")
    for index, order in enumerate(("k ASC", "k DESC", "g ASC,k DESC", "g DESC,k ASC")):
        # Tied projected rows are byte-identical, so physical tie order is irrelevant.
        yield ("ties/" + str(index), "SELECT " + ("g,k" if "g " in order else "k")
               + " FROM n ORDER BY " + order)
    for offset, limit in ((0, 0), (0, 1), (0, 256), (256, 257), (1026, 257), (4096, 1)):
        yield ("limits/" + str(offset) + "/" + str(limit), "SELECT " + selected
               + " FROM n ORDER BY d DESC,b ASC,id DESC LIMIT " + str(offset) + "," + str(limit))
    yield ("empty", "SELECT " + selected + " FROM n WHERE id<0 ORDER BY s,id")
    yield ("one", "SELECT " + selected + " FROM n WHERE id=513 ORDER BY s DESC,id")
    yield ("group", "SELECT g,k,COUNT(*),SUM(d),MIN(id),MAX(id) FROM n GROUP BY g,k "
           "ORDER BY g DESC,k ASC")
    yield ("merge_group", "SELECT /*+ USE_MERGE_AGGREGATION */ g,k,COUNT(*),SUM(d) "
           "FROM n GROUP BY g,k ORDER BY g ASC,k DESC")
    yield ("concat", "SELECT g,GROUP_CONCAT(id ORDER BY k DESC,id ASC SEPARATOR ':') "
           "FROM n GROUP BY g ORDER BY g")
    yield ("concat4", "SELECT g,GROUP_CONCAT(id ORDER BY k ASC,d DESC,b ASC,id DESC "
           "SEPARATOR ':') FROM n GROUP BY g ORDER BY g")
    yield ("distinct", "SELECT DISTINCT g,k FROM n ORDER BY g DESC,k ASC")
    yield ("union", "SELECT g,k FROM n WHERE id<=513 UNION SELECT g,k FROM n WHERE id>255 "
           "ORDER BY g DESC,k ASC")
    yield ("window", "SELECT id,g,k,ROW_NUMBER() OVER(PARTITION BY g ORDER BY k DESC,id),"
           "RANK() OVER(PARTITION BY g ORDER BY k),DENSE_RANK() OVER(PARTITION BY g ORDER BY k) "
           "FROM n ORDER BY id")
    yield ("partition_topn", "SELECT id,g,k,rn FROM (SELECT id,g,k,ROW_NUMBER() OVER("
           "PARTITION BY g ORDER BY k DESC,id ASC) rn FROM n) q WHERE rn<=7 ORDER BY g,rn")
    yield ("prefix_candidate", "SELECT /*+ INDEX(n igk) */ id,g,k,s FROM n "
           "ORDER BY g,k,s DESC,id DESC")
    yield ("rescan", "SELECT o.id,(SELECT GROUP_CONCAT(i.id ORDER BY i.d DESC,i.id) "
           "FROM n i WHERE i.g=o.g AND i.id<100) FROM n o WHERE o.id<30 ORDER BY o.id")
    yield ("merge_union_candidate", "SELECT id,g,k FROM n WHERE g=2 OR k=3 ORDER BY id")
    yield ("merge_intersection_candidate", "SELECT id,g,k FROM n WHERE g=2 AND k=3 ORDER BY id")
    yield ("merge_sort_candidate", "SELECT id,g,k,s FROM n WHERE g=2 OR k=3 "
           "ORDER BY s DESC,k ASC,id DESC LIMIT 1,257")
    for parser in ("beng", "ik"):
        score = "MATCH(body) AGAINST('database audit')"
        table = "ft_" + parser
        for direction in ("ASC", "DESC"):
            yield ("fts/" + parser + "/" + direction,
                   "SELECT id," + score + " AS score FROM " + table + " WHERE " + score
                   + " ORDER BY score " + direction + ",id DESC LIMIT 255,257")
        yield ("fts_merge/" + parser, "SELECT id," + score + " AS score FROM " + table
               + " WHERE " + score + " OR id<10 ORDER BY score DESC,id ASC LIMIT 257")


def cases():
    for mode, size, encoded in MODES:
        for name, sql in queries():
            hints = []
            if size is not None:
                hints.append("OPT_PARAM('rowsets_max_rows' " + str(size) + ")")
            if encoded is not None:
                hints.append("OPT_PARAM('enable_newsort' '" + str(encoded).lower() + "')")
            if hints:
                hint = " ".join(hints)
                if sql.startswith("SELECT /*+"):
                    sql = sql.replace("SELECT /*+", "SELECT /*+ " + hint, 1)
                else:
                    sql = sql.replace("SELECT ", "SELECT /*+ " + hint + " */ ", 1)
            yield {"id": mode + "/" + name, "sql": sql}


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


def signature(workload):
    definition = {"cases": workload, "ddl": DDL, "strict_mode": STRICT_MODE,
                  "rows": [[cell(v) for v in row] for row in documents()],
                  "fts_rows": list(fts_documents()), "group_concat_max_len": 1048576}
    return hashlib.sha256(json.dumps(definition, ensure_ascii=True, allow_nan=False,
                                     separators=(",", ":")).encode()).hexdigest()


def capture(options, workload):
    import pymysql
    results, plans = [], []
    # Preserve binary cells while adapting the driver's DECIMAL bytes conversion.
    conversions = pymysql.converters.conversions.copy()
    def decimal_value(value):
        return Decimal(value.decode("ascii") if isinstance(value, bytes) else value)
    conversions[pymysql.constants.FIELD_TYPE.DECIMAL] = decimal_value
    conversions[pymysql.constants.FIELD_TYPE.NEWDECIMAL] = decimal_value
    options = dict(options, conv=conversions)
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("SET SESSION sql_mode=%s", (STRICT_MODE,))
            cursor.execute("SET SESSION group_concat_max_len=1048576")
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE + " CHARACTER SET utf8mb4")
            try:
                cursor.execute("USE " + DATABASE)
                for sql in DDL:
                    cursor.execute(sql)
                cursor.executemany("INSERT INTO n VALUES(%s,%s,%s,%s,%s,%s,%s,%s)", list(documents()))
                for parser in ("beng", "ik"):
                    cursor.executemany("INSERT INTO ft_" + parser + " VALUES(%s,%s)",
                                       list(fts_documents()))
                for case in workload:
                    # Plans are evidence for review, not portable expected results.
                    print("CASE", case["id"], "EXPLAIN", flush=True, file=sys.stderr)
                    cursor.execute("EXPLAIN EXTENDED " + case["sql"])
                    plans.append({"id": case["id"],
                                  "rows": [[cell(v) for v in row] for row in cursor.fetchall()]})
                    print("CASE", case["id"], "EXECUTE", flush=True, file=sys.stderr)
                    cursor.execute(case["sql"])
                    columns = [[cell(v) for v in column] for column in cursor.description]
                    rows = [[cell(v) for v in row] for row in cursor.fetchall()]
                    cursor.execute("SHOW WARNINGS")
                    warnings = [[cell(v) for v in row] for row in cursor.fetchall()]
                    results.append({"id": case["id"], "columns": columns, "rows": rows,
                                    "warnings": warnings})
            finally:
                if connection.open:
                    cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
    return results, plans


def compare(expected, actual, path="results"):
    if type(expected) is not type(actual):
        raise AssertionError((path, "type mismatch"))
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError((path, "key mismatch"))
        for key in expected:
            compare(expected[key], actual[key], path + "." + key)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise AssertionError((path, "length mismatch", len(expected), len(actual)))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "[" + str(index) + "]")
    elif expected != actual:
        raise AssertionError((path, expected, actual))


def new_path(parser, path):
    if os.path.lexists(path) or not path.parent.is_dir():
        parser.error("Output requires a new path in an existing directory: " + str(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, choices=(2891,), default=2891)
    parser.add_argument("--user", default="root")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--record", type=Path, metavar="NEW_PATH")
    mode.add_argument("--expected", type=Path,
                      default=Path(__file__).with_name("sort_metadata_expected.json.gz"))
    parser.add_argument("--record-source-commit")
    parser.add_argument("--record-source-binary-sha256")
    parser.add_argument("--plans", type=Path, required=True, metavar="NEW_PATH",
                        help="Review plan/path coverage separately; no silent unsupported-case skips")
    args = parser.parse_args()
    new_path(parser, args.plans)
    workload = list(cases())
    workload_sha = signature(workload)
    if args.record is not None:
        new_path(parser, args.record)
        if args.plans.resolve() == args.record.resolve():
            parser.error("Fixture and plan report must have distinct paths")
        if args.record_source_commit != OFFICIAL:
            parser.error("--record requires --record-source-commit " + OFFICIAL)
        if not re.fullmatch(r"[0-9a-fA-F]{64}", args.record_source_binary_sha256 or ""):
            parser.error("--record requires independently verified original binary SHA256")
    else:
        if args.record_source_commit or args.record_source_binary_sha256:
            parser.error("Recording provenance requires --record")
        expected = json.loads(gzip.decompress(args.expected.read_bytes()))
        if (expected.get("format") != 1 or expected.get("workload_sha256") != workload_sha
                or expected.get("original", {}).get("commit") != OFFICIAL
                or not re.fullmatch(r"[0-9a-f]{64}", expected.get("original", {}).get("binary_sha256", ""))
                or [r["id"] for r in expected.get("results", [])] != [c["id"] for c in workload]):
            raise ValueError("Incomplete, unrelated, or unverified original fixture")
    actual, plans = capture(dict(host=args.host, port=args.port, user=args.user,
                                password=os.getenv("SEEKDB_TEST_PASSWORD", ""), charset="utf8mb4",
                                use_unicode=False, autocommit=True, connect_timeout=10,
                                read_timeout=180, write_timeout=180), workload)
    with args.plans.open("x", encoding="utf-8") as output:
        json.dump({"workload_sha256": workload_sha, "plans": plans}, output, ensure_ascii=True)
        output.write("\n")
    if args.record is not None:
        payload = {"format": 1, "workload_sha256": workload_sha,
                   "original": {"commit": OFFICIAL,
                                "binary_sha256": args.record_source_binary_sha256.lower()},
                   "results": actual}
        raw = (json.dumps(payload, ensure_ascii=True, allow_nan=False,
                          separators=(",", ":")) + "\n").encode()
        with args.record.open("xb") as output:
            output.write(gzip.compress(raw, mtime=0))
        print("RECORDED complete independent original results", args.record)
    else:
        compare(expected["results"], actual)
        print("PASS complete ordered rows, metadata and warnings")
    print("cases=", len(actual), "plan review still required for path coverage")


if __name__ == "__main__":
    main()
