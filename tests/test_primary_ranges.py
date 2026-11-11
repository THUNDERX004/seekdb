#!/usr/bin/env python3
"""Primary-key ranges must seek before payload I/O, including transaction changes."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

from jsonl_client import JsonlClient


def read_bytes(process):
    if os.name == 'nt':
        class Counters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in
                        ('read_ops', 'write_ops', 'other_ops',
                         'read_bytes', 'write_bytes', 'other_bytes')]
        counters = Counters()
        get = ctypes.WinDLL('kernel32', use_last_error=True).GetProcessIoCounters
        get.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters)]
        get.restype = ctypes.c_int
        if not get(int(process._handle), ctypes.byref(counters)):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.read_bytes
    values = dict(line.split(':', 1) for line in
                  Path(f'/proc/{process.pid}/io').read_text().splitlines())
    return int(values['rchar'])


class PrimaryRanges(unittest.TestCase):
    executable = None

    def open(self, cache):
        old = os.environ.get('MVD_CACHE_LIMIT_MB')
        os.environ['MVD_CACHE_LIMIT_MB'] = str(cache)
        try:
            client = JsonlClient([self.executable], Path(self.temp.name), timeout=10)
        finally:
            if old is None:
                os.environ.pop('MVD_CACHE_LIMIT_MB', None)
            else:
                os.environ['MVD_CACHE_LIMIT_MB'] = old
        self.addCleanup(client.close)
        return client

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mvd-primary-range-')
        self.addCleanup(self.temp.cleanup)

    def sql(self, client, sql):
        result = client.execute(sql)
        self.assertTrue(result['ok'], result)
        return result

    def populate(self, client, count=3000):
        self.sql(client, 'CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(128));')
        vector = '[1' + ',0' * 127 + ']'
        # Reverse each batch so physical record order is no ordering oracle.
        for first in range(1, count + 1, 100):
            rows = ','.join(f"({key},{key % 7},'{vector}')" for key in
                            reversed(range(first, min(first + 100, count + 1))))
            self.sql(client, 'INSERT INTO t VALUES' + rows + ';')

    def ids(self, client, sql):
        return self.sql(client, sql)['rows']

    def test_disk_ranges_and_limit_skip_unrelated_payload_io(self):
        client = self.open(1)
        self.populate(client)
        for sql, expected in [
            ('SELECT id FROM t WHERE id>3000 ORDER BY id LIMIT 1;', []),
            ('SELECT id FROM t WHERE id>=1500 AND id<1503 ORDER BY id;',
             [[1500], [1501], [1502]]),
            ('SELECT id FROM t ORDER BY id LIMIT 3;', [[1], [2], [3]]),
            ('SELECT id FROM t ORDER BY id DESC LIMIT 3;', [[3000], [2999], [2998]]),
        ]:
            with self.subTest(sql=sql):
                before = read_bytes(client.process)
                self.assertEqual(self.ids(client, sql), expected)
                consumed = read_bytes(client.process) - before
                self.assertLess(consumed, 128 * 1024,
                                f'range read {consumed} bytes of unrelated payload')
        client.shutdown()

    def test_overlay_merge_both_directions_and_restart(self):
        client = self.open(1)
        self.populate(client)
        self.sql(client, 'BEGIN;')
        self.sql(client, 'DELETE FROM t WHERE id>=1499 AND id<1502;')
        self.sql(client, 'UPDATE t SET tag=99 WHERE id>=1502 AND id<=1503;')
        vector = '[0' + ',0' * 127 + ']'
        self.sql(client, f"INSERT INTO t VALUES(0,99,'{vector}'),(1499,99,'{vector}'),(3001,99,'{vector}');")
        expected = [[1498], [1499], [1502], [1503], [1504]]
        for order, rows in [('ASC', expected), ('DESC', list(reversed(expected)))]:
            self.assertEqual(self.ids(client,
                f'SELECT id FROM t WHERE id>=1498 AND id<=1504 ORDER BY id {order};'), rows)
        self.assertEqual(self.ids(client,
            'SELECT id FROM t WHERE id>=1498 AND id<=1504 AND tag=99 ORDER BY id LIMIT 2 OFFSET 1;'),
            [[1502], [1503]])
        self.sql(client, 'COMMIT;')
        client.shutdown()
        client = self.open(1)
        self.assertEqual(self.ids(client,
            'SELECT id FROM t WHERE id>=1498 AND id<=1504 ORDER BY id;'), expected)
        self.sql(client, 'BEGIN;')
        self.sql(client, 'DELETE FROM t WHERE id>=1498 AND id<=1504;')
        self.sql(client, 'ROLLBACK;')
        self.assertEqual(self.ids(client,
            'SELECT id FROM t WHERE id>=1498 AND id<=1504 ORDER BY id;'), expected)
        client.shutdown()

    def test_bigint_endpoints_contradictions_and_multiple_order_keys(self):
        client = self.open(64)
        self.sql(client, 'CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT);')
        minimum, maximum = -(1 << 63), (1 << 63) - 1
        keys = [minimum, -1, 0, 1, maximum]
        self.sql(client, 'INSERT INTO t VALUES' + ','.join(f'({key},7)' for key in keys) + ';')
        cases = [
            (f'id>{maximum}', []), (f'id<{minimum}', []),
            (f'id>={minimum} AND id<={maximum}', keys),
            ('id>0 AND id<0', []), ('id>=1 AND id<=-1', []),
            ('id>=-1 AND id<1', [-1, 0]), ('id=0 AND id>0', []),
        ]
        for condition, expected in cases:
            self.assertEqual(self.ids(client,
                f'SELECT id AS k FROM t WHERE {condition} ORDER BY k,tag DESC;'),
                [[key] for key in expected])
        self.sql(client, 'BEGIN;')
        self.sql(client, f'DELETE FROM t WHERE id<={minimum};')
        self.sql(client, f'UPDATE t SET tag=9 WHERE id>={maximum};')
        self.assertEqual(self.ids(client, 'SELECT id FROM t WHERE tag=9 ORDER BY id DESC;'), [[maximum]])
        self.sql(client, 'ROLLBACK;')
        self.assertEqual(self.ids(client, 'SELECT id FROM t ORDER BY id DESC LIMIT 2 OFFSET 1;'), [[1], [0]])
        client.shutdown()

    def test_range_filters_vector_search_and_unordered_mutations(self):
        client = self.open(1)
        self.populate(client)
        self.assertEqual(self.ids(client,
            "SELECT id,L2_DISTANCE(v,'[0" + ',0' * 127 + "]') AS d FROM t "
            'WHERE id>=1198 AND id<1202 ORDER BY d LIMIT 3;'),
            [[1198, 1.0], [1199, 1.0], [1200, 1.0]])
        self.assertEqual(self.sql(client,
            'UPDATE t SET tag=99 WHERE id>=1198 AND id<1202;')['affected_rows'], 4)
        self.assertEqual(self.ids(client,
            'SELECT id FROM t WHERE id>1197 AND id<1202 AND tag=99 ORDER BY tag,id;'),
            [[1198], [1199], [1200], [1201]])
        self.assertEqual(self.sql(client,
            'DELETE FROM t WHERE id>=1198 AND id<1202;')['affected_rows'], 4)
        self.assertEqual(self.ids(client,
            'SELECT id FROM t WHERE id>=1198 AND id<1202;'), [])
        client.shutdown()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    args, rest = parser.parse_known_args()
    PrimaryRanges.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *rest])
