"""JSON transaction/restart regression above the former 16 MiB code limit.

All but the first vector axis are identical. Exact squared distances therefore
reduce to one independently known integer difference; no database code or
quantized bounds are used to derive the expected ranking.
"""
from pathlib import Path
import math
import os
import sys
import tempfile
from unittest.mock import patch

ROOT=next(parent for parent in Path(__file__).resolve().parents
          if (parent/'tests/jsonl_client.py').is_file() and (parent/'src/minivectordb.cpp').is_file())
sys.path.insert(0,str(ROOT/'tests'))
from jsonl_client import JsonlClient


def main():
    executable=Path(sys.argv[1]).resolve(strict=True)
    dimension=8193
    suffix=','.join('0.5' if axis%2 else '-0.5' for axis in range(1,dimension))+']'
    literal=lambda coordinate:'['+str(coordinate/4096)+','+suffix
    rows={primary:primary for primary in range(1,2050)}
    def execute(client,sql,affected=None):
        result=client.execute(sql)
        assert result['ok'],result
        if affected is not None:assert result['affected_rows']==affected,result
        return result
    def check(client,state):
        for probe in (1024,1025):
            for category in (None,1):
                candidates=[(abs(coordinate-probe),primary) for primary,coordinate in state.items()
                            if category is None or primary%4==category]
                truth=sorted(candidates)[1:11]
                condition='' if category is None else f'WHERE cat={category} '
                result=execute(client,f"SELECT id,cat,L2_DISTANCE(v,'{literal(probe)}') AS d "
                               f'FROM large {condition}ORDER BY d,id LIMIT 10 OFFSET 1')
                assert result['columns']==['id','cat','d']
                assert len(result['rows'])==len(truth)
                for got,(distance,primary) in zip(result['rows'],truth):
                    assert got[:2]==[primary,primary%4],(got,primary)
                    assert math.isclose(got[2],distance/4096,rel_tol=0,abs_tol=0.000000501),(got,distance)
    with tempfile.TemporaryDirectory(prefix='mvd-large-quantized-') as directory, \
         patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'192'}):
        client=JsonlClient([executable],Path(directory),timeout=60)
        try:
            execute(client,f'CREATE TABLE large(id BIGINT PRIMARY KEY,cat BIGINT,v VECTOR({dimension}))')
            for first in range(1,2050,32):
                batch=range(first,min(first+32,2050))
                sql='INSERT INTO large VALUES '+','.join(f"({p},{p%4},'{literal(p)}')" for p in batch)
                execute(client,sql,affected=len(batch))
            check(client,rows)
            execute(client,'BEGIN')
            execute(client,f"UPDATE large SET v='{literal(513)}' WHERE id=1025",affected=1)
            execute(client,'DELETE FROM large WHERE id=1024',affected=1)
            execute(client,f"INSERT INTO large VALUES(3000,0,'{literal(1024)}')",affected=1)
            overlay=dict(rows)
            overlay[1025]=513
            del overlay[1024]
            overlay[3000]=1024
            check(client,overlay)
            execute(client,'ROLLBACK')
            check(client,rows)
            execute(client,'BEGIN')
            execute(client,f"UPDATE large SET v='{literal(16384)}' WHERE id=1",affected=1)
            execute(client,'DELETE FROM large WHERE id=1024',affected=1)
            execute(client,f"INSERT INTO large VALUES(3000,0,'{literal(1024)}')",affected=1)
            execute(client,'COMMIT')
            rows[1]=16384
            del rows[1024]
            rows[3000]=1024
            check(client,rows)
            client.shutdown()
        finally:client.close()
        client=JsonlClient([executable],Path(directory),timeout=60)
        try:
            check(client,rows)
            client.shutdown()
        finally:client.close()
    print('PASS large exact codes: full/equality, odd padding, overlay, rollback, commit, restart')


if __name__=='__main__':main()
