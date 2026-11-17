"""Exact ranking around the saved-score scan's gates and invalidation."""
import argparse
from dataclasses import replace
from pathlib import Path
import os,sys,tempfile,unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from jsonl_client import JsonlClient
from test_cached_topk import Row,quoted
from test_exact_distance import exact_squared,float32,from_bits,vector_sql

class SeedTopKTests(unittest.TestCase):
    executable=None

    def sql(self,client,text):
        result=client.execute(text)
        self.assertIs(result.get('ok'),True,result)
        return result.get('rows',[])

    def verify(self,client,table,rows,probe,groups=None,windows=None,implicit_ties=False):
        scores={v:exact_squared(v,probe) for v in {row.vector for row in rows.values()}}
        literal=quoted(vector_sql(probe))
        if groups is None:
            groups=[('',lambda r:True),('WHERE tag=7 ',lambda r:r.tag==7),
                    ('WHERE tag=8 ',lambda r:r.tag==8),
                    ("WHERE tag=7 AND category='red' ",lambda r:r.tag==7 and r.category=='red')]
            groups.extend((f'WHERE category={quoted(category)} ',
                           lambda r,category=category:r.category==category)
                          for category in sorted({r.category for r in rows.values()}))
        if windows is None:windows=[(1,0),(10,3),(27,5),(33,0)]
        for where,predicate in groups:
            expected=sorted((r for r in rows.values() if predicate(r)),key=lambda r:(scores[r.vector],r.primary))
            for limit,offset in windows:
                suffix=f'FROM {table} {where}ORDER BY d,id LIMIT {limit} OFFSET {offset}'
                actual=self.sql(client,f'SELECT id,L2_DISTANCE(v,{literal}) AS d {suffix}')
                self.assertEqual([r[0] for r in actual],[r.primary for r in expected[offset:offset+limit]],(table,where,limit,offset))
                self.assertEqual(len({r[0] for r in actual}),len(actual))
                order_only=self.sql(client,f'SELECT id FROM {table} {where}ORDER BY L2_DISTANCE(v,{literal}),id LIMIT {limit} OFFSET {offset}')
                self.assertEqual(order_only,[[r[0]] for r in actual])
            if implicit_ties:
                # Omitting the explicit secondary key must retain exact-score
                # ties in primary-key order, including ties across seed lanes.
                actual=self.sql(client,f'SELECT id FROM {table} {where}ORDER BY L2_DISTANCE(v,{literal}) LIMIT 27 OFFSET 5')
                self.assertEqual(actual,[[r.primary] for r in expected[5:32]],(table,where,'implicit primary tie'))

    def test_exact_ties_half_steps_tails_filters_and_transactions(self):
        with tempfile.TemporaryDirectory(prefix='mvd-seed-topk-') as directory:
            with patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'64'}):
                client=JsonlClient([self.executable],Path(directory),timeout=60)
            try:
                cases=[]
                for dimension in (64,128,256):
                    for pattern in ('normal','subnormal','swamped'):
                        table=f't{dimension}_{pattern}'
                        self.sql(client,f'CREATE TABLE {table}(id BIGINT PRIMARY KEY,tag BIGINT,category VARCHAR(8),v VECTOR({dimension}))')
                        prototypes=[]
                        for rank in range(17):
                            if pattern=='normal':
                                vector=tuple(float32(1000+((axis*13+rank*7)%97-48)/64) for axis in range(dimension))
                            elif pattern=='subnormal':
                                vector=tuple(from_bits(1+(rank*7+axis*3)%97) for axis in range(dimension))
                            else:
                                vector=(float(2**100),float(rank-8))+(0.,)*(dimension-2)
                            prototypes.append(vector)
                        count=4355
                        rows={}
                        for index in range(count):
                            primary=(index*73)%count-count//2
                            rows[primary]=Row(primary,7 if index<2112 else 8,
                                'red' if index%3 else 'blue','',prototypes[index%17])
                        items=list(rows.values())
                        for start in range(0,count,128):
                            values=','.join(f'({r.primary},{r.tag},{quoted(r.category)},{quoted(vector_sql(r.vector))})' for r in items[start:start+128])
                            self.sql(client,f'INSERT INTO {table} VALUES '+values)
                        probe=prototypes[7]
                        self.verify(client,table,rows,probe)
                        cases.append((table,rows,probe))
                        if pattern=='normal':
                            before=rows.copy();moving=items[0]
                            changed=replace(moving,tag=8,vector=probe)
                            self.sql(client,'BEGIN')
                            self.sql(client,f'UPDATE {table} SET tag=8,v={quoted(vector_sql(probe))} WHERE id={moving.primary}')
                            rows[moving.primary]=changed
                            self.verify(client,table,rows,probe)
                            self.sql(client,'ROLLBACK');rows.clear();rows.update(before)
                            self.verify(client,table,rows,probe)
                            self.sql(client,'BEGIN')
                            self.sql(client,f'UPDATE {table} SET tag=8,v={quoted(vector_sql(probe))} WHERE id={moving.primary}')
                            self.sql(client,f'DELETE FROM {table} WHERE id={items[-1].primary}')
                            self.sql(client,'COMMIT')
                            rows[moving.primary]=changed;del rows[items[-1].primary]
                            self.verify(client,table,rows,probe)
                client.shutdown();client.close()
                with patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'64'}):
                    client=JsonlClient([self.executable],Path(directory),timeout=60)
                for table,rows,probe in cases:self.verify(client,table,rows,probe)
                client.shutdown()
            finally:client.close()

    def test_all_equality_buckets_cross_seed_gates_and_recover(self):
        sizes=(1,15,16,511,512,513,1023,2047,2048,2049,2500)
        categories=tuple(f'g{group:02d}' for group in range(len(sizes)))
        memberships=[group for group,size in enumerate(sizes) for _ in range(size)]
        count=len(memberships)
        gates=(511,512,513,2047,2048,2049)

        def bucket_groups():
            # Warm the parent first, then regroup the same shared codes by
            # integer and text equality. Replay earlier buckets after each
            # other column has built its differently ordered bucket slices.
            groups=[('',lambda r:True)]
            for group in range(len(sizes)):
                tag=7+group
                groups.append((f'WHERE tag={tag} ',lambda r,tag=tag:r.tag==tag))
            groups.append(('',lambda r:True))
            for category in reversed(categories):
                groups.append((f'WHERE category={quoted(category)} ',
                               lambda r,category=category:r.category==category))
            for size in gates:
                group=sizes.index(size);tag=7+group;category=categories[group]
                groups.append((f'WHERE tag={tag} ',lambda r,tag=tag:r.tag==tag))
                groups.append((f'WHERE category={quoted(category)} ',
                               lambda r,category=category:r.category==category))
            groups.extend([
                ("WHERE tag=11 AND category='g04' ",lambda r:r.tag==11 and r.category=='g04'),
                ('WHERE tag=99999 ',lambda r:False),
                ("WHERE category='missing' ",lambda r:False),
            ])
            return groups

        def verify_buckets(client,table,rows,probe,thorough=False):
            windows=[(1,0),(10,3),(27,5),(32,0),(1,31),(1,32),(33,0)] if thorough else [(10,3),(27,5)]
            self.verify(client,table,rows,probe,groups=bucket_groups(),
                        windows=windows,implicit_ties=thorough)

        def assert_sizes(rows,wanted):
            for group,size in enumerate(wanted):
                self.assertEqual(sum(r.tag==7+group for r in rows.values()),size,('tag',group))
                self.assertEqual(sum(r.category==categories[group] for r in rows.values()),size,('category',group))

        with tempfile.TemporaryDirectory(prefix='mvd-seed-equality-') as directory:
            with patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'64'}):
                client=JsonlClient([self.executable],Path(directory),timeout=60)
            cases=[]
            try:
                for dimension in (64,128,256):
                    table=f'equality_{dimension}'
                    self.sql(client,f'CREATE TABLE {table}(id BIGINT PRIMARY KEY,tag BIGINT,category VARCHAR(8),v VECTOR({dimension}))')
                    prototypes=[tuple(float32(((axis*13+rank*7)%97-48)*scale)
                                      for axis in range(dimension))
                                for scale in (1/64,1.,2.) for rank in range(7)]
                    fixture=[]
                    for index,group in enumerate(memberships):
                        # Both permutations are coprime to count. Equality
                        # columns have identical bucket lengths but distinct
                        # memberships, independent of primary/insertion order.
                        primary=(index*73)%count-count//2
                        category=categories[memberships[(index*37+19)%count]]
                        vector=prototypes[(group%3)*7+(index*5+group*3)%7]
                        fixture.append(Row(primary,7+group,category,'',vector))
                    rows={r.primary:r for r in fixture}
                    self.assertEqual(len(rows),count)
                    assert_sizes(rows,sizes)
                    for group,size in enumerate(sizes):
                        if size>=511:
                            bucket=[r for r in fixture if r.tag==7+group]
                            self.assertGreater(max(sum(r.vector==v for r in bucket)
                                                   for v in {r.vector for r in bucket}),32)
                    for start in range(0,count,128):
                        values=','.join(f'({r.primary},{r.tag},{quoted(r.category)},{quoted(vector_sql(r.vector))})' for r in fixture[start:start+128])
                        self.sql(client,f'INSERT INTO {table} VALUES '+values)
                    probe=prototypes[3]
                    verify_buckets(client,table,rows,probe,thorough=True)
                    verify_buckets(client,table,rows,(0.,)*dimension)

                    # An ordinary update moves both equality columns across
                    # 512 -> 511 and 511 -> 512 while replacing a ranked vector.
                    source=sizes.index(512);target=sizes.index(511)
                    moving=next(r for r in rows.values()
                                if r.tag==7+source and r.category==categories[source])
                    changed=replace(moving,tag=7+target,category=categories[target],vector=probe)
                    self.sql(client,f'UPDATE {table} SET tag={changed.tag},category={quoted(changed.category)},v={quoted(vector_sql(changed.vector))} WHERE id={changed.primary}')
                    rows[changed.primary]=changed
                    updated_sizes=list(sizes);updated_sizes[source]-=1;updated_sizes[target]+=1
                    assert_sizes(rows,updated_sizes)
                    verify_buckets(client,table,rows,probe)

                    # The transaction also crosses 2048/2047 and 513/512,
                    # changes the matrix scale, and adds the smallest PK to a
                    # distance tie. Repeat the same edits for rollback/commit.
                    bridge_group=sizes.index(2048);bridge_target=sizes.index(2047)
                    bridge=next(r for r in rows.values()
                                if r.tag==7+bridge_group and r.category==categories[bridge_group])
                    deletion_group=sizes.index(513)
                    removed=next(r for r in rows.values()
                                 if r.tag==7+deletion_group and r.category==categories[deletion_group])
                    bridged=replace(bridge,tag=7+bridge_target,category=categories[bridge_target],
                                    vector=(4096.,)+(0.,)*(dimension-1))
                    inserted=Row(-count-17,7+source,categories[source],'',probe)
                    committed_sizes=updated_sizes.copy()
                    committed_sizes[bridge_group]-=1;committed_sizes[bridge_target]+=1
                    committed_sizes[deletion_group]-=1;committed_sizes[source]+=1
                    for commit in (False,True):
                        self.sql(client,'BEGIN')
                        pending=rows.copy()
                        self.sql(client,f'UPDATE {table} SET tag={bridged.tag},category={quoted(bridged.category)},v={quoted(vector_sql(bridged.vector))} WHERE id={bridged.primary}')
                        pending[bridged.primary]=bridged
                        self.sql(client,f'DELETE FROM {table} WHERE id={removed.primary}')
                        del pending[removed.primary]
                        self.sql(client,f'INSERT INTO {table} VALUES ({inserted.primary},{inserted.tag},{quoted(inserted.category)},{quoted(vector_sql(inserted.vector))})')
                        pending[inserted.primary]=inserted
                        assert_sizes(pending,committed_sizes)
                        verify_buckets(client,table,pending,probe)
                        self.sql(client,'COMMIT' if commit else 'ROLLBACK')
                        if commit:rows=pending
                        assert_sizes(rows,committed_sizes if commit else updated_sizes)
                        verify_buckets(client,table,rows,probe)
                    cases.append((table,rows,probe))
                client.shutdown();client.close()
                with patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'64'}):
                    client=JsonlClient([self.executable],Path(directory),timeout=60)
                for table,rows,probe in cases:
                    verify_buckets(client,table,rows,probe)
                client.shutdown()
            finally:client.close()

    def test_both_parallel_ranges_have_seed_scans(self):
        # Retain the aligned split at 32784; 65566 instead splits at 32783,
        # making the second range start at leading_rows=15 in shared codes.
        for count in (65568,65566):
            with self.subTest(count=count):
                dimension=64
                prototypes=[tuple(float((axis*13+rank*7)%17-8) for axis in range(dimension)) for rank in range(17)]
                rows={}
                for index in range(count):
                    primary=(index*37)%count-count//2
                    rows[primary]=Row(primary,7,'red' if index%3 else 'blue','',prototypes[index%17])
                self.assertEqual(len(rows),count)
                with tempfile.TemporaryDirectory(prefix='mvd-both-seed-ranges-') as directory:
                    with patch.dict(os.environ,{'MVD_CACHE_LIMIT_MB':'192'}):
                        client=JsonlClient([self.executable],Path(directory),timeout=120)
                    try:
                        self.sql(client,'CREATE TABLE large(id BIGINT PRIMARY KEY,tag BIGINT,category VARCHAR(8),v VECTOR(64))')
                        items=list(rows.values())
                        for start in range(0,count,512):
                            values=','.join(f'({r.primary},{r.tag},{quoted(r.category)},{quoted(vector_sql(r.vector))})' for r in items[start:start+512])
                            self.sql(client,'INSERT INTO large VALUES '+values)
                        self.verify(client,'large',rows,prototypes[0])
                        self.verify(client,'large',rows,(0.,)*dimension)
                        client.shutdown()
                    finally:client.close()

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('executable');a,remaining=p.parse_known_args()
    SeedTopKTests.executable=str(Path(a.executable).resolve())
    unittest.main(argv=[sys.argv[0],*remaining])
