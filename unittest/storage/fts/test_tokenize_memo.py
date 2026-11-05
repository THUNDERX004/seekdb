#!/usr/bin/env python3
"""Bounded TOKENIZE memo regression using the official-original config fixture."""
import argparse
import json
import os
from pathlib import Path

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=2891)
    args = parser.parse_args()
    if args.port == 2881:
        parser.error('Use a dedicated test instance')
    fixture = json.loads(Path(__file__).with_name('tokenize_config_expected.json').read_text(encoding='utf-8'))
    configs = {None, '[]', '[{"output":"all"}]', '[{"output":"bad"}]',
               '[{"additional_args":[{"min_token_size":1,"max_token_size":10}]}]'}
    cases = [(values, expected) for values, expected in fixture
             if isinstance(values, list) and len(values) in (2, 3) and values[0] != 'prepared'
             and values[1] in ('ik', 'beng', 'space', 'missing', None)
             and (None if len(values) == 2 else values[2]) in configs]
    empty_cases = [(values, expected) for values, expected in cases if values[0] == '']
    cases += [([None] + values[1:], expected) for values, expected in empty_cases]
    cases += [([' ' * length] + values[1:], expected)
              for values, expected in empty_cases if values[1] == 'beng'
              for length in (1023, 1024, 1025, 5000)]
    with pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                         password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
                         charset='utf8mb4', autocommit=True) as connection:
        with connection.cursor() as cursor:
            for values, expected in cases:
                expressions = ['%s'] * len(values)
                if values[0] is None:
                    expressions[0] = 'CONVERT(%s USING utf8mb4)'
                sql = 'SELECT TOKENIZE(' + ','.join(expressions) + ')'
                # Consecutive calls exercise cold/hit without hiding error changes.
                for repeat in range(2):
                    try:
                        cursor.execute(sql, values)
                        actual = [list(row) for row in cursor.fetchall()]
                    except pymysql.MySQLError as error:
                        actual = {'error': error.args[0]}
                    if actual != expected:
                        raise AssertionError((values, repeat, actual, expected))
            rows = [(values, expected) for values, expected in cases
                    if len(values) == 3 and not isinstance(expected, dict)
                    and values[0] and len(values[0]) < 1024][:18]
            selections, params, expected_rows = [], [], []
            for row_id, (values, expected) in enumerate(rows * 3):
                selections.append('SELECT %s AS row_id,%s AS doc,%s AS parser,%s AS config')
                params.extend([row_id] + values)
                expected_rows.append([row_id, expected[0][0]])
            cursor.execute('SELECT row_id,TOKENIZE(doc,parser,config) FROM ('
                           + ' UNION ALL '.join(selections) + ') q ORDER BY row_id', params)
            actual = [list(row) for row in cursor.fetchall()]
            if actual != expected_rows:
                raise AssertionError(('row result lifetime', actual, expected_rows))
    print('PASS memo cases=' + str(len(cases)) + ' repeated=2 computed_rows=' + str(len(expected_rows)))


if __name__ == '__main__':
    main()
