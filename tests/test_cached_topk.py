#!/usr/bin/env python3
"""Bounded JSON TopK correctness checks; run with an already-built executable."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
from pathlib import Path
import sys
import tempfile
import unittest

from jsonl_client import JsonlClient
from test_exact_distance import exact_squared, from_bits, vector_sql


def quoted(text):
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


@dataclass(frozen=True)
class Row:
    primary: int
    tag: int
    category: str
    payload: str
    vector: tuple
    distance: int = 0


def integer_row(primary, tag, category, xy, distance, payload=None):
    if sum(value * value for value in xy) != distance * distance:
        raise AssertionError("fixture distance must be an exact integer")
    return Row(primary, tag, category, payload or f"row {primary}",
               tuple(xy) + (0,) * 126, distance)


class CachedTopKTests(unittest.TestCase):
    executable = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("supply an existing database executable")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-cached-topk-")
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)

    def open_client(self):
        client = JsonlClient([self.executable], self.data_dir, timeout=5)
        self.addCleanup(client.close)
        return client

    def sql(self, client, statement, affected=None):
        response = client.execute(statement)
        self.assertIs(response.get("ok"), True, response)
        self.assertNotIn("error", response)
        if affected is not None:
            self.assertIs(type(response.get("affected_rows")), int, response)
            self.assertEqual(response["affected_rows"], affected, statement)
        return response

    def insert(self, client, table, rows):
        values = ",".join(
            f"({row.primary},{row.tag},{quoted(row.category)},"
            f"{quoted(row.payload)},{quoted(vector_sql(row.vector))})"
            for row in rows
        )
        self.sql(client, f"INSERT INTO {table} VALUES {values}", len(rows))

    def create(self, client, table, dimension, rows):
        self.sql(client, f"CREATE TABLE {table}(id BIGINT PRIMARY KEY,tag BIGINT,"
                 f"category VARCHAR(16),payload VARCHAR(64),v VECTOR({dimension}))")
        self.insert(client, table, rows)

    def query(self, client, table, probe, rows, scores, where="", predicate=None,
              limit=3, offset=0, include_vector=True):
        candidates = [row for row in rows if predicate is None or predicate(row)]
        expected = sorted(candidates, key=lambda row: (scores[row.primary], row.primary))
        expected = expected[offset:offset + limit]
        fields = "id,tag,category,payload," + ("v," if include_vector else "")
        statement = (f"SELECT {fields}L2_DISTANCE(v,{quoted(vector_sql(probe))}) AS d "
                     f"FROM {table} " + (f"WHERE {where} " if where else "")
                     + f"ORDER BY d LIMIT {limit} OFFSET {offset}")
        response = self.sql(client, statement)
        diagnostic = (f"{table}, dimension={len(probe)}, filter={where!r}, "
                      f"limit={limit}, offset={offset}")
        columns = ["id", "tag", "category", "payload"]
        if include_vector:
            columns.append("v")
        self.assertEqual(response.get("columns"), columns + ["d"], diagnostic)
        actual_rows = response.get("rows")
        self.assertIs(type(actual_rows), list, response)
        self.assertEqual(len(actual_rows), len(expected), diagnostic)
        self.assertEqual([actual[0] for actual in actual_rows],
                         [row.primary for row in expected], diagnostic)
        for actual, row in zip(actual_rows, expected):
            self.assertIs(type(actual), list)
            self.assertEqual(len(actual), len(columns) + 1, diagnostic)
            self.assertIs(type(actual[0]), int)
            self.assertIs(type(actual[1]), int)
            self.assertIs(type(actual[2]), str)
            self.assertIs(type(actual[3]), str)
            self.assertEqual(actual[:4],
                             [row.primary, row.tag, row.category, row.payload], diagnostic)
            if include_vector:
                self.assertIs(type(actual[4]), list)
                self.assertEqual(len(actual[4]), len(row.vector))
                for component, wanted in zip(actual[4], row.vector):
                    self.assertIn(type(component), (int, float))
                    self.assertEqual(component, wanted, diagnostic)
            distance = actual[-1]
            self.assertIn(type(distance), (int, float))
            self.assertTrue(math.isfinite(distance), diagnostic)
            if include_vector:
                self.assertEqual(distance, row.distance, diagnostic)
            else:
                # exact_squared uses the independent 2^-149 integer lattice.
                wanted = math.sqrt(math.ldexp(float(scores[row.primary]), -298))
                # Public distance tolerance permits rounded tiny display values;
                # the complete ID sequence above still needs exact lattice order.
                self.assertTrue(math.isclose(distance, wanted, rel_tol=1e-5, abs_tol=1e-5),
                                f"{diagnostic}: {distance!r} != {wanted!r}")

    def hot_queries(self, client, rows):
        scores = {row.primary: row.distance * row.distance for row in rows.values()}
        for where, predicate in (
            ("", None),
            ("tag=7", lambda row: row.tag == 7),
            ("category='red'", lambda row: row.category == "red"),
            ("tag=8", lambda row: row.tag == 8),
            ("category='blue'", lambda row: row.category == "blue"),
            ("", None),
            ("tag=7", lambda row: row.tag == 7),
        ):
            self.query(client, "t", (0,) * 128, rows.values(), scores,
                       where, predicate)

    def test_mutations_transactions_and_restart_preserve_all_fields(self):
        client = self.open_client()
        fixture = [
            integer_row(51, 7, "red", (5, 12), 13),
            integer_row(21, 8, "blue", (6, 8), 10),
            integer_row(61, 8, "blue", (8, 15), 17),
            integer_row(31, 7, "blue", (0, 5), 5),
            integer_row(11, 8, "red", (3, 4), 5, "quote'; slash\\; 中文"),
            integer_row(41, 7, "red", (0, 0), 0),
        ]
        rows = {row.primary: row for row in fixture}
        self.create(client, "t", 128, fixture)
        self.hot_queries(client, rows)
        self.sql(client, "CREATE VECTOR INDEX ix ON t(v)")
        self.hot_queries(client, rows)
        scores = {row.primary: row.distance ** 2 for row in rows.values()}
        for where, predicate in (
            ("tag=999", lambda row: False),
            ("category='absent'", lambda row: False),
            ("tag=7 AND category='red'", lambda row: row.tag == 7 and row.category == "red"),
            ("id>=20 AND id<60", lambda row: 20 <= row.primary < 60),
            ("id=31", lambda row: row.primary == 31),
        ):
            self.query(client, "t", (0,) * 128, rows.values(), scores, where, predicate)
        self.query(client, "t", (0,) * 128, rows.values(), scores, limit=2, offset=1)

        changed = integer_row(41, 8, "blue", (9, 12), 15, "updated; 41")
        self.sql(client, f"UPDATE t SET v={quoted(vector_sql(changed.vector))},"
                 f"tag=8,category='blue',payload={quoted(changed.payload)} WHERE id=41", 1)
        rows[41] = changed
        self.hot_queries(client, rows)
        inserted = [integer_row(1, 7, "red", (0, 0), 0),
                    integer_row(71, 8, "blue", (7, 24), 25),
                    integer_row(81, 7, "red", (20, 21), 29)]
        self.insert(client, "t", inserted)
        rows.update((row.primary, row) for row in inserted)
        self.hot_queries(client, rows)
        self.sql(client, "DELETE FROM t WHERE id=11", 1)
        del rows[11]
        self.hot_queries(client, rows)

        # Exercise overlay reads, then query warmed committed data after rollback.
        self.sql(client, "BEGIN")
        pending = rows.copy()
        changed = integer_row(31, 8, "blue", (0, 0), 0, "rolled back")
        self.sql(client, f"UPDATE t SET v={quoted(vector_sql(changed.vector))},"
                 "tag=8,category='blue',payload='rolled back' WHERE id=31", 1)
        pending[31] = changed
        self.sql(client, "DELETE FROM t WHERE id=21", 1)
        del pending[21]
        inserted = integer_row(2, 7, "red", (3, 4), 5)
        self.insert(client, "t", [inserted])
        pending[2] = inserted
        self.hot_queries(client, pending)
        self.sql(client, "ROLLBACK")
        self.hot_queries(client, rows)

        self.sql(client, "BEGIN")
        changed = integer_row(51, 8, "blue", (0, 0), 0, "committed")
        self.sql(client, f"UPDATE t SET v={quoted(vector_sql(changed.vector))},"
                 "tag=8,category='blue',payload='committed' WHERE id=51", 1)
        rows[51] = changed
        self.sql(client, "DELETE FROM t WHERE id=61", 1)
        del rows[61]
        inserted = integer_row(3, 7, "red", (0, 5), 5)
        self.insert(client, "t", [inserted])
        rows[3] = inserted
        self.hot_queries(client, rows)
        self.sql(client, "COMMIT")
        self.hot_queries(client, rows)
        client.shutdown()
        client = self.open_client()
        self.hot_queries(client, rows)
        client.shutdown()

    def test_exact_float32_order_at_simd_tail_and_high_dimension(self):
        client = self.open_client()
        largest, tiny = from_bits(0x7F7FFFFF), from_bits(1)
        for dimension in (128, 129, 4096):
            with self.subTest(dimension=dimension):
                table = f"extreme_{dimension}"
                fixture = [
                    Row(primary, tag, category, f"rank {rank}",
                        (largest,) * (dimension - 1) + (rank * tiny,))
                    for primary, rank, tag, category in (
                        (70, 7, 7, "red"), (10, 5, 8, "red"), (50, 1, 7, "blue"),
                        (20, 3, 8, "blue"), (40, -1, 7, "red"), (30, -3, 8, "blue"),
                    )
                ]
                self.create(client, table, dimension, fixture)
                self.sql(client, f"CREATE VECTOR INDEX ix_{dimension} ON {table}(v)")
                for common in (-largest, largest):
                    # Opposite maxima overflow float32 subtraction; equal maxima
                    # expose subnormal tails whose squares underflow float32.
                    probe = (common,) * (dimension - 1) + (tiny,)
                    scores = {row.primary: exact_squared(row.vector, probe) for row in fixture}
                    ordered = sorted(fixture, key=lambda row: (scores[row.primary], row.primary))
                    self.assertEqual([row.primary for row in ordered], [50, 20, 40, 10, 30, 70])
                    self.assertEqual(len(set(scores.values())), 4)
                    if common < 0:
                        self.assertEqual(len({float(score) for score in scores.values()}), 1,
                                         "fixture must defeat double-score ordering")
                    for where, predicate in (
                        ("", None),
                        ("tag=7", lambda row: row.tag == 7),
                        ("category='red'", lambda row: row.category == "red"),
                    ):
                        self.query(client, table, probe, fixture, scores, where, predicate,
                                   include_vector=False)
        client.shutdown()


    def test_near_unit_ulp_order_with_small_limits_and_filter(self):
        client = self.open_client()
        for dimension in (128, 129, 16385):
            with self.subTest(dimension=dimension):
                table = f"near_unit_{dimension}"
                fixture = []
                for primary, rank, tag, category in (
                    (70, 6, 7, "red"), (10, 4, 8, "red"), (50, 1, 7, "blue"),
                    (20, 2, 8, "blue"), (40, 3, 7, "red"), (30, 5, 8, "blue"),
                ):
                    vector = [1.0] * dimension
                    # Balanced displacements cancel the linear terms in squared
                    # distance: only tiny quadratic ULP differences choose TopK.
                    vector[0] = from_bits(0x3F800000 + rank)
                    vector[-1] = from_bits(0x3F800000 - 2 * rank)
                    fixture.append(Row(primary, tag, category, f"ulp {rank}", tuple(vector)))
                probe = (0.0,) * dimension
                scores = {row.primary: exact_squared(row.vector, probe) for row in fixture}
                ordered = sorted(fixture, key=lambda row: (scores[row.primary], row.primary))
                self.assertEqual([row.primary for row in ordered], [50, 20, 40, 10, 30, 70])
                self.assertEqual(len(set(scores.values())), len(fixture))
                if dimension == 16385:
                    self.assertEqual(len({float(score) for score in scores.values()}), 1,
                                     "high-dimensional fixture must defeat double-score ordering")
                self.create(client, table, dimension, fixture)
                self.sql(client, f"CREATE VECTOR INDEX ix_{dimension} ON {table}(v)")
                for limit, where, predicate in (
                    (1, "", None),
                    (3, "", None),
                    (3, "tag=7", lambda row: row.tag == 7),
                ):
                    self.query(client, table, probe, fixture, scores, where, predicate,
                               limit=limit, include_vector=False)
        client.shutdown()



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args, remaining = parser.parse_known_args()
    CachedTopKTests.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *remaining])