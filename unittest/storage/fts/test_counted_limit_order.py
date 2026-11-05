"""Differential errors/results for counted FTS limits on an owned instance."""
import argparse
from decimal import Decimal
import hashlib
import gzip
import json
import os
from pathlib import Path

import pymysql

DATABASE='fts_counted_order_boundaries'

def require(condition,message):
    if not condition:
        raise RuntimeError(message)

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=2891)
    p.add_argument('--output',type=Path);p.add_argument('--reference',type=Path,
        default=Path(__file__).with_name('counted_limit_order_expected.json.gz'))
    p.add_argument('--check-plans',action='store_true')
    a=p.parse_args();require(a.port!=2881 and (a.output is None or not a.output.exists()), 'Dedicated port and fresh output required')
    records=[];plans=[]
    with pymysql.connect(host='127.0.0.1',port=a.port,user='root',charset='utf8mb4',autocommit=True,
                         password=os.environ.get('SEEKDB_TEST_PASSWORD','')) as c:
        with c.cursor() as cur:
            cur.execute('SET ob_query_timeout=1000000000')
            cur.execute('DROP DATABASE IF EXISTS '+DATABASE)
            cur.execute('CREATE DATABASE '+DATABASE+' CHARACTER SET utf8mb4')
            try:
                cur.execute('USE '+DATABASE)
                cur.execute('CREATE TABLE docs(id BIGINT PRIMARY KEY, v INT, body TEXT)')
                cur.executemany('INSERT INTO docs VALUES(%s,%s,%s)',[(i+1,None if i%5==0 else i%9,'common '+('needle ' if i%3==0 else '')+'token '*(i%5+1)) for i in range(513)])
                cur.execute('ALTER TABLE docs ADD FULLTEXT INDEX fti(body) WITH PARSER beng')
                base="SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST(?) LIMIT ? OFFSET ?) counted"
                def capture(label,sql=None):
                    try:
                        cur.execute(sql or label)
                        value=dict(rows=cur.fetchall())
                    except pymysql.MySQLError as error:
                        value=dict(error=int(error.args[0]))
                    records.append(dict(case=label,**value))
                cur.execute('PREPARE counted_order FROM %s',(base,))
                try:
                    bindings=[('common',20,0),('needle',1,1),('',20,0),('absentuniqueword',20,0),
                              (None,20,0),('common',None,0),('common',20,None),
                              ('common',-1,0),('common',20,-1),('common',0,0),
                              ('common',2**63-1,0),('common',20,2**63-1),
                              ('common',2**64-1,0),('common',20,2**64-1),
                              ('common','20','1'),('common','bad','bad'),('common',1.5,0),
                              ('needle',20,512),('common',20,512),('common',20,1000)]
                    for repetition in range(2):
                        for term,limit,offset in bindings:
                            cur.execute('SET @term=%s,@lim=%s,@off=%s',(term,limit,offset))
                            capture('prepared:'+json.dumps([repetition,term,limit,offset]),'EXECUTE counted_order USING @term,@lim,@off')
                finally:
                    cur.execute('DEALLOCATE PREPARE counted_order')
                # Same prepared handle: force immediate valid -> invalid -> valid reuse.
                cur.execute('PREPARE counted_reuse FROM %s',(base,))
                try:
                    reuse=[('common',20,0),('common',2**63-1,0),('common',20,0),
                           ('common',20,2**63-1),('common',20,0),
                           ('common',2**32,0),('common',20,0),
                           ('common',2**32-1,1),('common',20,0),
                           ('common',2**63-1,20),('common',20,0),
                           ('common',None,0),('common',20,0),
                           ('common',-1,0),('common',20,0),
                           ('common',20,None),('common',20,0),
                           ('common',20,-1),('common',20,0),
                           ('common','20','1'),('needle',21,1),('common',20,0)]
                    for index,(term,limit,offset) in enumerate(reuse):
                        cur.execute('SET @term=%s,@lim=%s,@off=%s',(term,limit,offset))
                        capture('reuse:'+str(index),'EXECUTE counted_reuse USING @term,@lim,@off')
                finally:
                    cur.execute('DEALLOCATE PREPARE counted_reuse')
                # Auto-parameterized literal statements exercise both cache warm orders.
                small=(20,0)
                for direction,bounds in enumerate(((2**32,0),(2**32-1,1),
                                                   (2**63-1,0),(20,2**63-1))):
                    for phase,sequence in enumerate(((small,bounds,small),(bounds,small,bounds,small))):
                        for index,(limit,offset) in enumerate(sequence):
                            sql=("SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) "
                                 "AGAINST('common') LIMIT "+str(limit)+" OFFSET "+str(offset)+") counted_literal_"+str(direction)+"_"+str(phase))
                            capture('literal-reuse:'+str((direction,phase,index)),sql)
                variants=[
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 20 OFFSET 1) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') ORDER BY id DESC LIMIT 20 OFFSET 1) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') ORDER BY MATCH(body) AGAINST('needle') DESC,id LIMIT 20 OFFSET 1) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') AND v IS NOT NULL LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT id+1 FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST(CONCAT('nee','dle')) LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('+needle -absentuniqueword' IN BOOLEAN MODE) LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT DISTINCT v FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 20) counted",
                    "SELECT COUNT(v) FROM (SELECT v FROM docs WHERE MATCH(body) AGAINST('needle') ORDER BY id LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT /*+ NO_REWRITE */ id FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 0) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('needle') LIMIT 20 OFFSET 1000) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST('') LIMIT 20) counted",
                    "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST(NULL) LIMIT 20) counted",
                ]
                for number,sql in enumerate(variants):
                    capture('variant:'+str(number),sql)
                    cur.execute('EXPLAIN EXTENDED '+sql)
                    plan='\n'.join(str(row[0]) for row in cur.fetchall())
                    plans.append(dict(case=number,sql=sql,plan=plan))
                    if a.check_plans and number==0:
                        require('calc_relevance=true' in plan and 'sort_keys(' not in plan,plan)
                        require('limit(' in plan.lower(),plan)
                    if a.check_plans and number in (2,3,4,5,9):
                        require('sort_keys(' in plan or '|SORT' in plan,plan)
            finally:
                cur.execute('DROP DATABASE IF EXISTS '+DATABASE)
    normalized=json.loads(json.dumps(records,default=lambda v:str(v) if isinstance(v,Decimal) else v))
    differences=[]
    if a.reference:
        reference=json.loads(gzip.decompress(a.reference.read_bytes()) if a.reference.suffix=='.gz' else a.reference.read_bytes())
        require(len(normalized)==len(reference['records'])==104,'Expected exactly 104 actual and reference records')
        differences=[dict(index=i,reference=old,candidate=new) for i,(old,new) in enumerate(zip(reference['records'],normalized)) if old!=new]
    result=dict(status='FAIL_DIFFERENTIAL' if differences else 'PASS',records=normalized,plans=plans,differences=differences,reference=str(a.reference) if a.reference else None,
                interpretation='Differential full rows/error codes, not timing or independent semantic proof for invalid SQL.')
    if a.output:
        with a.output.open('x',encoding='utf-8') as stream:
            json.dump(result,stream,indent=2);stream.write('\n')
    print(result['status']+' boundary results/errors cases='+str(len(records)))
    if a.output:
        print('OUTPUT sha256='+hashlib.sha256(a.output.read_bytes()).hexdigest())
    require(not differences,json.dumps(differences))

if __name__=='__main__':main()
