#!/usr/bin/env python3
"""Verify complete stopword filtering under concurrent parser evaluations.

Expected token frequencies and document lengths are derived from literal ASCII
inputs independently of the database. Uses only TOKENIZE; no table is changed.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
import threading

import pymysql

STOPWORDS = ('a about an are as at be by com de en for from how i in is it la '
             'of on or that the this to was what when where who will with und www').split()
CONFIG = '[{"output":"all"},{"additional_args":[{"min_token_size":1}]}]'


def cases():
    retained = ['and', 'keepword', 'x', 'ox', 'omega']
    words = STOPWORDS + retained + [word.upper() for word in STOPWORDS + retained]
    words += ['keepword', 'and', 'x'] * 3
    inputs = [[], STOPWORDS, words, list(reversed(words)), retained * 7]
    result = []
    for parser in ['beng', 'space']:
        for words in inputs:
            expected = Counter(word.lower() for word in words if word.lower() not in STOPWORDS)
            for collation in ['utf8mb4_general_ci', 'utf8mb4_bin', 'utf8mb4_unicode_ci']:
                result.append((parser, collation, ' '.join(words), dict(expected), sum(expected.values())))
    return result


def run(options, inputs, barrier=None):
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            if barrier:
                barrier.wait(timeout=30)
            for parser, collation, text, expected, doc_len in inputs:
                cursor.execute('SELECT TOKENIZE(CONVERT(%s USING utf8mb4) COLLATE ' + collation + ',%s,%s) AS tokens',
                               (text, parser, CONFIG))
                actual = json.loads(cursor.fetchone()[0])
                # Check every token and frequency; no duplicates or extra fields.
                tokens = actual['tokens']
                assert len(tokens) == len(expected), (parser, collation, actual, expected)
                merged = {}
                for token in tokens:
                    assert len(token) == 1
                    key, value = next(iter(token.items()))
                    assert key not in merged
                    merged[key] = value
                assert merged == expected, (parser, collation, merged, expected)
                assert actual == {'tokens': tokens, 'doc_len': doc_len}, (parser, collation, actual, doc_len)
    return len(inputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--user', default='root')
    args = parser.parse_args()
    options = dict(host=args.host, port=args.port, user=args.user,
                   password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                   charset='utf8mb4', autocommit=True)
    inputs = cases()
    checks = run(options, inputs)
    barrier = threading.Barrier(8)
    with ThreadPoolExecutor(max_workers=8) as pool:
        checks += sum(pool.map(lambda _: run(options, inputs * 2, barrier), range(8)))
    print(f'PASS: {checks} complete stopword token/frequency/doc-length checks across three collations and eight sessions')


if __name__ == '__main__':
    main()
