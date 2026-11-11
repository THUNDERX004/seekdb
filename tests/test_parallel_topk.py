#!/usr/bin/env python3
"""Large cached exact-TopK regressions against an already-built executable.

Run: python -X utf8 -B tests/test_parallel_topk.py /path/to/minivectordb
No timing assertions or builds. SQL cannot prove that workers actually ran;
65539 candidates cross the parallel gate and split at 32769, off a SIMD block.
Expected rows come from one global Python sort of exact integer distances,
never from database ordering or a reproduction of worker partitions/merging.
"""

from __future__ import annotations

import argparse
from dataclasses import replace
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import os

from jsonl_client import JsonlClient
from test_cached_topk import Row, quoted
from test_exact_distance import exact_squared, vector_sql


DIMENSION = 64
ROW_COUNT = 65539
PARALLEL_MINIMUM = 65536
ZERO = (0.0,) * DIMENSION
PROTOTYPES = tuple(
    tuple(-1.0 if ((axis & mask).bit_count() + mask) % 2 else 1.0
          for axis in range(DIMENSION))
    for mask in range(8)
)


def axis_vector(axis):
    return tuple(8.0 if index == axis else 0.0 for index in range(DIMENSION))


TAIL_PROBE = axis_vector(63)
REINSERT_PROBE = axis_vector(62)
# Ordinary rows' double distances collide here, although their exact distances
# differ. This probe exceeds the quantized range: it checks the serial range
# fallback, not worker merging, and must still rank by exact distance.
HUGE_PROBE = (math.ldexp(1.0, 100),) + (0.0,) * (DIMENSION - 1)


def fixture():
    sentinels = dict(zip((0, 1, 32768, 32769, 32770, 65536, 65537, 65538),
                        (7, 15, 23, 31, 39, 47, 55, 63)))
    rows = []
    for index in range(ROW_COUNT):
        # Permute PKs independently of insertion/vector order. Scan order is
        # deliberately unspecified: allocation addresses are not an oracle.
        primary = (index * 37) % ROW_COUNT - ROW_COUNT // 2
        vector = (axis_vector(sentinels[index]) if index in sentinels
                  else PROTOTYPES[index % len(PROTOTYPES)])
        included = index < PARALLEL_MINIMUM
        rows.append(Row(primary, 7 if included else 8,
                        "all" if included else "outside",
                        f"row {index}; 'quote' \\ 中文", vector))
    return rows


