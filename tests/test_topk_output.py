#!/usr/bin/env python3
"""Check exact ranking and identical distance display across SQL execution paths."""
from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

from jsonl_client import JsonlClient
from test_cached_topk import quoted
from test_exact_distance import exact_squared, float32, from_bits, vector_sql


class TopKOutputTests(unittest.TestCase):
    executable = None

    def sql(self, client, text):
        result = client.execute(text)
        self.assertIs(result.get("ok"), True, result)
        return result.get("rows", [])

    def verify(self, client, table, vectors, probe):
        literal = quoted(vector_sql(probe))
        other = quoted(vector_sql([-value for value in probe]))
        scores = {key: exact_squared(vector, probe) for key, vector in vectors.items()}
        ranked = sorted(vectors, key=lambda key: (scores[key], key))
        for where, keys in (("", ranked), ("WHERE tag=1 ", [key for key in ranked if key % 3 == 1])):
            for limit, offset in ((1, 0), (10, 3), (100, 0)):
                suffix = f"FROM {table} {where}ORDER BY d,id LIMIT {limit} OFFSET {offset}"
                # Two selected distances force the general path. The second
                # expression differs, detecting accidental reuse for a wrong probe.
                fast = self.sql(client, f"SELECT id,L2_DISTANCE(v,{literal}) AS d {suffix}")
                general = self.sql(client, f"SELECT id,L2_DISTANCE(v,{literal}) AS d,"
                                   f"L2_DISTANCE(v,{other}) AS e {suffix}")
                self.assertEqual([row[0] for row in fast], keys[offset:offset + limit])
                self.assertEqual(fast, [row[:2] for row in general])
                for key, displayed in fast:
                    # A primary-key lookup bypasses the TopK executor entirely.
                    direct = self.sql(client, f"SELECT L2_DISTANCE(v,{literal}) AS d "
                                      f"FROM {table} WHERE id={key}")
                    self.assertEqual(displayed, direct[0][0])
                    self.assertTrue(math.isfinite(displayed))

    def test_cached_and_disk_distance_display(self):
        rng = random.Random(20261116)
        for cache_mb in (64, 1):
            with self.subTest(cache_mb=cache_mb), tempfile.TemporaryDirectory() as temporary:
                data = Path(temporary)
                with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(cache_mb)}):
                    client = JsonlClient([self.executable], data, timeout=30)
                try:
                    for dimension in (7, 65, 128):
                        table = f"t{dimension}"
                        # Payload exceeds the small cache independently of vector
                        # size, so the same checks also cover ephemeral disk rows.
                        self.sql(client, f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,tag BIGINT,"
                                 f"body VARCHAR(24000),v VECTOR({dimension}))")
                        vectors = {}
                        for index in range(64):
                            key = (index * 19) % 64 - 32
                            vector = [float32(rng.uniform(-3, 3)) for _ in range(dimension)]
                            if index < 4:
                                vector = [float32((index - 2) / 16)] * dimension
                            vectors[key] = vector
                        # Include actual ties with independently permuted PKs.
                        vectors[31] = vectors[-31][:]
                        payload = quoted("ordinary payload " + "x" * 21000)
                        for start in range(0, 64, 16):
                            batch = list(vectors.items())[start:start + 16]
                            values = ",".join(f"({key},{key % 3},{payload},"
                                              f"{quoted(vector_sql(vector))})"
                                              for key, vector in batch)
                            self.sql(client, f"INSERT INTO {table} VALUES " + values)
                        probes = [[float32(rng.uniform(-1, 1)) for _ in range(dimension)],
                                  [0.0] * dimension]
                        for probe in probes:
                            self.verify(client, table, vectors, probe)
                        client.shutdown()
                        client.close()
                        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(cache_mb)}):
                            client = JsonlClient([self.executable], data, timeout=30)
                        self.verify(client, table, vectors, probes[0])
                    # Nonfinite screening estimates must fall back to double;
                    # tiny coordinates must still participate in exact ties.
                    self.sql(client, "CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(17))")
                    huge, tiny = from_bits(0x7F7FFFFF), from_bits(1)
                    vectors = {key: [huge if key % 2 else -huge, tiny * (key + 3)]
                               + [0.0] * 15 for key in range(-3, 4)}
                    self.sql(client, "INSERT INTO t VALUES " + ",".join(
                        f"({key},{key % 3},{quoted(vector_sql(vector))})"
                        for key, vector in vectors.items()))
                    self.verify(client, "t", vectors, [tiny, -tiny] + [0.0] * 15)
                finally:
                    client.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args, remaining = parser.parse_known_args()
    TopKOutputTests.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *remaining])
