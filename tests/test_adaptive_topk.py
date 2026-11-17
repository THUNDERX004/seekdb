"""8192-row exact TopK gate checks against an already-built JSONL executable.

Expected IDs use one global Python integer-distance sort, never another DB.
SQL checks results; worker scheduling itself belongs to the worker unit tests.
"""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from jsonl_client import JsonlClient


def literal(vector):
    return "'[" + ','.join(map(str, vector)) + "]'"


class AdaptiveTopKTests(unittest.TestCase):
    executable = None

    def sql(self, client, statement):
        result = client.execute(statement)
        self.assertIs(result.get('ok'), True, result)
        return result.get('rows', [])

    def insert(self, client, table, items):
        # Integer components are exactly representable as float32. Cache SQL
        # strings and oracle scores by prototype to keep this suite bounded.
        literals = {vector: literal(vector) for _, vector in items}
        for start in range(0, len(items), 512):
            values = ','.join(f'({pk},{7 if pk < 4096 else 8},{literals[v]})'
                              for pk, v in items[start:start + 512])
            self.sql(client, f'INSERT INTO {table} VALUES ' + values)

    def check(self, client, table, rows, probe, filtered=False):
        candidates = [(pk, vector) for pk, (tag, vector) in rows.items()
                      if not filtered or tag == 7]
        scores = {vector: sum((a - b) ** 2 for a, b in zip(vector, probe))
                  for _, vector in candidates}
        ordered = sorted(candidates, key=lambda row: (scores[row[1]], row[0]))
        where = 'WHERE tag=7 ' if filtered else ''
        expression = f'L2_DISTANCE(v,{literal(probe)})'
        # Exercise the full query repeatedly so warm reuse follows cold or
        # parked fallbacks. Alternate implicit PK ties and explicit ORDER BY.
        for offset in (0, 3):
            expected = [[pk] for pk, _ in ordered[offset:offset + 10]]
            for order in (expression, expression + ',id'):
                actual = self.sql(client, f'SELECT id FROM {table} {where}'
                                  f'ORDER BY {order} LIMIT 10 OFFSET {offset}')
                self.assertEqual(actual, expected,
                                 (table, len(candidates), filtered, offset, probe))
                self.assertTrue(all(type(row[0]) is int for row in actual))

    def test_gate_ties_ranges_bucket_overlay_commit_restart(self):
        with tempfile.TemporaryDirectory(prefix='mvd-adaptive-topk-') as directory:
            def open_client():
                with patch.dict(os.environ, {'MVD_CACHE_LIMIT_MB': '64'}):
                    result = JsonlClient([self.executable], Path(directory), timeout=30)
                self.addCleanup(result.close)
                return result

            client = open_client()
            cases = []
            for dimension in (64, 128):
                table = f't{dimension}'
                zero = (0,) * dimension
                prototypes = [tuple(1 if (axis + rank) % 7 < 3 else -1
                                    for axis in range(dimension)) for rank in range(7)]

                def spike(axis):
                    vector = [0] * dimension
                    vector[axis] = 8
                    if dimension == 128:
                        vector[(axis + 64) % dimension] = 8
                    return tuple(vector)

                # 8191 is the serial control. Both eligible 8192/8193 scans
                # split at 4096, and 8193 has an odd final SIMD tail. Equal
                # norms tie across both halves; distinct probes target the
                # split's neighbours and final row, independently of merging.
                special = {4095: spike(7), 4096: spike(15), 4097: spike(31),
                           8191: spike(47), 8192: spike(dimension - 1)}
                source = [(index - 4096, special.get(index, prototypes[index % 7]))
                          for index in range(8193)]
                self.assertTrue(all(sum(v * v for v in vector) == dimension
                                    for _, vector in source))
                rows = {pk: (7 if pk < 4096 else 8, vector)
                        for pk, vector in source[:8191]}
                self.sql(client, f'CREATE TABLE {table}'
                         f'(id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR({dimension}))')
                self.insert(client, table, list(reversed(source[:8191])))
                for count in (8191, 8192, 8193):
                    if count > 8191:
                        pk, vector = source[count - 1]
                        self.insert(client, table, [(pk, vector)])
                        rows[pk] = (7 if pk < 4096 else 8, vector)
                    self.assertEqual(len(rows), count)
                    for probe in (zero, special[4095], special[4096],
                                  special[4097], special[8192]):
                        self.check(client, table, rows, probe)
                self.assertEqual(sum(tag == 7 for tag, _ in rows.values()), 8192)
                for probe in (zero, special[4096], special[8192]):
                    self.check(client, table, rows, probe, filtered=True)

                # Warmed descriptors must not hide private updates, deletes
                # or inserts. The bucket stays at exactly 8192 private rows.
                moving = source[4096][0]
                removed = source[4095][0]
                inserted = -10000
                for commit in (False, True):
                    self.sql(client, 'BEGIN')
                    pending = rows.copy()
                    self.sql(client, f'UPDATE {table} SET v={literal(zero)} WHERE id={moving}')
                    pending[moving] = (7, zero)
                    self.sql(client, f'DELETE FROM {table} WHERE id={removed}')
                    del pending[removed]
                    self.sql(client, f'INSERT INTO {table} VALUES ({inserted},7,{literal(zero)})')
                    pending[inserted] = (7, zero)
                    for filtered in (False, True):
                        self.check(client, table, pending, zero, filtered)
                        self.check(client, table, pending, special[8192], filtered)
                    self.sql(client, 'COMMIT' if commit else 'ROLLBACK')
                    if commit:
                        rows = pending
                    for filtered in (False, True):
                        self.check(client, table, rows, zero, filtered)
                # A separate autocommit update invalidates the just-reused
                # full matrix and equality view, including the odd tail row.
                tail = source[-1][0]
                self.sql(client, f'UPDATE {table} SET v={literal(zero)} WHERE id={tail}')
                rows[tail] = (8, zero)
                for filtered in (False, True):
                    self.check(client, table, rows, zero, filtered)
                cases.append((table, rows, zero, special[8192]))
            client.shutdown()
            client.close()
            client = open_client()
            for table, rows, zero, tail_probe in cases:
                # Filtered-first cold preparation after recovery, then full
                # scan and a second probe catch stale persisted/worker state.
                for filtered in (True, False):
                    for probe in (zero, tail_probe):
                        self.check(client, table, rows, probe, filtered)
            client.shutdown()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    args, remaining = parser.parse_known_args()
    AdaptiveTopKTests.executable = str(args.executable.resolve())
    unittest.main(argv=[sys.argv[0], *remaining])
