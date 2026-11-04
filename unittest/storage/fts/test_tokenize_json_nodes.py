#!/usr/bin/env python3
"""Complete TOKENIZE JSON differential; explicit independent-original recording.

Dedicated instance only: creates/drops tokenize_json_nodes_r14 on port 2891.
No startup, build, native code, timing, auto-recording or unsupported-case skips.
Caller must verify the running original binary independently; declared hashes
and SQL version strings alone do not establish binary provenance.
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

OFFICIAL = "5bce9e06d1281347ad61344e4b01fab9158c58a2"
DATABASE = "tokenize_json_nodes_r14"
PARSERS = ("space", "beng", "ik", "ngram", "ngram2")
BOUNDARIES = (0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128,
              255, 256, 257, 511, 512, 513, 997, 998, 1023, 1024, 1025, 4097)
STRICT_MODE = "STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION"


def word(number):
    return "zx" + "".join(chr(97 + number // power % 26) for power in (676, 26, 1)) + "v"


def texts():
    values = [("null_doc", None), ("empty", ""), ("only_space", " \t\r\n"),
              ("only_stopwords", "a an the THE and or"),
              ("unicode", "OceanBase oceanbase 数据库 搜索 中文分词 123.45 42个"),
              ("equivalence", "café cafe CAFÉ cafe ÉÉÉ ééé EEE eee"),
              ("punctuation", '"quote" \\slash / \\ / : { } [ ] \t\n'),
              ("nul", "a\0b hello\0world \0 中文\0分词"),
              ("supplementary", "😀 𠀀 𠀁 A😀B café e\u0301 İ ı ß ẞ K"),
              ("width", "ＡＢＣ ａｂｃ ABC abc １２３ 123"),
              ("repeat", "repetition " * 2049)]
    values.extend(("length" + str(n), "x" * n + " hello " + "中" * n)
                  for n in (1, 2, 3, 9, 10, 16, 83, 84, 85, 86))
    for count in BOUNDARIES:
        tokens = [word(i) for i in range(count)]
        values.append(("unique" + str(count), " ".join(tokens)))
        if count in (1, 31, 257, 1025):
            values.append(("counts" + str(count), " ".join(tokens + tokens + tokens[:7]
                                                           + list(reversed(tokens)))))
    return values


def parser_options(parser):
    common = ["[]", '[{"additional_args":[]}]', '[{"output":"all"}]',
              '[{"output":"all"},{"output":"default"}]',
              '[{"output":"default"},{"output":"all"}]',
              '[{"output":"bad"}]', '[{"output":1}]', '[{"case":"lower"}]',
              '[{"stopwords":[]}]', '[{"unknown":1}]', '[{"additional_args":{}}]',
              '[{"additional_args":[1]}]', '[{"additional_args":[{}]}]',
              '[{"additional_args":[{"min_token_size":1,"max_token_size":84}]}]',
              '[{"additional_args":[{"unknown":1}]}]', "{}", "null", "[1]"]
    options = {
        "beng": [{"min_token_size": v} for v in (1, 3, 16, 0, 17, "3", None, True)]
                + [{"max_token_size": v} for v in (10, 84, 9, 85)]
                + [{"min_token_size": 16}, {"max_token_size": 10}],
        "space": [{"min_token_size": 1}, {"max_token_size": 10}],
        "ik": [{"ik_mode": v} for v in ("smart", "max_word", "SMART", "bad", 1)],
        "ngram": [{"ngram_token_size": v} for v in (1, 2, 10, 0, 11)],
        "ngram2": [{"min_ngram_size": v} for v in (1, 16, 0, 17)]
                  + [{"max_ngram_size": v} for v in (1, 17)],
    }
    common.extend(json.dumps([{"output": "all"}, {"additional_args": [option]}],
                             ensure_ascii=True, separators=(",", ":"))
                  for option in options[parser])
    # Duplicate and overwritten groups exercise the unchanged validation order.
    common += ['[{"additional_args":[{"min_token_size":0},{"min_token_size":3}]}]',
               '[{"additional_args":[{"min_token_size":3},{"min_token_size":0}]}]',
               '[{"additional_args":[{"unknown":1}]},{"additional_args":[]}]',
               '[{"additional_args":[1]},{"output":"bad"}]']
    return common


def cases():
    def case(name, sql, params=(), allow_error=False, before=()):
        # Long bound literals can otherwise be truncated inside a UTF8 column
        # name by the original server; compare stable metadata instead.
        if sql.startswith("SELECT") and " FROM " not in sql:
            sql += " AS r14_result"
        return {"id": name, "sql": sql, "params": list(params),
                "allow_error": allow_error, "before": list(before)}

    for batch in (1, 256):
        select = "SELECT /*+ OPT_PARAM('rowsets_max_rows' " + str(batch) + ") */ "
        for parser in PARSERS:
            prefix = str(batch) + "/" + parser + "/"
            for label, text in texts():
                for output in ("default", "all"):
                    config = '[{"output":"' + output + '"}]'
                    yield case(prefix + label + "/" + output, select + "TOKENIZE(%s,%s,%s)",
                               (text, parser, config), text is None and parser == "ik")
            for i, config in enumerate(parser_options(parser)):
                yield case(prefix + "properties/" + str(i), select + "TOKENIZE(%s,%s,%s)",
                           ("HELLO hello café 数据库 搜索 123个", parser, config), True)
            for collation in ("utf8mb4_bin", "utf8mb4_general_ci", "utf8mb4_unicode_ci"):
                for output in ("default", "all"):
                    yield case(prefix + collation + "/" + output,
                               select + "TOKENIZE(CONVERT(%s USING utf8mb4) COLLATE "
                               + collation + ",%s,%s)",
                               ("café cafe CAFÉ cafe abc ABC a\0b 中 中文 😀", parser,
                                '[{"output":"' + output + '"}]'))
            # Large results inside other JSON expressions must outlive the helper/map.
            for expression in ("JSON_ARRAY(TOKENIZE(%s,%s,%s),'tail')",
                               "JSON_OBJECT('value',TOKENIZE(%s,%s,%s),'tail',7)",
                               "JSON_LENGTH(TOKENIZE(%s,%s,%s))",
                               "JSON_EXTRACT(TOKENIZE(%s,%s,%s),'$[0]')",
                               "JSON_EXTRACT(TOKENIZE(%s,%s,%s),'$.tokens[0]')",
                               "JSON_KEYS(TOKENIZE(%s,%s,%s))"):
                for output in ("default", "all"):
                    yield case(prefix + "consumer/" + expression + "/" + output,
                               select + expression,
                               (" ".join(word(i) for i in range(257)), parser,
                                '[{"output":"' + output + '"}]'))
            yield case(prefix + "typed_json_options",
                       select + "TOKENIZE(%s,%s,CAST(%s AS JSON))",
                       ("HELLO 数据库", parser, '[{"output":"all"}]'))
            yield case(prefix + "omitted_options", select + "TOKENIZE(%s,%s)",
                       ("HELLO hello 数据库", parser))
            yield case(prefix + "empty_doc_properties_error", select + "TOKENIZE('',%s,%s)",
                       (parser, '[{"output":"bad"}]'), True)
        for i, name in enumerate((None, " IK ", "BeNg", "", "missing", "ik.1")):
            yield case(str(batch) + "/parser/" + str(i), select + "TOKENIZE(%s,%s)",
                       ("HELLO hello 数据库", name), True)
        yield case(str(batch) + "/default_parser", select + "TOKENIZE(%s)", ("HELLO hello",))
        yield case(str(batch) + "/varying_rows", select + "id,TOKENIZE(doc,parser,config) "
                   "FROM inputs ORDER BY id")
        yield case(str(batch) + "/persisted", select + "id,j,JSON_LENGTH(j),"
                   "JSON_EXTRACT(j,'$.doc_len'),JSON_EXTRACT(j,'$.tokens') FROM saved ORDER BY id")
        yield case(str(batch) + "/two_results", select + "id,TOKENIZE(doc,parser),"
                   "TOKENIZE(doc,parser,'[{\"output\":\"all\"}]') FROM inputs ORDER BY id")
    for charset, literal in (("latin1", "E94341464520636166E9"),
                             ("gbk", "D6D0CEC42041424320616263"),
                             ("utf16", "4E2D658700200041004200430020006100620063"),
                             ("utf8mb4", "C080"), ("utf8mb4", "E282")):
        for parser in ("beng", "ik"):
            for output in ("default", "all"):
                yield case("charset/" + charset + "/" + literal + "/" + parser + "/" + output,
                           "SELECT TOKENIZE(CONVERT(X'" + literal + "' USING " + charset
                           + "),%s,%s)", (parser, '[{"output":"' + output + '"}]'), True)
    for i, (parser, config) in enumerate((
        ("ik", '[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]'),
        ("beng", '[{"output":"default"}]'), ("ngram2", '[{"output":"all"}]'),
        ("ik", "[]"), ("space", '[{"output":"all"}]'), ("beng", "[]"))):
        yield case("prepared/" + str(i), "EXECUTE tokenize_r14 USING @r14_doc,@r14_parser,@r14_config",
                   before=[{"sql": "SET @r14_doc=%s,@r14_parser=%s,@r14_config=%s",
                            "params": ["HELLO hello café 数据库 搜索 123个", parser, config]}])


def input_rows():
    corpus = texts()
    rows = []
    for i in range(1, 258):
        _, doc = corpus[(i * 13) % len(corpus)]
        parser = PARSERS[i % len(PARSERS)]
        config = '[{"output":"' + ("all" if i % 2 else "default") + '"}]'
        rows.append((i, doc, parser, config))
    return rows


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


def workload_hash(workload):
    definition = {"cases": workload, "inputs": input_rows(), "strict_mode": STRICT_MODE}
    return hashlib.sha256(json.dumps(definition, ensure_ascii=True, allow_nan=False,
                                     separators=(",", ":")).encode()).hexdigest()


def query(cursor, case):
    import pymysql
    for before in case["before"]:
        cursor.execute(before["sql"], tuple(before["params"]))
    try:
        cursor.execute(case["sql"], tuple(case["params"]))
        result = {"columns": [[cell(v) for v in column] for column in cursor.description],
                  "rows": [[cell(v) for v in row] for row in cursor.fetchall()]}
    except pymysql.MySQLError as error:
        code = error.args[0] if error.args else None
        if not case["allow_error"] or not isinstance(code, int) or code <= 0 or 2000 <= code < 3000:
            raise RuntimeError("Required success case failed: " + case["id"]) from error
        result = {"error_number": code, "message": cell(error.args[1])}
    cursor.execute("SHOW WARNINGS")
    result["warnings"] = [[cell(v) for v in row] for row in cursor.fetchall()]
    return {"id": case["id"], "result": result}


def capture(options, workload):
    import pymysql
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=1000000000")
            cursor.execute("SET SESSION sql_mode=%s", (STRICT_MODE,))
            cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)
            cursor.execute("CREATE DATABASE " + DATABASE + " CHARACTER SET utf8mb4")
            try:
                cursor.execute("USE " + DATABASE)
                # The independently verified original rejects IK on LOB
                # locators. VARCHAR keeps this builder-lifetime test on the
                # actual textual input; the corpus fits without truncation.
                cursor.execute("CREATE TABLE inputs(id INT PRIMARY KEY,doc VARCHAR(32768),"
                               "parser VARCHAR(16),config VARCHAR(1024) NOT NULL)")
                cursor.executemany("INSERT INTO inputs VALUES(%s,%s,%s,%s)", input_rows())
                cursor.execute("CREATE TABLE saved(id INT PRIMARY KEY,j JSON)")
                cursor.execute("INSERT INTO saved SELECT id,TOKENIZE(doc,parser,config) FROM inputs")
                cursor.execute("PREPARE tokenize_r14 FROM 'SELECT TOKENIZE(?,?,?)'")
                try:
                    return [query(cursor, case) for case in workload]
                finally:
                    cursor.execute("DEALLOCATE PREPARE tokenize_r14")
            finally:
                cursor.execute("DROP DATABASE IF EXISTS " + DATABASE)


def compare(expected, actual, path="results"):
    if type(expected) is not type(actual):
        raise AssertionError((path, "type mismatch"))
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError((path, "keys differ"))
        for key in expected:
            compare(expected[key], actual[key], path + "." + key)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise AssertionError((path, "length differs", len(expected), len(actual)))
        for i, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, path + "[" + str(i) + "]")
    elif expected != actual:
        raise AssertionError((path, expected, actual))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, choices=(2891,), default=2891)
    parser.add_argument("--user", default="root")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--record", type=Path, metavar="NEW_PATH")
    mode.add_argument("--expected", type=Path,
                      default=Path(__file__).with_name("tokenize_json_nodes_expected.json.gz"))
    parser.add_argument("--record-source-commit")
    parser.add_argument("--record-source-binary-sha256")
    args = parser.parse_args()
    workload = list(cases())
    signature = workload_hash(workload)
    if args.record is not None:
        if (os.path.lexists(args.record) or not args.record.parent.is_dir()
                or args.record_source_commit != OFFICIAL
                or not re.fullmatch(r"[0-9a-fA-F]{64}", args.record_source_binary_sha256 or "")):
            parser.error("--record requires a new path, official baseline commit and verified original binary SHA256")
    else:
        if args.record_source_commit or args.record_source_binary_sha256:
            parser.error("Provenance options require --record")
        expected = json.loads(gzip.decompress(args.expected.read_bytes()))
        if (expected.get("format") != 1 or expected.get("workload_sha256") != signature
                or expected.get("original", {}).get("commit") != OFFICIAL
                or not re.fullmatch(r"[0-9a-f]{64}", expected.get("original", {}).get("binary_sha256", ""))
                or [r["id"] for r in expected.get("results", [])] != [c["id"] for c in workload]):
            raise ValueError("Incomplete, unrelated or unverified independent-original fixture")
    actual = capture(dict(host=args.host, port=args.port, user=args.user,
                          password=os.getenv("SEEKDB_TEST_PASSWORD", ""), charset="utf8mb4",
                          use_unicode=False, autocommit=True, connect_timeout=10,
                          read_timeout=180, write_timeout=180), workload)
    if args.record is not None:
        payload = {"format": 1, "workload_sha256": signature,
                   "original": {"commit": OFFICIAL,
                                "binary_sha256": args.record_source_binary_sha256.lower()},
                   "results": actual}
        raw = (json.dumps(payload, ensure_ascii=True, allow_nan=False,
                          separators=(",", ":")) + "\n").encode()
        with args.record.open("xb") as output:
            output.write(gzip.compress(raw, mtime=0))
        print("RECORDED complete independent-original results", args.record)
    else:
        compare(expected["results"], actual)
        print("PASS exact ordered JSON, metadata, warnings and SQL errors")
    print("cases=", len(actual))


if __name__ == "__main__":
    main()
