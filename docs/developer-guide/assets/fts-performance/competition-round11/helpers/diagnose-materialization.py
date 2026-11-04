"""Execute a small plan-path diagnostic only; excluded from performance reports."""
import argparse
import json
from pathlib import Path

import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=2891, choices=[2891])
args = parser.parse_args()
queries = [
    ("count_cn", "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')", 8001),
    ("count_beng", "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title_en,content_en) AGAINST('database performance audit')", 11000),
    ("count_mixed", "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('数据库 database')", None),
    ("ids_ranked", "SELECT id FROM fts_large_bench.docs WHERE MATCH(title,content) AGAINST('全文索引 分词器') ORDER BY id LIMIT 3", None),
    ("limit_count", "SELECT COUNT(*) FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20)t", 20),
]
out = []
with pymysql.connect(host='127.0.0.1', port=args.port, user='root', charset='utf8mb4', autocommit=True) as connection:
    with connection.cursor() as cursor:
        for name, sql, expected in queries:
            cursor.execute(sql)
            rows = cursor.fetchall()
            if expected is not None:
                assert rows == ((expected,),), (name, rows)
            out.append({'name': name, 'sql': sql, 'rows': rows})
print(json.dumps(out, ensure_ascii=False, indent=2))
