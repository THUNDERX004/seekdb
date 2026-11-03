#!/usr/bin/env python3
"""Check complete IK prefix results against a separately verified original.

The optional fixture recording mode must only be used on the original binary.
No benchmark tables or score inputs are modified.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import random

import pymysql


def corpus():
    texts = [
        '', '中华人民共和国中华人民共和中华人民共和国',
        '研究生命起源研究生生命科学研究院',
        '南京市长江大桥长江大桥南京市长江',
        '北京大学生北京大学学生北京大学生命科学学院',
        '中华人民共和国中央人民政府中华人民共和国国务院',
        '一二三四五六七八九十百千万亿公斤千克公里厘米毫升',
        '数据库全文索引中文分词索引构建中英文混合检索',
        'ＡＢＣ１２３ abc123 中文-english 数据库123个三十公斤',
        '\x00中文\x01中文😀中文\n中文\t研究生命起源',
        '한글カタカナひらがな日本語한국어中文',
        '龘龖龗齉龍麤𠀀乁中华人民共和国\uffff',
        '中国中中中国国中华华人民民共和国国',
    ]
    # Include prefixes from disjoint initial-character ranges, frequent missing
    # characters, resets between characters, and long overlapping hit chains.
    rng = random.Random(8719)
    words = ['中华人民共和国', '人民', '北京大学', '南京市长江大桥',
             '研究生命起源', '生命科学', '中央人民政府', '国务院',
             '数据库', '全文索引', '中', '一千克', '龘', 'カタカナ',
             '한국어', '𠀀', 'ＡＢＣ', '英文ABC', '中文中文']
    for i in range(64):
        separator = ['', ' ', '，', '\x00'][i % 4]
        texts.append(separator.join(rng.choices(words, k=24)))
    texts.extend(texts[i] * 32 for i in [1, 2, 3, 4])
    return texts


def capture(options):
    output = []
    with pymysql.connect(**options) as connection:
        with connection.cursor() as cursor:
            for mode in ['smart', 'max_word']:
                for text in corpus():
                    config = json.dumps([{'output': 'all'},
                        {'additional_args': [{'ik_mode': mode}]}])
                    cursor.execute('SELECT TOKENIZE(%s,\'ik\',%s) AS tokens', (text, config))
                    output.append([mode, text, json.loads(cursor.fetchone()[0])])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2891)
    parser.add_argument('--user', default='root')
    parser.add_argument('--record', type=Path)
    args = parser.parse_args()
    options = dict(host=args.host, port=args.port, user=args.user,
                   password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                   charset='utf8mb4', autocommit=True)
    actual = capture(options)
    if args.record:
        args.record.write_text(json.dumps(actual, ensure_ascii=False,
            separators=(',', ':')) + '\n', encoding='utf-8')
        print(f'Recorded {len(actual)} complete prefix cases on the original')
    else:
        expected = json.loads(Path(__file__).with_name(
            'dictionary_prefixes_expected.json').read_text(encoding='utf-8'))
        assert actual == expected, 'IK prefix outputs differ from original'
        with ThreadPoolExecutor(max_workers=4) as pool:
            for concurrent in pool.map(capture, [options] * 4):
                assert concurrent == expected, 'Concurrent prefix outputs differ'
        print(f'PASS: {len(actual) * 5} complete IK prefix outputs, including four concurrent sessions')


if __name__ == '__main__':
    main()
