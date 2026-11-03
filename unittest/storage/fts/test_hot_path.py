#!/usr/bin/env python3
"""FTS hot-path regression against a snapshot recorded on unmodified seekdb.

Requires PyMySQL and a dedicated instance. Only fts_hot_path_regression is
created/dropped. Passwords are read from SEEKDB_TEST_PASSWORD. Run with --record
only on an independently verified baseline, then run candidates without it.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import random
import threading

import pymysql

DATABASE = 'fts_hot_path_regression'


def documents():
    values = [
        '', 'a', '123', 'hello world', 'HELLO hello Hello',
        '  \t\r\n\v\f punctuation!?! --_.,:+#@& ',
        'OceanBase 数据库 全文索引 分词器 tokenizer database',
        '中华人民共和国 北京大学 研究生命起源 南京市长江大桥',
        '一二两三四五六七八九十 百千万亿 12个 3.14公斤 1,234.50元',
        'ＡＢＣ１２３ ａｂｃ ＃＆＋－．＠＿，。 中文标点！',
        '한글 カタカナ ひらがな ㄱ ㇰ 日本語 한국어',
        'éÉ café CAFÉ Straße İİ ää ÖÖ ΩΩ ЖЖ',
        'emoji😀🧪𠀀乁尾部 ASCII end',
        'nul\x00inside\x01control\x7fend',
        'ab cd ef gh ij ' * 150,
        ('全文索引数据库 文档排序分词器 ' * 160) + '结尾 END',
        'x' * 4097,
        ''.join(chr(c) for c in range(128)),
    ]
    # Exercise both sides of the Unicode block predicates, not just benchmark text.
    boundaries = [0x10FF, 0x1100, 0x11FF, 0x1200, 0x303F, 0x3040, 0x309F,
                  0x30A0, 0x30FF, 0x3100, 0x312F, 0x3130, 0x318F, 0x3190,
                  0x31EF, 0x31F0, 0x31FF, 0x3200, 0x33FF, 0x3400, 0x4DBF,
                  0x4DC0, 0x4DFF, 0x4E00, 0x9FFF, 0xA000, 0xABFF, 0xAC00,
                  0xD7AF, 0xD7B0, 0xF8FF, 0xF900, 0xFAFF, 0xFB00, 0xFEFF,
                  0xFF00, 0xFFEF, 0xFFF0, 0x10000, 0x10FFFF]
    values.extend('before ' + chr(c) + ' 中文 after' for c in boundaries)
    rng = random.Random(1701)
    vocabulary = ['全文索引', '数据库', 'hello', 'HELLO', 'Query', '3.14', 'ＡＢＣ',
                  '123个', '中华人民共和国', '分词器', 'é', '😀', '한글', 'カタカナ']
    delimiters = [' ', '\t', '\n', '-', ',', '。', '\x00']
    values.extend(''.join(rng.choice(vocabulary) + rng.choice(delimiters)
                         for _ in range(rng.randrange(1, 55))) for _ in range(32))
    return values


def canonical(value):
    if isinstance(value, str) and value[:1] in ('{', '['):
        try:
            return canonical(json.loads(value))
        except (ValueError, TypeError):
            return value
    if isinstance(value, dict):
        return {k: canonical(v) for k, v in sorted(value.items())}
    if isinstance(value, (tuple, list)):
        normalized = [canonical(v) for v in value]
        return sorted(normalized, key=lambda v: json.dumps(v, ensure_ascii=True, sort_keys=True))
    return value


def capture(options, check_plans=False):
    result = {'tokens': [], 'queries': [], 'concurrent': [], 'predicate_queries': [], 'long_queries': []}
    corpus = documents()
    # Multiple independent connections exercise shared global parser metadata.
    barrier = threading.Barrier(6)
    def concurrent_worker(worker):
        with pymysql.connect(**options) as connection:
            with connection.cursor() as cursor:
                values = []
                barrier.wait(timeout=30)
                for index in range(24):
                    parser = 'ik' if (worker + index) % 2 else 'beng'
                    document = corpus[(worker * 13 + index) % len(corpus)]
                    cursor.execute('SELECT TOKENIZE(%s,%s,%s) AS tokens', (document, parser, '[{"output":"all"}]'))
                    values.append([parser, canonical(cursor.fetchone()[0])])
                return values
    with ThreadPoolExecutor(max_workers=6) as pool:
        result['concurrent'] = list(pool.map(concurrent_worker, range(6)))
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute('SET ob_query_timeout=1000000000')
            for parser, properties in [('ik', '[{"output":"all"}]'),
                                       ('ik', '[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]'),
                                       ('beng', '[{"output":"all"}]'),
                                       ('space', '[{"output":"all"}]')]:
                for index, document in enumerate(corpus):
                    cursor.execute('SELECT TOKENIZE(%s,%s,%s) AS tokens', (document, parser, properties))
                    result['tokens'].append([parser, properties, index, canonical(cursor.fetchone()[0])])
            for charset in ['utf8mb4', 'utf16', 'utf16le', 'latin1', 'gbk', 'gb18030']:
                for parser in ['ik', 'beng']:
                    for document in ['Hello WORLD 数据库', 'ＡＢＣ 123 café', '中文\x00END']:
                        try:
                            cursor.execute('SELECT TOKENIZE(CONVERT(%s USING ' + charset + '),%s,%s) AS tokens',
                                           (document, parser, '[{"output":"all"}]'))
                            value = canonical(cursor.fetchone()[0])
                        except pymysql.MySQLError as error:
                            value = {'error_number': error.args[0]}
                        result['tokens'].append([charset, parser, document, value])
            # Retain error behavior for truncated and invalid UTF-8 input.
            for data in ['80', 'c0af', 'e4b8', 'f0808080', 'f4908080', 'ff']:
                for parser in ['ik', 'beng']:
                    try:
                        cursor.execute("SELECT TOKENIZE(CONVERT(UNHEX(%s) USING utf8mb4),%s) AS tokens", (data, parser))
                        value = canonical(cursor.fetchone()[0])
                    except pymysql.MySQLError as error:
                        value = {'error_number': error.args[0]}
                    result['tokens'].append(['invalid', data, parser, value])
            cursor.execute('DROP DATABASE IF EXISTS ' + DATABASE)
            cursor.execute('CREATE DATABASE ' + DATABASE + ' CHARACTER SET utf8mb4')
            cursor.execute('USE ' + DATABASE)
            cursor.execute('CREATE TABLE docs(id INT PRIMARY KEY, title VARCHAR(256), body TEXT, english TEXT)')
            rows = [(i+1, ('标题 数据库 ' if i % 2 else '标题 全文索引 ') + str(i), text,
                     'Database database QUERY performance audit ' + ('tokenizer ' if i % 3 else 'search '))
                    for i, text in enumerate(corpus[:18] + corpus[-12:])]
            cursor.executemany('INSERT INTO docs VALUES(%s,%s,%s,%s)', rows)
            cursor.execute('ALTER TABLE docs ADD FULLTEXT INDEX ik_all(title,body) WITH PARSER ik')
            cursor.execute('ALTER TABLE docs ADD FULLTEXT INDEX ik_body(body) WITH PARSER ik')
            cursor.execute('ALTER TABLE docs ADD FULLTEXT INDEX beng(english) WITH PARSER beng')
            terms = [('title,body', '数据库 全文索引'), ('body', '研究 生命'),
                     ('body', 'hello'), ('body', '3.14 公斤'), ('body', '不存在的检索词'),
                     ('body', 'OceanBase 中文'), ('english', 'database performance audit'),
                     ('english', '+database -tokenizer'), ('english', '"database query"')]
            def queries(stage):
                for columns, term in terms:
                    for mode in ['', ' IN BOOLEAN MODE']:
                        match = 'MATCH(' + columns + ') AGAINST(%s' + mode + ')'
                        cursor.execute('SELECT id,' + match + ' FROM docs WHERE ' + match + ' ORDER BY id', (term, term))
                        result['queries'].append([stage, columns, term, mode, list(cursor.fetchall())])
                        cursor.execute('SELECT id,' + match + ' AS score FROM docs WHERE ' + match + ' ORDER BY score DESC,id LIMIT 7', (term, term))
                        result['queries'].append([stage + '_limit', columns, term, mode, list(cursor.fetchall())])
                        # Exercise score-free predicates and consumers that require exact scores.
                        variants = [
                            'SELECT id FROM docs WHERE {m} ORDER BY id',
                            'SELECT COUNT(*) FROM docs WHERE {m}',
                            'SELECT id FROM docs WHERE {m}>0 ORDER BY id',
                            'SELECT id FROM docs WHERE 0<{m} ORDER BY id LIMIT 7',
                            'SELECT id FROM docs WHERE {m}>0 AND id>=4 ORDER BY id',
                            'SELECT id FROM docs WHERE {m}>0.1 ORDER BY id',
                            'SELECT id FROM docs WHERE {m}>=0.1 ORDER BY id',
                            'SELECT id,({m}>0) AS matched FROM docs WHERE {m} ORDER BY id',
                            'SELECT id,{m}+1 AS value FROM docs WHERE {m} ORDER BY id',
                            'SELECT id FROM docs WHERE {m} ORDER BY {m} DESC,id LIMIT 7',
                            'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {m} LIMIT 7) q',
                            'SELECT id FROM docs WHERE {m} AND (id<5 OR id>20) ORDER BY id',
                        ]
                        for index, variant in enumerate(variants):
                            sql = variant.format(m=match)
                            try:
                                cursor.execute(sql, (term,) * sql.count('%s'))
                            except pymysql.MySQLError as error:
                                raise AssertionError((stage, columns, term, mode, index, sql, error.args)) from error
                            result['predicate_queries'].append([stage, columns, term, mode, index, list(cursor.fetchall())])
                # Zero is a plan-cache constraint, not a property of the SQL text.
                prepared = "SELECT id FROM docs WHERE MATCH(english) AGAINST('database performance') > ? ORDER BY id"
                cursor.execute('PREPARE score_threshold FROM %s', (prepared,))
                try:
                    for threshold in [0, 0.1, 1, 0, 100, 0]:
                        cursor.execute('SET @score_threshold=%s', (threshold,))
                        cursor.execute('EXECUTE score_threshold USING @score_threshold')
                        result['predicate_queries'].append([stage, 'prepared_threshold', threshold, list(cursor.fetchall())])
                finally:
                    cursor.execute('DEALLOCATE PREPARE score_threshold')
                # More than 256 distinct tokens selects the TaaT fallback.
                # Existing terms overlap, so document-id deduplication is required.
                long_term = 'database performance ' + ' '.join('word' + str(i) for i in range(300))
                for variant in [
                    "SELECT id FROM docs WHERE MATCH(english) AGAINST(%s) ORDER BY id",
                    "SELECT COUNT(*) FROM docs WHERE MATCH(english) AGAINST(%s)",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(english) AGAINST(%s) LIMIT 7) q",
                ]:
                    cursor.execute(variant, (long_term,))
                    result['long_queries'].append([stage, variant, list(cursor.fetchall())])
                for variant in [
                    "SELECT id FROM docs WHERE MATCH(english) AGAINST('database') OR id=1 ORDER BY id",
                    "SELECT id,CASE WHEN MATCH(english) AGAINST('database') THEN 1 ELSE 0 END FROM docs ORDER BY id",
                    "SELECT SUM(MATCH(english) AGAINST('database')) FROM docs",
                    "SELECT id FROM docs WHERE MATCH(english) AGAINST('database') IS NULL ORDER BY id",
                    "SELECT id FROM docs WHERE MATCH(english) AGAINST('database') AND EXISTS(SELECT 1 FROM docs other WHERE other.id=docs.id) ORDER BY id",
                ]:
                    cursor.execute(variant)
                    result['long_queries'].append([stage, variant, list(cursor.fetchall())])
                if check_plans:
                    for sql, expected in [
                        ("SELECT COUNT(*) FROM docs WHERE MATCH(english) AGAINST('database')", False),
                        ("SELECT id,MATCH(english) AGAINST('database') FROM docs WHERE MATCH(english) AGAINST('database')", True),
                        ("SELECT id FROM docs WHERE MATCH(english) AGAINST('database') ORDER BY MATCH(english) AGAINST('database') DESC", True),
                        ("SELECT id FROM docs WHERE MATCH(english) AGAINST('database')>0.1", True),
                        ("SELECT COUNT(*) FROM docs WHERE MATCH(english) AGAINST('database' IN BOOLEAN MODE)", True),
                        ("SELECT id FROM docs WHERE MATCH(english) AGAINST('database') ORDER BY id LIMIT 2,7", True),
                    ]:
                        cursor.execute('EXPLAIN ' + sql)
                        plan = '\\n'.join(str(row[0]) for row in cursor.fetchall())
                        if expected and 'has_functional_lookup=true' in plan:
                            assert 'calc_relevance=false' not in plan, (sql, plan)
                        else:
                            assert ('calc_relevance=' + str(expected).lower()) in plan, (sql, plan)

            queries('built')
            cursor.execute('UPDATE docs SET body=%s,english=%s WHERE id=1', ('数据库 hello 全文索引', 'audit tokenizer'))
            cursor.execute('DELETE FROM docs WHERE id=2')
            cursor.execute('INSERT INTO docs VALUES(1001,%s,%s,%s)', ('新标题', '数据库研究 全文索引 HELLO', 'database query performance'))
            queries('mutated')
            cursor.execute('START TRANSACTION')
            cursor.execute('UPDATE docs SET body=%s WHERE id=3', ('数据库 未提交',))
            queries('transaction')
            cursor.execute('ROLLBACK')
            queries('rolled_back')
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute('DROP DATABASE ' + DATABASE)
    return result


def compare(expected, actual, path='root'):
    if isinstance(expected, float) and isinstance(actual, (int, float)):
        assert math.isclose(expected, actual, rel_tol=1e-12, abs_tol=1e-12), (path, expected, actual)
    elif isinstance(expected, dict):
        assert expected.keys() == actual.keys(), path
        for key in expected:
            compare(expected[key], actual[key], path + '.' + key)
    elif isinstance(expected, (tuple, list)):
        assert len(expected) == len(actual), (path, len(expected), len(actual))
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, f'{path}[{index}]')
    else:
        assert expected == actual, (path, expected, actual)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--user', default='root')
    parser.add_argument('--record', type=Path)
    parser.add_argument('--expected', type=Path, default=Path(__file__).with_name('hot_path_expected.json'))
    parser.add_argument('--check-plans', action='store_true', help='Assert candidate score-elision decisions')
    args = parser.parse_args()
    options = dict(host=args.host, port=args.port, user=args.user,
                   password=os.environ.get('SEEKDB_TEST_PASSWORD', ''), charset='utf8mb4', autocommit=True,
                   read_timeout=180, write_timeout=180)
    actual = capture(options, args.check_plans)
    if args.record:
        args.record.write_text(json.dumps(actual, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
        print('Recorded baseline:', args.record)
    else:
        compare(json.loads(args.expected.read_text(encoding='utf-8')), actual)
        print('PASS: complete token/frequency/doc-length and ranked MATCH results match baseline')
    print(f"Covered {len(actual['tokens'])} token cases, {len(actual['queries'])} ranked queries, {len(actual['predicate_queries']) + len(actual['long_queries'])} predicate query results, and 144 concurrent parses")


if __name__ == '__main__':
    main()
