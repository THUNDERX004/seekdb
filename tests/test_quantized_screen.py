#!/usr/bin/env python3
"""Focused exact TopK black-box regressions against an already-built executable.

Run: python -X utf8 -B tests/test_quantized_screen.py /path/to/minivectordb
The same cases apply to SIMD and forced-scalar builds. Candidate counts and
numeric fixtures exercise the screening gates, but SQL cannot observe which
kernel ran. Expected ranks come only from the independent float32 integer
lattice oracle. Large dimensions use few unique vectors and small results.
"""

from __future__ import annotations

import argparse
from dataclasses import replace
import math
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

import test_cached_topk as cached
from test_exact_distance import exact_squared, from_bits, vector_sql


Row = cached.Row
quoted = cached.quoted


def repeated_rows(vectors, count=259, outsiders=4):
    """A real equality subset: >=256 red rows plus nonmatching blue rows."""
    rows = [Row((index * 73) % 8191 - 4095, 7, "red", f"red {index}",
                vectors[index % len(vectors)]) for index in range(count)]
    rows += [Row(100000 + index, 8, "blue", f"blue {index}",
                 vectors[index % len(vectors)]) for index in range(outsiders)]
    return rows


def exact_scores(rows, probe):
    # Memoize identical vectors, especially the 16384/16385-dimensional cases;
    # every unique distance still uses exact_squared, never a quantized oracle.
    distinct = {}
    scores = {}
    for row in rows:
        if row.vector not in distinct:
            distinct[row.vector] = exact_squared(row.vector, probe)
        scores[row.primary] = distinct[row.vector]
    return scores


