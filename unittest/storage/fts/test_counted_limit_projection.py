#!/usr/bin/env python3
"""Regression for unused projections in COUNT(*) over an FTS limited view.

Requires PyMySQL and a dedicated test instance. Compares limited cardinalities
with complete unbounded result sets, and value consumers with complete ranked
rows. Only fts_counted_limit_regression is created/dropped.
"""
import argparse
import json
import os
from pathlib import Path
import re

import pymysql

DATABASE = 'fts_counted_limit_regression'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--user', default='root')
    parser.add_argument('--check-plans', action='store_true')
    parser.add_argument('--plans', type=Path)
    args = parser.parse_args()
    assert args.port != 2881, 'Use a dedicated test instance'
    options = dict(host=args.host, port=args.port, user=args.user,
                   password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                   charset='utf8mb4', autocommit=True)
    records, plans = [], []
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cur:
            cur.execute('SET ob_query_timeout=1000000000')
            cur.execute('DROP DATABASE IF EXISTS ' + DATABASE)
            cur.execute('CREATE DATABASE ' + DATABASE + ' CHARACTER SET utf8mb4')
            try:
                cur.execute('USE ' + DATABASE)
                cur.execute('CREATE TABLE docs(id BIGINT PRIMARY KEY, tag INT, body TEXT, english TEXT)')
                rows = [(i * 7 + 3, None if i % 5 == 0 else i % 11,
                         '全文索引 数据库 ' + ('稀有词 ' if i % 3 == 0 else '文档 ') + '测试 ' * (i % 7 + 1),
                         'common document ' + ('needle ' if i % 3 == 0 else 'other ') + 'token ' * (i % 7 + 1))
                        for i in range(513)]
                cur.executemany('INSERT INTO docs VALUES(%s,%s,%s,%s)', rows)
                cur.execute('ALTER TABLE docs ADD FULLTEXT INDEX fti_ik(body) WITH PARSER ik')
                cur.execute('ALTER TABLE docs ADD FULLTEXT INDEX fti_beng(english) WITH PARSER beng')
                cases = [('body', '数据库'), ('body', '稀有词'), ('body', '不存在的长词'),
                         ('body', ''), ('english', 'common'), ('english', 'needle'),
                         ('english', 'absentuniqueword'), ('english', '')]
                boundaries = [(0, 0), (1, 0), (20, 0), (20, 5), (512, 1),
                              (1000, 0), (20, 512), (20, 1000)]
                totals = {}
                for column, term in cases:
                    match = 'MATCH(' + column + ') AGAINST(%s)'
                    cur.execute('SELECT id,tag,' + match + ' AS score FROM docs WHERE ' + match + ' ORDER BY score DESC,id', (term, term))
                    complete = list(cur.fetchall())
                    totals[(column, term)] = len(complete)
                    # The result of the full MATCH is also checked against the count path.
                    cur.execute('SELECT COUNT(*) FROM docs WHERE ' + match, (term,))
                    assert cur.fetchone() == (len(complete),)
                    for limit, offset in boundaries:
                        suffix = ' LIMIT ' + str(limit) + ' OFFSET ' + str(offset)
                        expected_count = len(complete[offset:offset + limit])
                        for select_list in ('id', 'id,tag', 'tag'):
                            sql = 'SELECT COUNT(*) FROM (SELECT ' + select_list + ' FROM docs WHERE ' + match + suffix + ') counted'
                            cur.execute(sql, (term,))
                            actual = cur.fetchall()
                            assert actual == ((expected_count,),), (column, term, select_list, limit, offset, actual)
                            records.append([column, term, select_list, limit, offset, actual])
                        # Explicit ordering and LIMIT/OFFSET remain semantic constraints.
                        inner = 'SELECT id,tag FROM docs WHERE ' + match + ' ORDER BY id' + suffix
                        expected = sorted(complete, key=lambda row: row[0])[offset:offset + limit]
                        for aggregate, wanted in [
                            ('SUM(id)', sum(row[0] for row in expected) if expected else None),
                            ('COUNT(tag)', sum(row[1] is not None for row in expected)),
                            ('COUNT(DISTINCT tag)', len({row[1] for row in expected if row[1] is not None}))]:
                            cur.execute('SELECT ' + aggregate + ' FROM (' + inner + ') ranked', (term,))
                            actual = cur.fetchall()
                            assert actual == ((wanted,),), (column, term, aggregate, limit, offset, actual, wanted)
                            records.append([column, term, aggregate, limit, offset, str(actual[0][0])])
                    sql = 'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE ' + match + ' LIMIT 20) counted'
                    cur.execute('EXPLAIN ' + sql, (term,))
                    plan = '\n'.join(str(row[0]) for row in cur.fetchall())
                    plans.append(dict(column=column, term=term, plan=plan))
                    if args.check_plans and term in ('数据库', 'common'):
                        assert 'TEXT RETRIEVAL SCAN' in plan and 'is_index_back=false' in plan, plan
                        assert 'is_index_back=true' not in plan, plan
                # Repeated server-side prepared executions must not capture the
                # first LIMIT/OFFSET values in a reused rewritten plan.
                prepared = "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(english) AGAINST(?) LIMIT ? OFFSET ?) counted"
                cur.execute('PREPARE counted_stmt FROM %s', (prepared,))
                try:
                    for term, limit, offset in [('common', 20, 0), ('needle', 1, 5),
                                                ('common', 0, 0), ('needle', 20, 1000),
                                                ('common', 20, 512), ('needle', 20, 0)] * 2:
                        cur.execute('SET @counted_term=%s,@counted_limit=%s,@counted_offset=%s', (term, limit, offset))
                        cur.execute('EXECUTE counted_stmt USING @counted_term,@counted_limit,@counted_offset')
                        actual = cur.fetchall()
                        wanted = min(limit, max(totals[('english', term)] - offset, 0))
                        assert actual == ((wanted,),), (term, limit, offset, actual, wanted)
                        records.append(['prepared', term, limit, offset, actual])
                finally:
                    cur.execute('DEALLOCATE PREPARE counted_stmt')
                for hint in ('NO_REWRITE', 'NO_PROJECT_PRUNE'):
                    sql = "SELECT COUNT(*) FROM (SELECT /*+ " + hint + " */ id FROM docs WHERE MATCH(english) AGAINST('common') LIMIT 20) counted"
                    cur.execute(sql)
                    actual = cur.fetchall()
                    assert actual == ((20,),)
                    records.append(['hint', hint, actual])
                    cur.execute('EXPLAIN EXTENDED ' + sql)
                    plan = '\n'.join(str(row[0]) for row in cur.fetchall())
                    # The check concerns an applied/generated rewrite hint,
                    # rather than the text of NO_PROJECT_PRUNE itself.
                    assert re.search(r'(?<!NO_)PROJECT_PRUNE\(', plan) is None, plan
                    assert 'is_index_back=true' in plan, plan
                    plans.append(dict(hint=hint, plan=plan))
                sql = "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(english) AGAINST('common') LIMIT 20) counted"
                cur.execute('EXPLAIN EXTENDED ' + sql)
                plan = '\n'.join(str(row[0]) for row in cur.fetchall())
                outline = re.search(r'\bBEGIN_OUTLINE_DATA\b([\s\S]*?)\bEND_OUTLINE_DATA\b', plan)
                assert outline is not None, plan
                hint = 'BEGIN_OUTLINE_DATA ' + outline.group(1) + ' END_OUTLINE_DATA'
                replay = 'SELECT /*+ ' + hint + ' */' + sql[len('SELECT'):]
                cur.execute(replay)
                actual = cur.fetchall()
                assert actual == ((20,),)
                records.append(['outline-replay', actual])
                cur.execute('EXPLAIN EXTENDED ' + replay)
                replay_plan = '\n'.join(str(row[0]) for row in cur.fetchall())
                if args.check_plans:
                    assert 'PROJECT_PRUNE(' in plan, plan
                    assert 'is_index_back=false' in replay_plan and 'is_index_back=true' not in replay_plan, replay_plan
                plans.append(dict(outline_source=plan, outline_replay=replay_plan))
            finally:
                cur.execute('DROP DATABASE IF EXISTS ' + DATABASE)
    if args.plans:
        with args.plans.open('x', encoding='utf-8') as stream:
            json.dump(plans, stream, ensure_ascii=False, indent=2); stream.write('\n')
    for number, record in enumerate(records):
        print('CASE ' + str(number) + ' ' + json.dumps(record, ensure_ascii=False, default=str))
    print('PASS counted-limit-projection cases=' + str(len(records)))

if __name__ == '__main__':
    main()
