#!/usr/bin/env python3
"""Compare complete TOKENIZE results for every BMP scalar to official original.

Each digest covers the complete, ordered JSON result; counts alone never pass.
Record fixtures only on the independently built official competition original.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import pymysql

def documents():
    scalars = [chr(c) for c in range(0x10000) if not 0xd800 <= c <= 0xdfff]
    result = []
    for start in range(0, len(scalars), 128):
        chars = scalars[start:start+128]
        result.append(' '.join(chars))
        result.append(' '.join('中'+ch+'国 '+ch+ch for ch in chars))
    rng = random.Random(28143)
    rng.shuffle(scalars)
    for start in range(0, len(scalars), 256):
        result.append(''.join(scalars[start:start+256]))
    result.extend(['a\0中\0文 ', '中文𠀀中文😀中文乁',
                   'éé e\u0301e\u0301 ＡＢＣ１２３', '中华人民共和国南京市长江大桥'] )
    return result

def capture(options):
    results=[]
    with pymysql.connect(**options) as conn:
        with conn.cursor() as cur:
            cur.execute('SET ob_query_timeout=1000000000')
            for mode in ['smart','max_word']:
                config=json.dumps([{'output':'all'}, {'additional_args':[{'ik_mode':mode}]}])
                for i,text in enumerate(documents()):
                    cur.execute('SELECT TOKENIZE(%s,\'ik\',%s) AS tokens',(text,config))
                    value=json.loads(cur.fetchone()[0])
                    data=json.dumps(value,ensure_ascii=True,separators=(',',':')).encode()
                    results.append([mode,i,hashlib.sha256(data).hexdigest()])
    return results

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',default='127.0.0.1')
    p.add_argument('--port',type=int,default=2891)
    p.add_argument('--record',type=Path)
    a=p.parse_args()
    result=capture(dict(host=a.host,port=a.port,user='root',
        password=os.environ.get('SEEKDB_TEST_PASSWORD',''),charset='utf8mb4',autocommit=True))
    if a.record:
        a.record.write_text(json.dumps(result,separators=(',',':'))+'\n',encoding='utf-8')
        print('Recorded',len(result),'complete BMP output digests on original')
    else:
        expected=json.loads(Path(__file__).with_name('dictionary_bmp_expected.json').read_text(encoding='utf-8'))
        assert result==expected,'Complete BMP token result differs from official original'
        print('PASS:',len(result),'complete token outputs covering every BMP scalar in both IK modes')

if __name__=='__main__':
    main()
