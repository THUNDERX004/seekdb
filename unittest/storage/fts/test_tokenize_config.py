#!/usr/bin/env python3
"""Compare complete TOKENIZE configurations and errors with an original binary.

Requires PyMySQL and a dedicated seekdb instance. The fixture must be recorded
only on an independently verified original binary, never on the candidate.
"""
import argparse
import json
import os
from pathlib import Path

import pymysql


def capture(connection):
    results = []

    def query(cursor, sql, params):
        try:
            cursor.execute(sql, params)
            return [list(row) for row in cursor.fetchall()]
        except pymysql.MySQLError as error:
            return {'error': error.args[0]}

    with connection.cursor() as cursor:
        texts = ['', 'a an THE cafe café CAFÉ hello HELLO',
                 '中华人民共和国 研究生命起源 全文索引 OceanBase 123个',
                 'abc ' + 'x' * 85 + ' 1234 full-text éÉéÉ']
        options = [None, '[]', '[{"output":"all"}]',
                   '[{"output":"default"}]', '[{"additional_args":[]}]',
                   '[{"output":"all"},{"output":"default"}]',
                   '[{"additional_args":[{"unknown":1}]}]',
                   '[{"output":"bad"}]', '[{"output":1}]',
                   '[{"case":"lower"}]', '[{"stopwords":[]}]',
                   '[{"additional_args":{}}]', '[{"additional_args":[1]}]',
                   '[{"additional_args":[{"min_token_size":1,"max_token_size":10}]}]']
        parser_options = {
            'ik': [{'ik_mode': v} for v in ['smart', 'max_word', 'SMART', 'bad', 1]],
            'beng': [{'min_token_size': v} for v in [1, 3, 16, 0, 17, '3']]
                    + [{'max_token_size': v} for v in [10, 84, 9, 85]]
                    + [{'min_token_size': 16}, {'max_token_size': 10}],
            'space': [{'min_token_size': 1}, {'max_token_size': 10}],
            'ngram': [{'ngram_token_size': v} for v in [1, 2, 10, 0, 11]],
            'ngram2': [{'min_ngram_size': 1}, {'max_ngram_size': 1},
                       {'min_ngram_size': 16}, {'max_ngram_size': 17}],
        }
        for parser in ['ik', 'beng', 'space', 'ngram', 'ngram2', None,
                       ' IK ', 'BeNg', '', 'missing', 'ik.1']:
            parser_configs = options + [json.dumps([
                {'output': 'all'}, {'additional_args': [option]}])
                for option in parser_options.get(parser, [])]
            for text in texts:
                for config in parser_configs:
                    params = (text, parser) if config is None else (text, parser, config)
                    sql = 'SELECT TOKENIZE(' + ','.join(['%s'] * len(params)) + ')'
                    results.append([list(params), query(cursor, sql, params)])
        # Identical keys, accent/case equivalents, and latest-spelling retention.
        for collation in ['utf8mb4_general_ci', 'utf8mb4_unicode_ci', 'utf8mb4_bin']:
            for parser in ['beng', 'ik', 'space']:
                for text in ['café cafe CAFÉ cafe', 'cafe café CAFE café',
                             'ÉÉÉ ééé EEE eee', 'repeat ' * 150]:
                    sql = ('SELECT TOKENIZE(CONVERT(%s USING utf8mb4) COLLATE '
                           + collation + ',%s,\'[{"output":"all"}]\')')
                    results.append([[collation, parser, text], query(cursor, sql, (text, parser))])
        # Input, parser and options vary inside one evaluation context.
        sql = '''SELECT id,TOKENIZE(doc,parser,config) FROM (
          SELECT 1 AS id,'HELLO hello café' AS doc,'beng' AS parser,
                 '[{"output":"all"}]' AS config
          UNION ALL SELECT 2,'研究生命起源','ik',
                 '[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]'
          UNION ALL SELECT 3,'a an hello','beng',
                 '[{"additional_args":[{"min_token_size":1}]}]'
          UNION ALL SELECT 4,'中文分词','ngram','[]') q ORDER BY id'''
        results.append(['varying_rows', query(cursor, sql, ())])
        # Prepared execution must not retain a preceding parser or options.
        cursor.execute('PREPARE fts_config_stmt FROM \'SELECT TOKENIZE(?,?,?)\'')
        try:
            for parser, config in [('ik', '[{"additional_args":[{"ik_mode":"max_word"}]}]'),
                                   ('beng', '[{"output":"all"}]'), ('ik', '[]')]:
                cursor.execute('SET @fts_doc=%s,@fts_parser=%s,@fts_config=%s',
                               ('研究生命起源 HELLO hello café', parser, config))
                results.append([['prepared', parser, config], query(cursor,
                    'EXECUTE fts_config_stmt USING @fts_doc,@fts_parser,@fts_config', ())])
        finally:
            cursor.execute('DEALLOCATE PREPARE fts_config_stmt')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--user', default='root')
    parser.add_argument('--record', type=Path)
    args = parser.parse_args()
    with pymysql.connect(host=args.host, port=args.port, user=args.user,
                         password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                         charset='utf8mb4', autocommit=True) as connection:
        result = capture(connection)
    if args.record:
        args.record.write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')) + '\n',
                               encoding='utf-8')
        print(f'Recorded {len(result)} complete configuration cases on the original')
    else:
        expected = json.loads(Path(__file__).with_name('tokenize_config_expected.json').read_text(encoding='utf-8'))
        assert len(result) == len(expected), (len(result), len(expected))
        for index, (actual, original) in enumerate(zip(result, expected)):
            assert actual == original, (index, actual, original)
        print(f'PASS: {len(result)} complete configuration, grouping and error cases match original')


if __name__ == '__main__':
    main()