class ParallelTopKTests(unittest.TestCase):
    executable = None
    timeout = 120.0
    tmp_root = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("supply an existing database executable")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-parallel-topk-",
                                                dir=self.tmp_root)
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)
        self.query_count = 0

    def open_client(self):
        # Keep this a cached-list test even if a caller ran a low-budget suite
        # immediately beforehand. No worker/SIMD implementation flags are set.
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": "192"}):
            client = JsonlClient([self.executable], self.data_dir,
                                 timeout=self.timeout)
        self.addCleanup(client.close)
        return client

    def sql(self, client, statement, affected=None):
        response = client.execute(statement)
        self.assertIs(response.get("ok"), True, response)
        self.assertNotIn("error", response)
        if affected is not None:
            self.assertIs(type(response.get("affected_rows")), int, response)
            self.assertEqual(response["affected_rows"], affected, statement[:200])
        return response

    def insert(self, client, rows):
        rows = list(rows)
        literals = {}
        for start in range(0, len(rows), 512):
            batch = rows[start:start + 512]
            values = []
            for row in batch:
                if row.vector not in literals:
                    literals[row.vector] = quoted(vector_sql(row.vector))
                # PK is deliberately in the middle of the physical schema.
                values.append(f"({quoted(row.category)},{literals[row.vector]},"
                              f"{row.primary},{row.tag},{quoted(row.payload)})")
            self.sql(client, "INSERT INTO t VALUES " + ",".join(values), len(batch))

    def update(self, client, row):
        self.sql(client, f"UPDATE t SET cat={quoted(row.category)},"
                 f"v={quoted(vector_sql(row.vector))},tag={row.tag},"
                 f"payload={quoted(row.payload)} WHERE id={row.primary}", 1)

    def check(self, client, rows, probe, *, where="", limit=7, offset=0,
              explicit_pk=False):
        if where == "cat='all'":
            candidates = [row for row in rows.values() if row.category == "all"]
        elif where == "tag=7":
            candidates = [row for row in rows.values() if row.tag == 7]
        else:
            self.assertEqual(where, "")
            candidates = list(rows.values())
        self.assertGreaterEqual(len(candidates), PARALLEL_MINIMUM)
        # Few unique vectors keep the large fixture's exact lattice oracle
        # bounded. Python integers, including the huge-probe low bits, rank it.
        scores = {vector: exact_squared(vector, probe)
                  for vector in {row.vector for row in candidates}}
        ordered = sorted(candidates, key=lambda row: (scores[row.vector], row.primary))
        expected = ordered[offset:offset + limit]
        expression = f"L2_DISTANCE(v,{quoted(vector_sql(probe))})"
        order = "d,id ASC" if explicit_pk else "d"
        statement = (f"SELECT id,tag,cat,payload,v,{expression} AS d FROM t "
                     + (f"WHERE {where} " if where else "")
                     + f"ORDER BY {order} LIMIT {limit} OFFSET {offset}")
        response = self.sql(client, statement)
        self.query_count += 1
        diagnostic = f"query {self.query_count}: {where!r}, K={limit}, OFFSET={offset}"
        self.assertEqual(response.get("columns"),
                         ["id", "tag", "cat", "payload", "v", "d"], diagnostic)
        actual = response.get("rows")
        self.assertIs(type(actual), list, diagnostic)
        self.assertTrue(all(isinstance(row, list) and len(row) == 6 for row in actual),
                        diagnostic)
        self.assertEqual([row[0] for row in actual],
                         [row.primary for row in expected], diagnostic)
        for returned, row in zip(actual, expected):
            self.assertIs(type(returned[0]), int, diagnostic)
            self.assertIs(type(returned[1]), int, diagnostic)
            self.assertIs(type(returned[2]), str, diagnostic)
            self.assertIs(type(returned[3]), str, diagnostic)
            self.assertEqual(returned[:4],
                             [row.primary, row.tag, row.category, row.payload], diagnostic)
            self.assertIs(type(returned[4]), list, diagnostic)
            self.assertEqual(len(returned[4]), DIMENSION, diagnostic)
            for component, wanted in zip(returned[4], row.vector):
                self.assertIn(type(component), (int, float), diagnostic)
                self.assertEqual(component, wanted, diagnostic)
            distance = returned[5]
            self.assertIn(type(distance), (int, float), diagnostic)
            self.assertTrue(math.isfinite(distance) and distance >= 0, diagnostic)
            wanted = math.sqrt(math.ldexp(float(scores[row.vector]), -298))
            self.assertTrue(math.isclose(distance, wanted, rel_tol=1e-5, abs_tol=1e-5),
                            f"{diagnostic}: {distance!r} != {wanted!r}")
            if wanted.is_integer() and wanted <= (1 << 53):
                self.assertEqual(distance, wanted, diagnostic)

    def test_large_merge_boundaries_mutations_and_restart(self):
        source = fixture()
        rows = {row.primary: row for row in source}
        self.assertEqual(len(rows), ROW_COUNT)
        self.assertEqual(ROW_COUNT // 2, 32769)
        self.assertEqual((ROW_COUNT // 2) % 16, 1)
        self.assertEqual(sum(row.category == "all" for row in source), PARALLEL_MINIMUM)
        self.assertTrue(all(sum(value * value for value in vector) == 64
                            for vector in {row.vector for row in source}))
        huge_scores = {exact_squared(vector, HUGE_PROBE)
                       for vector in PROTOTYPES}
        self.assertGreater(len(huge_scores), 1)
        self.assertEqual(len({float(score) for score in huge_scores}), 1,
                         "fixture must distinguish exact rank from double distance")

        client = self.open_client()
        self.sql(client, "CREATE TABLE t(cat VARCHAR(16),v VECTOR(64),"
                 "id BIGINT PRIMARY KEY,tag BIGINT,payload VARCHAR(80))")
        self.insert(client, source)
        # Every row is exactly distance 8 from ZERO, so ties necessarily span
        # both halves. Distinct probes also expose skipped/misaddressed codes.
        self.check(client, rows, ZERO, limit=17)
        self.check(client, rows, ZERO, limit=1, offset=19, explicit_pk=True)
        self.check(client, rows, PROTOTYPES[3], where="cat='all'", limit=7, offset=2)
        self.check(client, rows, PROTOTYPES[5], where="tag=7", limit=31, offset=16)
        self.check(client, rows, TAIL_PROBE, limit=3)
        self.check(client, rows, HUGE_PROBE, limit=13, offset=7, explicit_pk=True)

        moving = source[32769].primary
        deleted = source[-1].primary
        old_winner = min(rows)
        self.sql(client, "BEGIN")
        pending = rows.copy()
        changed = replace(rows[moving], vector=ZERO, payload="private update '中文'")
        self.update(client, changed)
        pending[moving] = changed
        self.sql(client, f"DELETE FROM t WHERE id={old_winner}", 1)
        del pending[old_winner]
        inserted = Row(-(1 << 63), 7, "all", "private insert", ZERO)
        self.insert(client, [inserted])
        pending[inserted.primary] = inserted
        self.check(client, pending, ZERO, limit=9)
        self.sql(client, "ROLLBACK")
        self.check(client, rows, ZERO, limit=11, offset=2)

        # Separate committed publications invalidate warmed descriptors/codes;
        # retained worker state must not keep the old row/vector pointers.
        changed = replace(rows[moving], vector=ZERO, payload="committed update '中文'")
        self.update(client, changed)
        rows[moving] = changed
        self.check(client, rows, ZERO, limit=5)
        self.sql(client, f"DELETE FROM t WHERE id={deleted}", 1)
        del rows[deleted]
        self.check(client, rows, TAIL_PROBE, limit=7, offset=1)
        inserted = Row((1 << 63) - 1, 7, "all", "committed insert \\ 中文", TAIL_PROBE)
        self.insert(client, [inserted])
        rows[inserted.primary] = inserted
        self.check(client, rows, TAIL_PROBE, where="cat='all'", limit=5)
        reinserted = Row(deleted, 7, "all", "same PK, new allocation", REINSERT_PROBE)
        self.insert(client, [reinserted])
        rows[deleted] = reinserted
        self.check(client, rows, REINSERT_PROBE, limit=7)
        self.check(client, rows, PROTOTYPES[5], where="tag=7", limit=17, offset=16)
        client.shutdown()
        client.close()

        client = self.open_client()
        # Cold filtered-first preparation after recovery, then reuse/full scan.
        self.check(client, rows, TAIL_PROBE, where="cat='all'", limit=11)
        self.check(client, rows, REINSERT_PROBE, limit=7)
        self.check(client, rows, ZERO, limit=19)
        self.assertEqual(self.query_count, 16)
        client.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--tmp-root", type=Path)
    args, remaining = parser.parse_known_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    executable = args.executable.resolve(strict=True)
    if not executable.is_file():
        parser.error("executable must be a file")
    if args.tmp_root is not None and not args.tmp_root.is_dir():
        parser.error("--tmp-root must be an existing directory")
    ParallelTopKTests.executable = str(executable)
    ParallelTopKTests.timeout = args.timeout
    ParallelTopKTests.tmp_root = args.tmp_root
    unittest.main(argv=[sys.argv[0], *remaining])
