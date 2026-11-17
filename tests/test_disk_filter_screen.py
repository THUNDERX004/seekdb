#!/usr/bin/env python3
"""JSON exact disk-filter boundaries; accepts an existing candidate executable."""

from __future__ import annotations

import argparse
from dataclasses import replace
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_cached_topk as cached
from jsonl_client import JsonlClient
from test_exact_distance import exact_squared, float32, from_bits, vector_sql


DIMENSION, COUNT = 127, 2703
ZERO = (0.0,) * DIMENSION
VECTORS = tuple(
    tuple(float32(((index // 3 * 19 + axis * 7) % 129 - 64) / 32)
          for axis in range(DIMENSION - 1)) + (from_bits(index % 3 + 1),)
    for index in range(37)
)


def fixture():
    return [cached.Row(
        (index * 73) % 10007 - 5000,
        9 if index < 7 else (8 if index % 37 % 3 == 0 else 7),
        "rare" if index < 7 else
        ("red" if index % 37 < 24 else "blue" if index % 37 < 35 else "tail"),
        f"row {index:04d} " + "x" * 35, VECTORS[index % 37],
    ) for index in range(COUNT)]


class DiskFilterTests(unittest.TestCase):
    executable = None
    # Borrow assertions, not the existing test suite. No other engine is an oracle.
    setUp = cached.CachedTopKTests.setUp
    sql = cached.CachedTopKTests.sql
    query = cached.CachedTopKTests.query

    def open_client(self):
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": "1"}):
            client = JsonlClient([self.executable], self.data_dir, timeout=30)
        self.addCleanup(client.close)
        return client

    def create(self, client):
        rows = fixture()
        self.assertGreater(COUNT * DIMENSION * 4, 1024 ** 2)
        self.assertEqual(len({row.vector for row in rows}), 37)
        self.assertEqual(len({row.primary for row in rows}), COUNT)
        # Same double score, different exact lattice score: IDs must expose it.
        scores = [exact_squared(vector, ZERO) for vector in VECTORS[:3]]
        self.assertEqual(len(set(scores)), 3)
        self.assertEqual(len({float(score) for score in scores}), 1)
        self.sql(client, "CREATE TABLE d(id BIGINT PRIMARY KEY,tag BIGINT,"
                 "category VARCHAR(16),payload VARCHAR(64),v VECTOR(127))")
        literals = {vector: cached.quoted(vector_sql(vector)) for vector in VECTORS}
        for start in range(0, COUNT, 150):
            batch = rows[start:start + 150]
            values = ",".join(
                f"({row.primary},{row.tag},{cached.quoted(row.category)},"
                f"{cached.quoted(row.payload)},{literals[row.vector]})" for row in batch)
            self.sql(client, "INSERT INTO d VALUES " + values, len(batch))
        return {row.primary: row for row in rows}

    def check(self, client, rows, probe, where="", predicate=None, limit=4, offset=3):
        unique = {row.vector for row in rows.values()}
        scores = {vector: exact_squared(vector, probe) for vector in unique}
        self.query(client, "d", probe, rows.values(),
                   {row.primary: scores[row.vector] for row in rows.values()},
                   where, predicate, limit=limit, offset=offset, include_vector=False)

    def snapshot(self, client, rows):
        for probe in (VECTORS[19], ZERO, VECTORS[4]):
            self.check(client, rows, probe, "category='red'",
                       lambda row: row.category == "red")
            self.check(client, rows, probe, "category='red' AND tag=7",
                       lambda row: row.category == "red" and row.tag == 7)
            self.check(client, rows, probe, "tag=8",
                       lambda row: row.tag == 8, limit=3, offset=4)

    def update(self, client, row):
        self.sql(client, f"UPDATE d SET tag={row.tag},"
                 f"category={cached.quoted(row.category)},"
                 f"payload={cached.quoted(row.payload)},"
                 f"v={cached.quoted(vector_sql(row.vector))} WHERE id={row.primary}", 1)

    def test_cold_and_warm_filters_exact_lattice_and_small_windows(self):
        for warm in (False, True):
            with self.subTest(warm=warm):
                if warm:
                    temporary = tempfile.TemporaryDirectory(prefix="mvd-disk-filter-hot-")
                    self.addCleanup(temporary.cleanup)
                    self.data_dir = Path(temporary.name)
                client = self.open_client()
                rows = self.create(client)
                if warm:
                    self.check(client, rows, VECTORS[19], limit=7, offset=0)
                # Both cold and warm runs start their filtered reads on a dense bucket.
                self.snapshot(client, rows)
                cases = (
                    ("category='red'", lambda row: row.category == "red", 1, 2),
                    ("tag=7", lambda row: row.tag == 7, 1, 3),
                    ("category='blue'", lambda row: row.category == "blue", 3, 4),
                    ("tag=7 AND category='red'",
                     lambda row: row.tag == 7 and row.category == "red", 4, 3),
                    ("category='tail'", lambda row: row.category == "tail", 7, 7),
                    ("tag=9", lambda row: row.tag == 9, 4, 3),
                    ("category='rare'", lambda row: row.category == "rare", 4, 4),
                    ("category='rare'", lambda row: row.category == "rare", 3, 7),
                    ("tag=99", lambda row: False, 7, 0),
                    ("category='missing'", lambda row: False, 3, 4),
                    ("category='red' AND tag=9", lambda row: False, 4, 3),
                )
                for where, predicate, limit, offset in cases:
                    self.check(client, rows, ZERO, where, predicate, limit, offset)
                # Each payload is unique and long: its equality directory exceeds
                # the 256 KiB metadata cap; warmed full-table codes may be borrowed.
                self.check(client, rows, VECTORS[7], limit=7, offset=0)
                target = fixture()[23]
                for limit, offset in ((1, 0), (3, 1)):
                    for where, predicate in (
                        (f"payload={cached.quoted(target.payload)}",
                         lambda row: row.payload == target.payload),
                        (f"id={target.primary}", lambda row: row.primary == target.primary),
                        (f"id={target.primary} AND tag=99", lambda row: False),
                        ("id=9999999", lambda row: False),
                    ):
                        self.check(client, rows, VECTORS[7], where, predicate, limit, offset)
                client.shutdown()

    def test_mutation_rollback_commit_restart_and_smaller_codes_budget(self):
        client = self.open_client()
        rows = self.create(client)
        self.check(client, rows, VECTORS[19], limit=7, offset=0)
        self.snapshot(client, rows)
        source = fixture()[25]  # blue -> red; change both WHERE columns and vector.
        changed = replace(source, tag=7, category="red", payload="pending", vector=VECTORS[19])
        self.sql(client, "BEGIN")
        self.update(client, changed)
        pending = dict(rows)
        pending[source.primary] = changed
        self.snapshot(client, pending)
        self.sql(client, "ROLLBACK")
        self.snapshot(client, rows)

        # Commit one row entering the bucket and one leaving it.
        leaving = fixture()[19]
        moved = replace(leaving, tag=8, category="blue", payload="left red", vector=VECTORS[4])
        changed = replace(changed, payload="committed")
        self.sql(client, "BEGIN")
        self.update(client, changed)
        self.update(client, moved)
        pending = dict(rows)
        pending.update({changed.primary: changed, moved.primary: moved})
        self.snapshot(client, pending)
        self.sql(client, "COMMIT")
        rows = pending
        self.snapshot(client, rows)
        client.shutdown()
        client = self.open_client()
        self.snapshot(client, rows)  # First read after restart is filtered.
        self.check(client, rows, VECTORS[19], limit=7, offset=0)
        # Four tables divide 1 MiB into 256 KiB/table, smaller than 2703*127 codes.
        for name in ("pressure1", "pressure2", "pressure3"):
            self.sql(client, f"CREATE TABLE {name}(id BIGINT PRIMARY KEY)")
        self.snapshot(client, rows)
        client.shutdown()

    def test_warmed_corrupt_matching_nonresult_row_reports_corrupt_data(self):
        client = self.open_client()
        rows = self.create(client)
        probe = VECTORS[19]
        self.check(client, rows, probe, limit=7, offset=0)
        predicate = lambda row: row.category == "red"
        where = "category='red'"
        self.check(client, rows, probe, where, predicate, limit=7, offset=0)
        victim = fixture()[-1]  # Last appended row, matching WHERE but far from TopK.
        self.assertTrue(predicate(victim))
        ordered = sorted((row for row in rows.values() if predicate(row)),
                         key=lambda row: (exact_squared(row.vector, probe), row.primary))
        self.assertNotIn(victim.primary, [row.primary for row in ordered[:7]])
        self.assertGreater(exact_squared(victim.vector, probe),
                           exact_squared(ordered[6].vector, probe))
        paths = list(self.data_dir.glob("*.dat"))
        self.assertEqual(len(paths), 1)
        path = paths[0]
        stamp = path.stat()
        with path.open("r+b") as stream:
            stream.seek(-1, os.SEEK_END)
            byte = stream.read(1)
            self.assertEqual(len(byte), 1)
            stream.seek(-1, os.SEEK_END)
            stream.write(bytes([byte[0] ^ 1]))  # Finite float payload, invalid CRC.
            stream.flush()
            os.fsync(stream.fileno())
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 2_000_000_000))
        response = client.execute(
            f"SELECT id,L2_DISTANCE(v,{cached.quoted(vector_sql(probe))}) AS d "
            f"FROM d WHERE {where} ORDER BY d LIMIT 7")
        self.assertIs(response.get("ok"), False, response)
        self.assertEqual(response.get("error", {}).get("code"), "CORRUPT_DATA", response)
        client.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="existing candidate binary; no build")
    args, remaining = parser.parse_known_args()
    DiskFilterTests.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *remaining])