class QuantizedScreenTests(unittest.TestCase):
    executable = None

    # Borrow helpers without subclassing CachedTopKTests: unittest must not
    # collect its test_* methods a second time in this focused suite.
    setUp = cached.CachedTopKTests.setUp
    sql = cached.CachedTopKTests.sql
    create = cached.CachedTopKTests.create
    query = cached.CachedTopKTests.query

    def open_client(self, cache_mb=192):
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(cache_mb)}):
            return cached.CachedTopKTests.open_client(self)

    def insert(self, client, table, rows):
        rows = list(rows)
        literals = {}
        batch_size = max(1, min(520, 262144 // len(rows[0].vector)))
        for start in range(0, len(rows), batch_size):
            batch = rows[start:start + batch_size]
            values = []
            for row in batch:
                if row.vector not in literals:
                    literals[row.vector] = quoted(vector_sql(row.vector))
                values.append(f"({row.primary},{row.tag},{quoted(row.category)},"
                              f"{quoted(row.payload)},{literals[row.vector]})")
            self.sql(client, f"INSERT INTO {table} VALUES " + ",".join(values),
                     len(batch))

    def check(self, client, table, rows, probe, *, where="", predicate=None,
              limit=7, offset=2, scores=None):
        rows = list(rows)
        candidates = [row for row in rows if predicate is None or predicate(row)]
        self.assertGreaterEqual(len(candidates), 256, (table, where))
        self.assertEqual(len({row.primary for row in rows}), len(rows))
        if predicate is not None:
            self.assertLess(len(candidates), len(rows), "equality must exclude rows")
        if scores is None:
            scores = exact_scores(rows, probe)
        self.query(client, table, probe, rows, scores, where, predicate,
                   limit=limit, offset=offset, include_vector=False)

    def check_full_and_bucket(self, client, table, rows, probe, *, limit=7,
                              offset=2):
        scores = exact_scores(rows, probe)
        # Start with equality, then full, then the same equality again: both
        # candidate views can coexist, and the last query reuses a warm bucket.
        for where, predicate in (("tag=7", lambda row: row.tag == 7),
                                 ("", None),
                                 ("tag=7", lambda row: row.tag == 7)):
            self.check(client, table, rows, probe, where=where,
                       predicate=predicate, limit=limit, offset=offset,
                       scores=scores)

    def test_half_steps_clamping_ties_offsets_and_block_tails(self):
        client = self.open_client()
        # Adjacent binary32 values on either side of half steps, including the
        # rounding/clamping boundary at +/-127.5 and the last value below 128.
        positive = [from_bits(center + delta)
                    for center in (0x3F000000, 0x3FC00000, 0x42FD0000,
                                   0x42FE0000, 0x42FF0000)
                    for delta in (-1, 0, 1)] + [from_bits(0x42FFFFFF)]
        for dimension, count, exponent in ((16, 256, -20), (17, 257, -3),
                                            (127, 2048, 0), (128, 2049, 12),
                                            (129, 2050, 60)):
            with self.subTest(dimension=dimension, bucket=count):
                scale = math.ldexp(1.0, exponent)

                def alternating(value):
                    return tuple(value * scale * (-1 if axis % 2 else 1)
                                 for axis in range(dimension))

                vectors = [alternating(sign * value)
                           for sign in (1, -1) for value in positive]
                # Equal prefixes with different final components make the
                # dimensional tail determine membership, not just display d.
                for center in (0x3F000000, 0x42FF0000):
                    for delta in (-1, 1):
                        vector = list(alternating(from_bits(center)))
                        vector[-1] = (from_bits(center + delta) * scale
                                      * (-1 if (dimension - 1) % 2 else 1))
                        vectors.append(tuple(vector))
                rows = repeated_rows(vectors, count=count)
                self.assertEqual(sum(row.tag == 7 for row in rows), count)
                self.assertEqual(len(rows) % 4, count % 4)
                table = f"boundaries_{dimension}"
                self.create(client, table, dimension, rows)
                for value in (0.5, 127.5, -127.5):
                    probe = alternating(value)
                    scores = exact_scores(rows, probe)
                    self.assertLess(len(set(scores.values())), len(rows),
                                    "fixture must contain exact ties")
                    self.check_full_and_bucket(client, table, rows, probe)
                self.check(client, table, rows, alternating(0.5), limit=1, offset=0)
        client.shutdown()

    def test_out_of_range_zero_extreme_subnormal_and_near_ulp(self):
        client = self.open_client()
        dimension = 17
        zero = (0.0,) * dimension
        edge = from_bits(0x42FFFFFF)
        bounded = [(value,) + (0.0,) * 15 + (tail,)
                   for value in (-edge, -127.5, -0.5, 0.5, 127.5, edge)
                   for tail in (-0.5, 0.5)]
        rows = repeated_rows(bounded)
        self.create(client, "out_of_range", dimension, rows)
        self.check_full_and_bucket(client, "out_of_range", rows, zero)
        # Exactly +/-128 must fall back as well as values beyond the gate.
        # Put the sole out-of-range component last to check every query axis.
        for value in (128.0, -128.0, from_bits(0x43000001),
                      -from_bits(0x43000001), math.ldexp(1.0, 100)):
            with self.subTest(out_of_range=value):
                probe = (0.5,) + (0.0,) * 15 + (value,)
                self.check_full_and_bucket(client, "out_of_range", rows, probe,
                                           limit=3, offset=1)
        self.check_full_and_bucket(client, "out_of_range", rows, zero)

        tiny, largest = from_bits(1), from_bits(0x7F7FFFFF)
        near_unit = []
        for rank in (6, 4, 1, 2, 3, 5):
            vector = [1.0] * dimension
            vector[0] = from_bits(0x3F800000 + rank)
            vector[-1] = from_bits(0x3F800000 - 2 * rank)
            near_unit.append(tuple(vector))
        subnormals = [(from_bits(bits),) + (0.0,) * 15 + (-tiny,)
                      for bits in (0, 1, 2, 3, 0x007FFFFF, 0x00800000,
                                   0x00800001, 0x80000001, 0x807FFFFF,
                                   0x80800000)]
        extremes = [(largest,) * 16 + (rank * tiny,)
                    for rank in (7, 5, 1, 3, -1, -3)]
        cases = (
            ("zeros", [zero, (-0.0,) * dimension], [zero]),
            ("subnormals", subnormals, [(tiny,) + (0.0,) * 15 + (-tiny,)]),
            ("near_ulp", near_unit, [zero]),
            ("extremes", extremes,
             [(-largest,) * 16 + (tiny,), (largest,) * 16 + (tiny,)]),
        )
        for table, vectors, probes in cases:
            with self.subTest(table=table):
                rows = repeated_rows(vectors)
                self.create(client, table, dimension, rows)
                for probe in probes:
                    distinct = {exact_squared(vector, probe) for vector in vectors}
                    if table in ("near_ulp", "extremes"):
                        self.assertGreater(len(distinct), 1)
                    if table == "extremes" and probe[0] < 0:
                        self.assertEqual(len({float(score) for score in distinct}), 1,
                                         "double scores must not determine rank")
                    self.check_full_and_bucket(client, table, rows, probe,
                                               limit=9, offset=3)
        client.shutdown()

    def test_dimension_gate_16384_and_16385(self):
        client = self.open_client()
        # Only eight unique dense vectors, two SELECTs per dimension, no vector
        # projection or full sort. This bounds work within the suite timeout.
        for dimension in (16384, 16385):
            with self.subTest(dimension=dimension):
                vectors = [(sign * 127.0,) * (dimension - 1) + (126.5 - rank,)
                           for sign in (1, -1) for rank in range(4)]
                rows = repeated_rows(vectors, count=256)
                probe = (127.0,) * dimension
                table = f"dimension_{dimension}"
                self.create(client, table, dimension, rows)
                scores = exact_scores(rows, probe)
                for where, predicate in (("tag=7", lambda row: row.tag == 7),
                                         ("", None)):
                    self.check(client, table, rows, probe, where=where,
                               predicate=predicate, limit=3, offset=1, scores=scores)
        client.shutdown()

    def warm_views(self, client, rows):
        rows = list(rows.values())
        probe = (0.0,) * len(rows[0].vector)
        scores = exact_scores(rows, probe)
        # Warm two real >=2048 buckets and the full view together, then switch
        # equality columns to invalidate/rebuild their shared cache accounting.
        for where, predicate in (
            ("tag=7", lambda row: row.tag == 7),
            ("tag=8", lambda row: row.tag == 8),
            ("", None),
            ("tag=7", lambda row: row.tag == 7),
            ("category='red'", lambda row: row.category == "red"),
            ("category='blue'", lambda row: row.category == "blue"),
            ("", None),
            ("category='red'", lambda row: row.category == "red"),
        ):
            if predicate is not None:
                self.assertGreaterEqual(sum(predicate(row) for row in rows), 2048)
            self.check(client, "mutable", rows, probe, where=where,
                       predicate=predicate, limit=5, offset=0, scores=scores)

    def update_row(self, client, row):
        self.sql(client, "UPDATE mutable SET "
                 f"tag={row.tag},category={quoted(row.category)},"
                 f"payload={quoted(row.payload)},v={quoted(vector_sql(row.vector))} "
                 f"WHERE id={row.primary}", 1)

    def test_warm_views_mutations_rollback_commit_and_restart(self):
        client = self.open_client()
        # A non-multiple of four above the VNNI layout gate exercises packed
        # axis padding through every invalidation, overlay and restart path.
        dimension = 65
        fixture = [Row((index * 73) % 4177 - 2088,
                       7 if index < 2080 else 8,
                       "red" if index < 2080 else "blue", f"initial {index}",
                       (32.0 + index % 83,) + (0.0,) * (dimension - 2)
                       + (((index * 7) % 29 - 14) / 8,))
                   for index in range(4160)]
        rows = {row.primary: row for row in fixture}
        self.create(client, "mutable", dimension, fixture)
        self.warm_views(client, rows)
        self.sql(client, "CREATE VECTOR INDEX mutable_ix ON mutable(v)")
        self.warm_views(client, rows)

        changed = replace(fixture[20], tag=8, category="blue", payload="updated",
                          vector=(0.0,) * dimension)
        self.update_row(client, changed)
        rows[changed.primary] = changed
        self.warm_views(client, rows)
        additions = [Row(-10001, 7, "red", "new nearest", (0.0,) * dimension),
                     Row(10001, 8, "blue", "new scale", (4096.0,) + (0.0,) * (dimension - 1))]
        self.insert(client, "mutable", additions)
        rows.update((row.primary, row) for row in additions)
        self.warm_views(client, rows)
        self.sql(client, f"DELETE FROM mutable WHERE id={changed.primary}", 1)
        del rows[changed.primary]
        self.warm_views(client, rows)

        for commit in (False, True):
            with self.subTest(commit=commit):
                self.sql(client, "BEGIN")
                pending = rows.copy()
                moved = replace(rows[-10001], tag=8, category="blue",
                                payload="committed" if commit else "rolled back",
                                vector=(0.25,) + (0.0,) * (dimension - 1))
                self.update_row(client, moved)
                pending[moved.primary] = moved
                # Remove the previous scale maximum, forcing a different code
                # scale on commit while rollback must preserve the old view.
                self.sql(client, "DELETE FROM mutable WHERE id=10001", 1)
                del pending[10001]
                inserted = Row(-10002, 7, "red", "transaction insert", (0.0,) * dimension)
                self.insert(client, "mutable", [inserted])
                pending[inserted.primary] = inserted
                self.warm_views(client, pending)
                self.sql(client, "COMMIT" if commit else "ROLLBACK")
                if commit:
                    rows = pending
                self.warm_views(client, rows)
        client.shutdown()
        client = self.open_client()
        self.warm_views(client, rows)
        client.shutdown()

    def test_small_cache_budget_shared_views_and_eviction_fallback(self):
        client = self.open_client(cache_mb=1)
        dimension = 672
        vectors = [(float(rank),) + (0.0,) * (dimension - 2) + (0.5,)
                   for rank in (-95, -32, -1, 0, 1, 32, 95, 127)]
        rows = repeated_rows(vectors, count=260)
        # Even ignoring row/directory overhead, raw vectors nearly fill this
        # budget. Small filtered views retain the float scan; the full view
        # must share the remaining allowance with every existing directory.
        self.assertLess(len(rows) * dimension * 4, 1024 * 1024)
        self.assertGreater((len(rows) * 5 + 260) * dimension, 1024 * 1024)
        self.create(client, "budget", dimension, rows)
        probe = (0.0,) * dimension
        for _ in range(2):
            self.check_full_and_bucket(client, "budget", rows, probe)
            self.check(client, "budget", rows, probe, where="category='red'",
                       predicate=lambda row: row.category == "red")
        # An empty second table halves the per-table allowance. Raw vectors
        # alone now exceed it, so subsequent reads must also survive eviction.
        self.assertGreater(len(rows) * dimension * 4, 512 * 1024)
        self.sql(client, "CREATE TABLE pressure(id BIGINT PRIMARY KEY)")
        self.check_full_and_bucket(client, "budget", rows, probe)
        inserted = Row(-10001, 7, "red", "after eviction", probe)
        self.insert(client, "budget", [inserted])
        rows.append(inserted)
        self.check_full_and_bucket(client, "budget", rows, probe, limit=1, offset=0)
        client.shutdown()
        client = self.open_client(cache_mb=1)
        self.check_full_and_bucket(client, "budget", rows, probe, limit=5, offset=0)
        client.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args, remaining = parser.parse_known_args()
    QuantizedScreenTests.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *remaining])
