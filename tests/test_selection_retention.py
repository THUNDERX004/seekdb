#!/usr/bin/env python3
"""Sparse-projection retention checks using disposable, out-of-cache SQL rows.

Run: python -X utf8 -B tests/test_selection_retention.py build/minivectordb.exe

Unlike the broad selection-path matrix, these cases focus on unprojected sort
columns, projected vectors unused by ordering, and complete SELECT * payloads.
The VARCHAR payload alone exceeds twice the explicitly configured 1 MiB cache.
Integer vectors give an independent exact distance-ordering oracle; scalar and
string fields are checked exactly, distance text with the public SQL tolerance.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from unittest.mock import patch

from test_differential import TimedSession


DIM, ROW_COUNT = 128, 2048
COLUMNS = ("payload", "score", "emb", "id", "tag")


@dataclass(frozen=True)
class Row:
    payload: str
    score: int
    emb: tuple[int, ...]
    id: int
    tag: str


def literal(value):
    if isinstance(value, tuple):
        value = "[" + ",".join(map(str, value)) + "]"
    if isinstance(value, str):
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"
    return str(value)


DISTANCE = f"L2_DISTANCE(emb,{literal((0,) * DIM)})"


def fixture():
    rows = [Row("x" * 1024 + f"|sort={key * 37 % ROW_COUNT:04d}|id={key}",
                key * 17 % 13 - 6,
                tuple((key * (axis + 3) + axis * axis) % 19 - 9
                      for axis in range(DIM)), key, f"t{key % 5}")
            for key in range(-ROW_COUNT // 2, ROW_COUNT // 2)]
    if sum(len(row.payload.encode("utf-8")) for row in rows) <= 2 * 1024**2:
        raise AssertionError("fixture must exceed the 1 MiB row-cache budget")
    return rows


class Checks:
    def __init__(self, session):
        self.session, self.rows, self.queries = session, {}, 0
        self.phase = "base"

    def expect(self, sql, expected):
        actual = self.session.execute(sql)
        if actual != expected:
            raise AssertionError(f"{self.phase}: {sql[:160]}: {actual!r} != {expected!r}")

    def insert(self, rows):
        values = ",".join("(" + ",".join(literal(getattr(row, name))
                                           for name in COLUMNS) + ")" for row in rows)
        self.expect("INSERT INTO items VALUES " + values, [f"OK {len(rows)}"])
        self.rows.update((row.id, row) for row in rows)

    @staticmethod
    def value(row, name):
        return sum(x * x for x in row.emb) if name == "d" else getattr(row, name)

    def query(self, projection, columns, order_sql, order, limit=7, offset=1):
        sql = f"SELECT {projection} FROM items ORDER BY {order_sql}"
        if limit is not None:
            sql += f" LIMIT {limit} OFFSET {offset}"
        elif offset:
            raise ValueError("unlimited cases do not use OFFSET")
        wanted = sorted(self.rows.values(), key=lambda row: row.id)
        for name, descending in reversed(order):
            wanted.sort(key=lambda row: self.value(row, name), reverse=descending)
        wanted = wanted[offset:] if limit is None else wanted[offset:offset + limit]
        actual = self.session.execute(sql)
        self.queries += 1
        if len(actual) != len(wanted):
            raise AssertionError(f"{self.phase}: {sql}: {len(actual)} rows != {len(wanted)}; "
                                 f"first output={actual[:1]!r}")
        for line, row in zip(actual, wanted):
            fields = line.split("\t")
            if len(fields) != len(columns):
                raise AssertionError(f"{self.phase}: {sql}: wrong width at PK={row.id}")
            for name, field in zip(columns, fields):
                expected = self.value(row, name)
                if name == "emb":
                    matches = json.loads(field) == list(expected)
                elif name == "d":
                    matches = math.isclose(float(field), math.sqrt(expected),
                                           rel_tol=1e-5, abs_tol=1e-5)
                else:
                    matches = field == str(expected)
                if not matches:
                    raise AssertionError(f"{self.phase}: {sql}: PK={row.id} column={name}: "
                                         f"{field[:100]!r} != {str(expected)[:100]!r}")

    def suite(self):
        # Long VARCHAR and score are needed for comparison but absent in output.
        self.query("id,tag", ("id", "tag"), "payload DESC,score ASC",
                   (("payload", True), ("score", False)))
        # No projected PK: unique payloads also expose lost PK tie-breaking.
        self.query("payload", ("payload",), "score DESC,tag ASC",
                   (("score", True), ("tag", False)))
        self.query("score AS ranking,id AS pk", ("score", "id"),
                   "ranking DESC,payload ASC", (("score", True), ("payload", False)))
        # An unused-for-ordering vector must survive when projected or starred.
        self.query("emb,id", ("emb", "id"), "score DESC,tag ASC",
                   (("score", True), ("tag", False)))
        self.query("*", COLUMNS, "score DESC,tag ASC,id DESC",
                   (("score", True), ("tag", False), ("id", True)), limit=5)
        self.query("tag,id", ("tag", "id"), "score DESC,payload ASC",
                   (("score", True), ("payload", False)), limit=None, offset=0)
        # Hidden distances require vector data in both heap and general sorting.
        self.query("id,tag", ("id", "tag"), DISTANCE, (("d", False),))
        self.query("id,payload", ("id", "payload"), DISTANCE + ",score DESC",
                   (("d", False), ("score", True)))
        # Projection alone can require the vector, even with ordinary ordering.
        self.query(f"id,{DISTANCE} AS d", ("id", "d"), "score DESC,payload ASC",
                   (("score", True), ("payload", False)))
        self.query(f"tag,id,{DISTANCE} AS d", ("tag", "id", "d"),
                   "d DESC,score ASC,payload ASC",
                   (("d", True), ("score", False), ("payload", False)))
        self.query("*", COLUMNS, DISTANCE, (("d", False),), limit=5)


def change_overlay(h):
    # Two base replacements and two inserted rows compete with ephemeral disk
    # rows. Zero/far vectors ensure both ascending and descending distance pages
    # contain overlay rows after OFFSET; all four lead ordinary score ordering.
    for key, component in ((-1000, 0), (-999, 50), (3001, 0), (3002, 50)):
        row = Row("z" * 1024 + f"|overlay={key}", 10000,
                  (component,) * DIM, key, "overlay")
        if key in h.rows:
            assignments = ",".join(f"{name}={literal(getattr(row, name))}"
                                   for name in COLUMNS if name != "id")
            h.expect(f"UPDATE items SET {assignments} WHERE id={key}", ["OK 1"])
            h.rows[key] = row
        else:
            h.insert([row])
    h.expect("DELETE FROM items WHERE id=-1020", ["OK 1"])
    del h.rows[-1020]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="mvd-selection-retention-") as directory:
        # Only the spawn sees the environment patch; the child keeps its budget.
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": "1"}):
            session = TimedSession(executable, Path(directory), 8)
        try:
            h = Checks(session)
            h.expect("CREATE TABLE items(payload VARCHAR(2048),score BIGINT,"
                     "emb VECTOR(128),id BIGINT PRIMARY KEY,tag VARCHAR(8))", ["OK"])
            rows = fixture()
            for start in range(0, len(rows), 64):
                h.insert(rows[start:start + 64])
            h.suite()
            committed = h.rows.copy()
            h.expect("BEGIN", ["OK"])
            change_overlay(h)
            h.phase = "disk plus uncommitted overlay"
            h.suite()
            h.expect("ROLLBACK", ["OK"])
            h.rows, h.phase = committed, "after rollback"
            h.query("*", COLUMNS, "score DESC,tag ASC",
                    (("score", True), ("tag", False)))
            h.expect("BEGIN", ["OK"])
            change_overlay(h)
            h.expect("COMMIT", ["OK"])
            h.phase = "committed replacements on disk"
            h.suite()
        finally:
            session.shutdown()
    print(f"PASS selection retention: {ROW_COUNT} base rows, {DIM} dimensions, "
          f"{h.queries} exact-field queries, cache=1 MiB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    run(args.executable.resolve(strict=True))


if __name__ == "__main__":
    main()
