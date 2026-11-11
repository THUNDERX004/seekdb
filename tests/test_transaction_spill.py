#!/usr/bin/env python3
"""Exercise SQL and recovery with transaction payloads forced onto /data."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from jsonl_client import JsonlClient


class TransactionSpill(unittest.TestCase):
    executable = None

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mvd-transaction-spill-')
        self.addCleanup(self.temp.cleanup)

    def open(self, budget=0):
        with patch.dict(os.environ, {'MVD_CACHE_LIMIT_MB': '1',
                                     'MVD_TRANSACTION_MEMORY_MB': str(budget)}):
            client = JsonlClient([self.executable], Path(self.temp.name), timeout=15)
        self.addCleanup(client.close)
        return client

    def sql(self, client, statement):
        response = client.execute(statement)
        self.assertTrue(response['ok'], response)
        return response

    def rows(self, client, statement):
        return self.sql(client, statement)['rows']

    def no_scratch(self):
        self.assertFalse(list(Path(self.temp.name).glob('.mvd-transaction-*')))

    def create(self, client):
        self.sql(client, 'CREATE TABLE t(id BIGINT PRIMARY KEY,tag VARCHAR(16),v VECTOR(2));')
        self.sql(client, "INSERT INTO t VALUES(1,'base','[3,4]'),(5,'base','[0,0]');")

    def test_spilled_reads_sort_topk_and_rollback(self):
        client = self.open()
        self.create(client)
        self.sql(client, 'BEGIN;')
        self.sql(client, "INSERT INTO t VALUES(9,'c','[1,2]'),(3,'a','[1,0]'),(7,'a','[2,0]');")
        self.sql(client, "UPDATE t SET tag='b' WHERE id>=7;")
        self.sql(client, "UPDATE t SET v='[0,2]' WHERE id=9;")
        self.sql(client, 'DELETE FROM t WHERE id=5;')
        self.assertEqual(self.rows(client, 'SELECT id,tag FROM t ORDER BY tag DESC;'),
                         [[1, 'base'], [7, 'b'], [9, 'b'], [3, 'a']])
        self.assertEqual(self.rows(client, 'SELECT id FROM t WHERE tag=\'b\' ORDER BY id DESC;'),
                         [[9], [7]])
        self.assertEqual(self.rows(client, 'SELECT id,v FROM t WHERE id>=3 ORDER BY id DESC LIMIT 2;'),
                         [[9, [0, 2]], [7, [2, 0]]])
        expected = [[3, 1.0], [7, 2.0], [9, 2.0]]
        self.assertEqual(self.rows(client,
            "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 3;"), expected)
        self.assertEqual(self.rows(client,
            "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d,tag LIMIT 3;"), expected)
        self.sql(client, 'ROLLBACK;')
        self.assertEqual(self.rows(client, 'SELECT id,tag,v FROM t ORDER BY id;'),
                         [[1, 'base', [3, 4]], [5, 'base', [0, 0]]])
        self.no_scratch()
        client.shutdown()

    def test_multitable_commit_and_crash_recovery(self):
        client = self.open()
        self.create(client)
        self.sql(client, 'CREATE TABLE u(id BIGINT PRIMARY KEY,s VARCHAR(16));')
        self.sql(client, "INSERT INTO u VALUES(1,'original');")
        self.sql(client, 'BEGIN;')
        self.sql(client, "UPDATE t SET tag='new',v='[8,6]';")
        self.sql(client, 'DELETE FROM t WHERE id=1;')
        self.sql(client, "INSERT INTO t VALUES(1,'replaced','[6,8]');")
        self.sql(client, "UPDATE u SET s='committed';")
        self.sql(client, 'COMMIT;')
        self.assertEqual(self.rows(client,
            "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;"),
                         [[1, 10.0], [5, 10.0]])
        client.shutdown()
        self.no_scratch()
        client = self.open()
        expected = [[1, 'replaced', [6, 8]], [5, 'new', [8, 6]]]
        self.assertEqual(self.rows(client, 'SELECT id,tag,v FROM t ORDER BY id;'), expected)
        self.assertEqual(self.rows(client, 'SELECT id,s FROM u;'), [[1, 'committed']])
        self.sql(client, 'BEGIN;')
        self.sql(client, "UPDATE t SET tag='uncommitted';")
        self.sql(client, "INSERT INTO u VALUES(2,'uncommitted');")
        client.close()
        self.no_scratch()
        client = self.open()
        self.assertEqual(self.rows(client, 'SELECT id,tag,v FROM t ORDER BY id;'), expected)
        self.assertEqual(self.rows(client, 'SELECT id,s FROM u;'), [[1, 'committed']])
        client.shutdown()

    def test_failed_statement_keeps_prior_spilled_changes(self):
        client = self.open()
        self.create(client)
        self.sql(client, 'BEGIN;')
        self.sql(client, "UPDATE t SET tag='prior' WHERE id=1;")
        response = client.execute("INSERT INTO t VALUES(2,'new','[1,0]'),(5,'duplicate','[0,1]');")
        self.assertFalse(response['ok'])
        self.assertEqual(response['error']['code'], 'DUPLICATE_PRIMARY_KEY')
        self.assertEqual(self.rows(client, 'SELECT id,tag FROM t ORDER BY id;'),
                         [[1, 'prior'], [5, 'base']])
        self.sql(client, 'COMMIT;')
        client.shutdown()
        client = self.open()
        self.assertEqual(self.rows(client, "SELECT id FROM t WHERE tag='prior';"), [[1]])
        client.shutdown()
        self.no_scratch()

    def test_shutdown_discards_spilled_transaction(self):
        client = self.open()
        self.create(client)
        self.sql(client, 'BEGIN;')
        self.sql(client, "INSERT INTO t VALUES(11,'private','[1,0]');")
        self.sql(client, "UPDATE t SET tag='private';")
        client.shutdown()
        self.no_scratch()
        client = self.open()
        self.assertEqual(self.rows(client, 'SELECT id,tag FROM t ORDER BY id;'),
                         [[1, 'base'], [5, 'base']])
        client.shutdown()

    def test_mixed_resident_and_spilled_rows(self):
        client = self.open(1)
        self.sql(client, 'CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(512));')
        self.sql(client, 'BEGIN;')
        tail = ',0' * 511 + ']'
        for start in range(1, 601, 50):
            values = ','.join(f"({i},{i % 7},'[{i}{tail}')" for i in range(start, start + 50))
            self.sql(client, 'INSERT INTO t VALUES' + values + ';')
        self.sql(client, 'UPDATE t SET tag=99 WHERE id>=590;')
        self.assertEqual(self.rows(client, 'SELECT id,tag FROM t ORDER BY id DESC LIMIT 3;'),
                         [[600, 99], [599, 99], [598, 99]])
        query = "SELECT id,L2_DISTANCE(v,'[0" + ',0' * 511 + "]') AS d FROM t ORDER BY d LIMIT 3;"
        self.assertEqual(self.rows(client, query), [[1, 1.0], [2, 2.0], [3, 3.0]])
        self.sql(client, 'COMMIT;')
        client.shutdown()
        client = self.open()
        self.assertEqual(self.rows(client, 'SELECT id FROM t WHERE tag=99 ORDER BY id;'),
                         [[i] for i in range(590, 601)])
        self.assertEqual(self.rows(client, query), [[1, 1.0], [2, 2.0], [3, 3.0]])
        client.shutdown()
        self.no_scratch()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    args, remaining = parser.parse_known_args()
    TransactionSpill.executable = args.executable.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
