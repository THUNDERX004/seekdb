#!/usr/bin/env python3
"""Verify independent document/query word maps under concurrent SQL sessions.

Uses only the dedicated fts_map_concurrency_regression database. IDs are
derived independently from document contents; complete ranking results must
also match the same query executed sequentially before the concurrent phase.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import threading

import pymysql

DATABASE = 'fts_map_concurrency_regression'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--user', default='root')
    args = parser.parse_args()
    options = dict(host=args.host, port=args.port, user=args.user,
                   password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                   charset='utf8mb4', autocommit=True)
    documents = [(i, f'cohort{i % 7} commonword cohort{i % 7} repeatword')
                 for i in range(1, 128)]
    terms = [f'cohort{i}' for i in range(7)] + [
        f'cohort{i} cohort{(i + 3) % 7} cohort{i}' for i in range(7)] + ['absentword']
    rank_sql = ('SELECT id,MATCH(body) AGAINST(%s) AS score FROM docs '
                'WHERE MATCH(body) AGAINST(%s) ORDER BY score DESC,id')
    ids_sql = 'SELECT id FROM docs WHERE MATCH(body) AGAINST(%s) ORDER BY id'
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            cursor.execute('DROP DATABASE IF EXISTS ' + DATABASE)
            cursor.execute('CREATE DATABASE ' + DATABASE)
            cursor.execute('USE ' + DATABASE)
            cursor.execute('CREATE TABLE docs(id INT PRIMARY KEY,body TEXT)')
            cursor.executemany('INSERT INTO docs VALUES(%s,%s)', documents)
            cursor.execute('ALTER TABLE docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng')
            sequential = []
            for term in terms:
                expected_ids = tuple((i,) for i, text in documents
                                     if set(text.split()) & set(term.split()))
                cursor.execute(ids_sql, (term,))
                assert cursor.fetchall() == expected_ids, (term, expected_ids)
                cursor.execute(rank_sql, (term, term))
                ranked = cursor.fetchall()
                assert sorted(row[0] for row in ranked) == [row[0] for row in expected_ids]
                sequential.append((expected_ids, ranked))
            cursor.execute('SELECT TOKENIZE(%s,%s,%s)',
                           (documents[0][1], 'beng', '[{"output":"all"}]'))
            expected_tokens = cursor.fetchone()
        barrier = threading.Barrier(8)

        def worker(index):
            with pymysql.connect(**options, database=DATABASE) as session:
                with session.cursor() as query:
                    barrier.wait(timeout=30)
                    for iteration in range(60):
                        n = (index * 7 + iteration) % len(terms)
                        term = terms[n]
                        query.execute(ids_sql, (term,))
                        assert query.fetchall() == sequential[n][0], (index, iteration, 'ids')
                        query.execute(rank_sql, (term, term))
                        assert query.fetchall() == sequential[n][1], (index, iteration, 'ranking')
                        query.execute('SELECT TOKENIZE(%s,%s,%s)',
                                      (documents[0][1], 'beng', '[{"output":"all"}]'))
                        assert query.fetchone() == expected_tokens, (index, iteration, 'tokens')
            return 60 * 3

        try:
            with ThreadPoolExecutor(max_workers=8) as pool:
                checks = sum(pool.map(worker, range(8)))
        finally:
            with connection.cursor() as cursor:
                cursor.execute('DROP DATABASE ' + DATABASE)
    print(f'PASS: {checks} concurrent complete ID/ranking/token checks across 8 sessions')


if __name__ == '__main__':
    main()
