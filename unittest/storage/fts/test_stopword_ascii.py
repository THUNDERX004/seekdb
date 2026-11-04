#!/usr/bin/env python3
"""Record on the official original, then compare complete TOKENIZE results.

Only SELECT statements are sent. No expected SQL output is generated locally.
Stopwords and case variants are derived from the unchanged built-in source list.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
OFFICIAL_BASELINE = "5bce9e06d1281347ad61344e4b01fab9158c58a2"


def builtins(source_root: Path) -> tuple[list[str], str]:
    text = (source_root / "src/storage/fts/ob_fts_stop_word.h").read_text(encoding="utf-8")
    body = re.search(r"static const char ob_stop_word_list\[\]\[FTS_STOP_WORD_MAX_LENGTH\] = \{(.*?)\};",
                     text, re.S)
    if body is None:
        raise ValueError("Cannot find the original stopword initializer")
    words = re.findall(r'"([A-Za-z]+)"', body.group(1))
    remainder = re.sub(r'"[A-Za-z]+"|[\s,]', "", body.group(1))
    if not words or remainder:
        raise ValueError("Stopword initializer changed; review this corpus extractor")
    raw = json.dumps(words, separators=(",", ":")).encode()
    return words, hashlib.sha256(raw).hexdigest()


def cases(words: list[str]):
    unique = sorted(set(words))
    variants = sorted({"".join(chars) for word in unique
                       for chars in itertools.product(*[(c.lower(), c.upper()) for c in word])})
    misses = sorted({candidate for word in unique for candidate in
                     [word + "z", "z" + word, word[:-1], word[1:]]
                     if candidate and candidate.lower() not in unique})
    groups = ["", " ".join(words), " ".join(variants), " ".join(misses),
              " ".join(reversed(variants)),
              " ".join((misses + variants + misses[:8]) * 3)]
    # Each built-in is tested separately; one/two-character entries must not be
    # lost behind default min_token_size. Mutations include prefixes/suffixes.
    groups += [f"{word} {word.upper()} {word} {misses[i % len(misses)]}"
               for i, word in enumerate(unique)]
    groups += [
        "thé THE thé ábout ABOUT à an án a i İ ı ＴＨＥ café CAFÉ",
        "the\u0301 the\u00a0about\u2003with\u3000www",
        "the9 9the t_he the_ the-the the.the don't www.example.com",
        "the the\tABOUT\r\nwith\x00und\x01www\x7f of ",
        "the  the   the\t\tabout\nwith\rwww ",
        "x" * 9 + " " + "x" * 10 + " " + "x" * 84 + " " + "x" * 85,
        "cafe café CAFE café hello HELLO hello and AND and",
    ]
    configs = [
        '[{"output":"all"},{"additional_args":[{"min_token_size":1}]}]',
        '[{"output":"all"}]',
        '[{"output":"all"},{"additional_args":[{"min_token_size":3,"max_token_size":10}]}]',
        '[{"output":"all"},{"additional_args":[{"min_token_size":16,"max_token_size":84}]}]',
        '[{"output":"default"},{"additional_args":[{"min_token_size":1}]}]',
        # Keep the original invalid multi-key cases above and add valid
        # separate min/max option entries for both successful boundary paths.
        '[{"output":"all"},{"additional_args":[{"min_token_size":3},{"max_token_size":10}]}]',
        '[{"output":"all"},{"additional_args":[{"min_token_size":16},{"max_token_size":84}]}]',
    ]
    for parser in ("beng", "space"):
        for collation in ("utf8mb4_general_ci", "utf8mb4_bin", "utf8mb4_unicode_ci"):
            for index, text in enumerate(groups):
                for config in configs:
                    yield {"id": [parser, collation, index, config],
                           "sql": "SELECT TOKENIZE(CONVERT(%s USING utf8mb4) COLLATE "
                                  + collation + ",%s,%s) AS tokens",
                           "params": [text, parser, config]}
    # These inputs exercise original conversion/error paths, outside the three
    # whitelisted collations. Exact errors are part of the original fixture.
    for charset in ("binary", "latin1", "utf16", "utf16le", "gbk", "gb18030"):
        for parser in ("beng", "space"):
            for text in (" ".join(variants), groups[-1], groups[-7]):
                yield {"id": [parser, charset, text],
                       "sql": "SELECT TOKENIZE(CONVERT(%s USING " + charset + "),%s,%s) AS tokens",
                       "params": [text, parser, configs[0]]}
    # Malformed UTF8 is sent as bytes, never predecoded by Python. SQL conversion
    # may sanitize it, so direct checker C++ cases are supplied separately.
    for raw in (b"the\x00about", b"the\xc0\x80", b"\xed\xa0\x80the",
                b"\xf4\x90\x80\x80the", b"the\xe0\x80\x80", b"\xffABOUT"):
        for parser in ("beng", "space"):
            yield {"id": [parser, "raw_utf8", raw.hex()],
                   "sql": "SELECT TOKENIZE(CONVERT(%s USING utf8mb4),%s,%s) AS tokens",
                   "params": [raw, parser, configs[0]]}


def capture(connection, queries, database_error):
    results = []
    with connection.cursor() as cursor:
        for case in queries:
            try:
                cursor.execute(case["sql"], case["params"])
                rows = []
                for row in cursor.fetchall():
                    # Preserve token-array order, spelling, frequencies, lengths,
                    # all output fields and all result cells; never sort tokens.
                    # Raw protocol bytes also retain original malformed UTF8 output.
                    rows.append([{"raw_bytes_hex": cell.hex()} if isinstance(cell, bytes)
                                 else json.loads(cell) if isinstance(cell, str)
                                 and cell[:1] in ("{", "[") else cell for cell in row])
                result = {"rows": rows}
            except database_error as error:
                code = error.args[0] if error.args else None
                # Transport/client failures must never become expected results.
                if not isinstance(code, int) or code <= 0 or 2000 <= code < 3000:
                    raise
                result = {"error": code}
            results.append({"id": case["id"], "result": result})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, choices=[2891], default=2891)
    parser.add_argument("--user", default="root")
    parser.add_argument("--fixture", type=Path, default=HERE / "stopword_ascii_expected.json")
    parser.add_argument("--record", type=Path)
    parser.add_argument("--baseline-revision")
    parser.add_argument("--baseline-binary-sha256")
    args = parser.parse_args()
    if args.record and (args.baseline_revision != OFFICIAL_BASELINE
                        or not re.fullmatch(r"[0-9a-f]{64}", args.baseline_binary_sha256 or "")):
        parser.error("Recording requires the official baseline revision and its verified binary SHA-256")
    if args.record and os.path.lexists(args.record):
        parser.error("Refusing to overwrite an existing fixture")
    words, words_hash = builtins(args.source_root.resolve())
    queries = list(cases(words))
    expected = None
    if not args.record:
        expected = json.loads(args.fixture.read_text(encoding="utf-8"))
        if expected["official_baseline"] != OFFICIAL_BASELINE or expected["stopword_list_sha256"] != words_hash:
            raise ValueError("Fixture baseline/list provenance mismatch")
        if len(expected["cases"]) != len(queries):
            raise ValueError("Case set changed; record it again on the official original")
    import pymysql
    with pymysql.connect(host=args.host, port=args.port, user=args.user,
                         password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                         charset="utf8mb4", use_unicode=False, autocommit=True) as connection:
        results = capture(connection, queries, pymysql.MySQLError)
    if args.record:
        report = {"official_baseline": args.baseline_revision,
                  "baseline_binary_sha256": args.baseline_binary_sha256,
                  "stopword_list_sha256": words_hash, "cases": results}
        with args.record.open("x", encoding="utf-8") as output:
            output.write(json.dumps(report, ensure_ascii=True, separators=(",", ":")) + "\n")
        print(f"RECORDED {len(results)} cases; binary identity supplied by parent")
    else:
        for index, (actual, original) in enumerate(zip(results, expected["cases"])):
            if actual != original:
                raise AssertionError((index, actual, original))
        print(f"PASS {len(results)} complete ordered TOKENIZE/error cases")


if __name__ == "__main__":
    main()
