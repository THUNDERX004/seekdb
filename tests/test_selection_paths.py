#!/usr/bin/env python3
"""Bounded selection and deferred projection regression tests.

Run: python tests/test_selection_paths.py build/minivectordb.exe
The same deterministic fixture runs sequentially with a 64 MiB row cache and
a 1 MiB cache that its VARCHAR payload alone exceeds by more than twofold.
Only the existing timed protocol client is shared. Expected rows, transaction
snapshots, sorting and pagination are generated independently in Python.
All vector components/probes are small integers, exactly representable as
float32; integer squared distances give exact ordering, including true ties.
Distance text must match the protocol's six decimal places exactly.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import tempfile
import time
from dataclasses import dataclass, replace
from pathlib import Path
from unittest.mock import patch

from test_differential import TimedSession


DIM = 128
MINIMUM, MAXIMUM = -(1 << 63), (1 << 63) - 1
COLUMNS = ("id", "score", "cat", "body", "emb")
PROBES = {
    "d": (0,) * DIM,
    "e": (1, -2) + (0,) * (DIM - 3) + (3,),
}
ORDERS = (
    (("score", True), ("cat", False), ("id", True)),
    (("cat", True), ("score", False)),
    (("body", False), ("score", True)),
    (("d", False),),
    (("d", True), ("score", True)),
    (("d", False), ("score", True), ("cat", False)),
    (("score", True), ("d", False), ("id", True)),
    (("d", False), ("e", True), ("score", False)),
)


@dataclass(frozen=True)
class Row:
    id: int
    score: int
    cat: str
    body: str
    emb: tuple[int, ...]


def literal(value):
    if isinstance(value, tuple):
        value = "[" + ",".join(map(str, value)) + "]"
    if isinstance(value, str):
        return "'" + value.replace(chr(92), chr(92) * 2).replace("'", "''") + "'"
    return str(value)


def body_for(key, phase="base", size=8192):
    prefix = f"row={key:+021d}|{phase}|'quoted'\\payload|"
    suffix = f"|end={key}:{phase}"
    fill = chr(ord("a") + key % 26)
    return prefix + fill * (size - len(prefix) - len(suffix)) + suffix


def fixture():
    rows = []
    for index, key in enumerate((MINIMUM, MAXIMUM, *range(-128, 128))):
        emb = [0] * DIM
        x, y = ((3, 4), (-3, 4), (4, -3), (-4, -3))[index % 4]
        radius = index % 16 // 4 + 1
        emb[0], emb[1] = x * radius, y * radius
        emb[2 + index % 124] = index // 124 + 1
        emb[-1] = radius
        rows.append(Row(key, index * 17 % 11 - 5,
                        ("alpha", "beta", "gamma", "delta")[index % 4],
                        body_for(key), tuple(emb)))
    payload = sum(len(row.body.encode("utf-8")) for row in rows)
    if not 2 * 1024**2 < payload < 8 * 1024**2:
        raise AssertionError("fixture must exceed 1 MiB cache and fit 64 MiB cache")
    return rows


def squared_distance(row, probe):
    # Integer subtraction and summation avoid rounded output as an ordering oracle.
    return sum((value - query) ** 2 for value, query in zip(row.emb, probe))


def distance_sql(name):
    return f"L2_DISTANCE(emb,{literal(PROBES[name])})"


class Harness:
    def __init__(self, executable, data_dir, cache_mb):
        self.executable, self.data_dir = executable, data_dir
        self.cache_mb = cache_mb
        self.rows = {}
        self.session = None
        self.statements = self.queries = self.restarts = 0
        self.phase = "empty"
        self.start()

    def start(self):
        # The inherited client intentionally has no environment argument. Patch
        # only while spawning; each child receives its own explicit cache budget.
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(self.cache_mb)}):
            self.session = TimedSession(self.executable, self.data_dir, 8)

    def close(self):
        if self.session is not None:
            session, self.session = self.session, None
            session.shutdown()

    def restart(self, crash=False):
        session, self.session = self.session, None
        session.shutdown(crash=crash)
        self.restarts += 1
        self.start()

    def execute(self, sql):
        self.statements += 1
        output = self.session.execute(sql)
        if any(line.startswith("ERROR ") for line in output):
            raise AssertionError(f"{self.phase}: {sql[:200]}: {output!r}")
        return output

    def expect(self, sql, expected):
        actual = self.execute(sql)
        if actual != expected:
            raise AssertionError(f"{self.phase}: {sql[:160]}: {actual!r} != {expected!r}")

    def insert(self, rows):
        values = ",".join(
            "(" + ",".join(literal(getattr(row, name)) for name in COLUMNS) + ")"
            for row in rows
        )
        self.expect("INSERT INTO items VALUES " + values, [f"OK {len(rows)}"])
        self.rows.update((row.id, row) for row in rows)

    def update(self, key, **changes):
        assignments = ",".join(f"{name}={literal(value)}" for name, value in changes.items())
        self.expect(f"UPDATE items SET {assignments} WHERE id={key}", ["OK 1"])
        self.rows[key] = replace(self.rows[key], **changes)

    def delete(self, key):
        self.expect(f"DELETE FROM items WHERE id={key}", ["OK 1"])
        del self.rows[key]

    def query(self, order, columns=("id",), limit=7, offset=0,
              condition="", predicate=lambda row: True, comma=False,
              expression=False, star=False):
        columns = COLUMNS if star else columns
        needed = set(columns) | {name for name, _ in order}
        probes = {name: PROBES[name] for name in PROBES if name in needed}
        projection = "*" if star else ",".join(
            f"{distance_sql(name)} AS {name}" if name in PROBES else name
            for name in columns
        )
        sql = f"SELECT {projection} FROM items"
        if condition:
            sql += " WHERE " + condition
        sql += " ORDER BY " + ",".join(
            (distance_sql(name) if name in PROBES and
             (expression or name not in columns) else name) +
            (" DESC" if descending else " ASC")
            for name, descending in order
        )
        if limit is not None:
            sql += f" LIMIT {offset},{limit}" if comma else f" LIMIT {limit} OFFSET {offset}"

        wanted = [row for row in self.rows.values() if predicate(row)]
        distances = {row.id: {name: squared_distance(row, probe)
                              for name, probe in probes.items()} for row in wanted}

        def sort_value(row, name):
            value = distances[row.id][name] if name in probes else getattr(row, name)
            return value.encode("utf-8") if isinstance(value, str) else value

        # Stable sorts implement every SQL key, then ascending PK for exact ties.
        wanted.sort(key=lambda row: row.id)
        for name, descending in reversed(order):
            wanted.sort(key=lambda row: sort_value(row, name), reverse=descending)
        wanted = wanted[offset:] if limit is None else wanted[offset:offset + limit]
        actual = self.execute(sql)
        self.queries += 1
        if len(actual) != len(wanted):
            raise AssertionError(f"{self.phase}: {sql}: rows {len(actual)} != {len(wanted)}")
        for index, (line, row) in enumerate(zip(actual, wanted)):
            fields = line.split("\t")
            if len(fields) != len(columns):
                raise AssertionError(f"{self.phase}: {sql}: row {index} has wrong column count")
            for name, field in zip(columns, fields):
                if name == "emb":
                    decoded = json.loads(field)
                    good = (isinstance(decoded, list) and len(decoded) == DIM and
                            all(type(value) in (int, float) for value in decoded) and
                            tuple(decoded) == row.emb)
                    expected = str(row.emb)
                else:
                    expected = (f"{math.sqrt(distances[row.id][name]):.6f}"
                                if name in probes else str(getattr(row, name)))
                    good = field == expected
                if not good:
                    raise AssertionError(
                        f"cache={self.cache_mb} phase={self.phase}: {sql}\n"
                        f"row={index} expected PK={row.id} column={name}: "
                        f"actual={field[:140]!r} (len={len(field)}) != "
                        f"expected={expected[:140]!r} (len={len(expected)})"
                    )

    def snapshot(self, label):
        self.phase = label
        # Audit every stored field, not only TopK IDs, before and after mutations.
        self.query((("id", False),), limit=None, star=True)
        for order in (ORDERS[0], ORDERS[3], ORDERS[5], ORDERS[6], ORDERS[7]):
            distances = tuple(name for name in PROBES if any(key == name for key, _ in order))
            columns = ("body", "id", "emb", "score", "cat") + distances
            self.query(order, columns, limit=11, offset=3)
            # Repeated equality queries also cover cached indexed candidates and
            # rows entering/leaving that filter through the transaction overlay.
            self.query(order, columns, limit=9, offset=1,
                       condition="cat='alpha' AND score>=-1000",
                       predicate=lambda row: row.cat == "alpha" and row.score >= -1000)
        self.query(ORDERS[3], ("body", "id", "emb", "body", "d"), limit=5, offset=2)
        self.query(ORDERS[3], ("body", "emb", "id"), limit=5, offset=2)
        self.query(ORDERS[0], ("id", "d", "body", "emb"), limit=5, offset=2)
        self.query(ORDERS[3], ("id", "body", "emb", "d"), limit=1,
                   condition="id=9223372036854775807", predicate=lambda row: row.id == MAXIMUM)
        self.query(ORDERS[0], ("id", "body", "emb"), condition="cat='missing'",
                   predicate=lambda row: False)


def matrix(h):
    count = len(h.rows)
    windows = (
        (None, 0), (0, 0), (1, 0), (7, 3),
        (7, count - 1), (3, count), (3, count + 1),
        (MAXIMUM, 1), (1, MAXIMUM), (MAXIMUM, MAXIMUM),
        (0, MAXIMUM), (MAXIMUM - 4, 9),
    )
    for order in ORDERS:
        distances = tuple(name for name in PROBES if any(key == name for key, _ in order))
        for index, (limit, offset) in enumerate(windows):
            h.query(order, ("id", "score", "cat") + distances, limit, offset,
                    comma=bool(index % 2), expression=bool(index % 3))
        h.query(order, ("id", "body", "emb") + distances, limit=13, offset=7)
    # True equal distances must cross heap and page boundaries in PK order,
    # including signed BIGINT extremes and explicit descending PK override.
    group = squared_distance(next(iter(h.rows.values())), PROBES["d"])
    tied = [row for row in h.rows.values() if squared_distance(row, PROBES["d"]) == group]
    if len(tied) < 4:
        raise AssertionError("fixture lost its true-distance ties")
    for order in ((("d", False),), (("d", True),),
                  (("d", False), ("id", True))):
        h.query(order, ("id", "body", "emb", "d"), limit=3, offset=1)
    for order in (ORDERS[0], ORDERS[3]):
        for limit, offset in ((1, 0), (1, 1), (0, 0), (MAXIMUM, 0)):
            h.query(order, ("id", "body", "emb"), limit, offset,
                    condition=f"id={MAXIMUM}", predicate=lambda row: row.id == MAXIMUM)


def mutate_overlay(h, phase):
    h.phase = phase
    # Both directions across an already queried equality predicate.
    h.update(MINIMUM, score=-999, cat="beta", emb=PROBES["d"],
             body=body_for(MINIMUM, phase, 12288))
    h.update(MAXIMUM, score=999, cat="alpha", emb=PROBES["d"],
             body=body_for(MAXIMUM, phase, 16384))
    h.delete(-128)
    # Delete/reinsert a base PK and update an inserted row in the same overlay.
    h.delete(-127)
    h.insert([Row(-127, 999, "alpha", body_for(-127, phase), PROBES["d"]),
              Row(10001, 999, "alpha", body_for(10001, phase), PROBES["d"]),
              Row(10002, -999, "alpha", body_for(10002, phase), PROBES["e"])])
    h.update(10001, score=1000, emb=PROBES["e"], body=body_for(10001, phase + "-updated"))
    h.delete(10002)


def run(executable, cache_mb):
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix=f"mvd-selection-{cache_mb}mb-") as temporary:
        h = Harness(executable, Path(temporary), cache_mb)
        try:
            h.expect("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT,"
                     "cat VARCHAR(16),body VARCHAR(16384),emb VECTOR(128))", ["OK"])
            for order in ORDERS:
                h.query(order, ("id", "body", "emb"), limit=0)
                h.query(order, ("id", "body", "emb"), limit=3)
            rows = fixture()
            h.insert(rows[:1])
            h.snapshot("one row")
            for start in range(1, len(rows), 32):
                h.insert(rows[start:start + 32])
            h.phase = "base matrix"
            matrix(h)
            h.snapshot("base cached/disk projection")
            committed = h.rows.copy()
            h.expect("BEGIN", ["OK"])
            mutate_overlay(h, "rollback overlay")
            h.snapshot("uncommitted before rollback")
            h.expect("ROLLBACK", ["OK"])
            h.rows = committed.copy()
            h.snapshot("after rollback")
            h.expect("BEGIN", ["OK"])
            mutate_overlay(h, "commit overlay")
            h.snapshot("uncommitted before commit")
            h.expect("COMMIT", ["OK"])
            committed = h.rows.copy()
            h.snapshot("after commit")
            h.restart()
            h.snapshot("committed after clean restart")
            # Restart with a dirty overlay must recover the committed snapshot.
            h.expect("BEGIN", ["OK"])
            h.update(MAXIMUM, cat="beta", body=body_for(MAXIMUM, "discard"), emb=PROBES["e"])
            h.delete(10001)
            h.insert([Row(20001, 2000, "alpha", body_for(20001, "discard"), PROBES["d"])])
            h.snapshot("dirty overlay before forced restart")
            h.restart(crash=True)
            h.rows = committed.copy()
            h.snapshot("committed after forced restart")
        finally:
            h.close()
    elapsed = time.monotonic() - started
    print(f"PASS selection cache={cache_mb}MiB: {h.queries} exact queries, "
          f"{h.statements} SQL statements, {h.restarts} restarts, {elapsed:.3f}s", flush=True)
    return h.queries, h.statements


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"executable does not exist: {executable}")
    started = time.monotonic()
    counts = [run(executable, budget) for budget in (64, 1)]
    print(f"PASS selection paths: {sum(item[0] for item in counts)} exact queries, "
          f"{sum(item[1] for item in counts)} SQL statements, "
          f"{time.monotonic() - started:.3f}s total", flush=True)


if __name__ == "__main__":
    main()
