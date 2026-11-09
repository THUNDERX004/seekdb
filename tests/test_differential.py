#!/usr/bin/env python3
"""Independent, reproducible MiniVectorDB SQL/reference differential tests.

Run: python -X utf8 -B tests/test_differential.py ./build/minivectordb
Only the standard library and test_extended.Session are used. Database files
live in disposable system temporary directories, never in the checkout.
Distance ordering uses exact squared float32 distances, independently computed
with Python arbitrary-precision integers and float.as_integer_ratio(). Output
distance checks still use math.fsum/sqrt and the original formatting tolerance;
sorting never uses rounded double distances or six-decimal response text.
"""

from __future__ import annotations

import argparse
import json
import math
import operator
import queue
import random
import struct
import sys
import tempfile
import threading
from collections import deque
from dataclasses import dataclass, replace
from fractions import Fraction
from pathlib import Path

from test_extended import Session


SEEDS = (7, 429, 20261109)
SEED_DIMENSIONS = dict(zip(SEEDS, (4, 8, 33)))
COLUMNS = ("id", "score", "tag", "body", "emb")
BIGINT_MIN, BIGINT_MAX = -(1 << 63), (1 << 63) - 1
TEXT = (
    "", "a", "A", "z", "中文", "甲乙", "向量数据库", "é",
    "é", "😀🚀", "مرحبا", " leading", "trailing ", "O'Brien",
    "semi;colon", "--not a comment", "/*literal*/", "back" + chr(92) + "slash",
)
OPS = {"=": operator.eq, "<": operator.lt, "<=": operator.le,
       ">": operator.gt, ">=": operator.ge}


def f32(value: float) -> float:
    return struct.unpack("!f", struct.pack("!f", float(value)))[0]


def vector(values) -> tuple[float, ...]:
    return tuple(f32(value) for value in values)


def distance(left, right) -> float:
    # Subtract AFTER conversion to Python double, including opposite large values.
    return math.sqrt(math.fsum((a - b) * (a - b) for a, b in zip(left, right)))


def exact_squared_distance(left, right) -> int:
    """Exact squared distance in units of 2^-298, without engine bit decoding."""
    if len(left) != len(right):
        raise ValueError("reference vector dimension mismatch")
    # Every finite float32 denominator divides 2^149. as_integer_ratio() is a
    # Python numeric operation, independent of the engine's mantissa/limb code.
    scale = 1 << 149
    total = 0
    for value, query in zip(left, right):
        numerator, denominator = f32(value).as_integer_ratio()
        query_numerator, query_denominator = f32(query).as_integer_ratio()
        difference = (numerator * (scale // denominator) -
                      query_numerator * (scale // query_denominator))
        total += difference * difference
    return total


def check_exact_distance_oracle():
    """Guard the oracle itself against the former large-probe false ties."""
    scale_squared = 1 << 298
    for sign in (1.0, -1.0):
        probe = vector((sign * 1e30,))
        points = [vector((sign * value,)) for value in (2, 1, 0, -1)]
        scores = [exact_squared_distance(point, probe) for point in points]
        exact_probe = Fraction.from_float(probe[0])
        for point, score in zip(points, scores):
            rational_truth = (Fraction.from_float(point[0]) - exact_probe) ** 2
            if Fraction(score, scale_squared) != rational_truth:
                raise AssertionError("integer oracle disagrees with 1D Fraction truth")
        if not all(a < b for a, b in zip(scores, scores[1:])):
            raise AssertionError("large-probe exact distances must be strictly ordered")
        # Original numeric failures were oracle failures: fsum cannot recover
        # the small operand already lost by double subtraction from 1e30.
        if len({distance(point, probe) for point in points}) != 1:
            raise AssertionError("large-probe fixture no longer exercises a double tie")
    for left, right in (
        (vector((0.0, -0.0)), vector((-0.0, 0.0))),
        (vector((2**-149, -2**-149)), vector((0.0, 2**-148))),
        (vector((3e38, -3e38, 1e-40)), vector((-3e38, 3e38, 0.0))),
    ):
        rational_truth = sum(
            (Fraction.from_float(a) - Fraction.from_float(b)) ** 2
            for a, b in zip(left, right)
        )
        if Fraction(exact_squared_distance(left, right), scale_squared) != rational_truth:
            raise AssertionError("integer oracle disagrees with Fraction boundary truth")


def quote(text: str) -> str:
    return "'" + text.replace(chr(92), chr(92) * 2).replace("'", "''") + "'"


def literal(value) -> str:
    if isinstance(value, tuple):
        return quote("[" + ",".join(format(x, ".17g") for x in value) + "]")
    return quote(value) if isinstance(value, str) else str(value)


def where(conditions) -> str:
    return (" WHERE " + " AND ".join(
        f"{column} {op} {literal(value)}" for column, op, value in conditions
    )) if conditions else ""


@dataclass(frozen=True)
class Row:
    id: int
    score: int
    tag: str
    body: str
    emb: tuple[float, ...]


def matches(row: Row, conditions) -> bool:
    return all(OPS[op](getattr(row, column), value)
               for column, op, value in conditions)


class DuplicateKey(ValueError):
    pass


class Reference:
    """Immutable rows plus a full transaction snapshot; no engine SQL parsing."""

    def __init__(self):
        self.committed: dict[int, Row] = {}
        self.pending: dict[int, Row] | None = None

    @property
    def rows(self) -> dict[int, Row]:
        return self.committed if self.pending is None else self.pending

    def begin(self):
        assert self.pending is None
        self.pending = self.committed.copy()

    def commit(self):
        assert self.pending is not None
        self.committed = self.pending
        self.pending = None

    def rollback(self):
        self.pending = None

    def insert(self, rows):
        keys = [row.id for row in rows]
        if len(set(keys)) != len(keys) or any(key in self.rows for key in keys):
            raise DuplicateKey
        self.rows.update((row.id, row) for row in rows)

    def update(self, changes, conditions):
        selected = [row for row in self.rows.values() if matches(row, conditions)]
        self.rows.update((row.id, replace(row, **changes)) for row in selected)
        return len(selected)

    def delete(self, conditions):
        keys = [key for key, row in self.rows.items() if matches(row, conditions)]
        for key in keys:
            del self.rows[key]
        return len(keys)

    def select(self, columns, conditions, order, probes, limit, offset):
        selected = []
        for row in self.rows.values():
            if matches(row, conditions):
                values = {name: getattr(row, name) for name in COLUMNS}
                values.update((name, distance(row.emb, probe))
                              for name, probe in probes.items())
                sort_values = values.copy()
                sort_values.update((name, exact_squared_distance(row.emb, probe))
                                   for name, probe in probes.items())
                selected.append((values, sort_values))
        # Stable sorts implement every key and a final ascending PK tie breaker.
        selected.sort(key=lambda pair: pair[1]["id"])
        for name, descending in reversed(order):
            selected.sort(key=lambda pair: pair[1][name], reverse=descending)
        selected = selected[offset:] if limit is None else selected[offset:offset + limit]
        return [tuple(values[column] for column in columns) for values, _ in selected]


class TimedSession(Session):
    """Bound even a blocked stdin write/readline, rather than checking between reads."""

    def __init__(self, executable, data_dir, timeout):
        super().__init__(executable, data_dir)
        self.timeout = timeout
        self.requests = queue.Queue()
        self.responses = queue.Queue()
        self.worker = threading.Thread(target=self._work, daemon=True)
        self.worker.start()

    def _work(self):
        while True:
            sql = self.requests.get()
            if sql is None:
                return
            try:
                self.responses.put((True, super().execute(sql)))
            except Exception as error:
                self.responses.put((False, error))

    def execute(self, sql):
        self.requests.put(sql)
        try:
            success, result = self.responses.get(timeout=self.timeout)
        except queue.Empty:
            self.kill()
            raise AssertionError(f"real timeout after {self.timeout:g}s; SQL={sql!r}") from None
        if not success:
            raise result
        return result

    def shutdown(self, crash=False):
        try:
            if crash:
                self.kill()
            else:
                self.close()
        finally:
            self.kill()
            self.requests.put(None)
            self.worker.join(timeout=2)
            for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
                if pipe is not None:
                    pipe.close()


class Harness:
    def __init__(self, executable, seed, dim, timeout, label):
        self.executable, self.seed, self.dim = executable, seed, dim
        self.timeout, self.label = timeout, label
        self.rng = random.Random(seed)
        self.ref = Reference()
        self.history = deque(maxlen=10)
        self.count = 0
        self.query_failures = 0
        self.failure_examples = []
        self.mode = "exact"
        self.next_id = 10000
        self.temp = tempfile.TemporaryDirectory(prefix="mvd-differential-")
        self.data_dir = Path(self.temp.name)
        self.session = None

    def __enter__(self):
        try:
            self.session = TimedSession(self.executable, self.data_dir, self.timeout)
            self.expect(
                f"CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT, "
                f"tag VARCHAR(32), body VARCHAR(128), emb VECTOR({self.dim}))", ["OK"]
            )
        except Exception as error:
            self.__exit__(type(error), error, error.__traceback__)
            raise
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            if self.session is not None:
                self.session.shutdown(crash=exc_type is not None)
        finally:
            self.temp.cleanup()

    def fail(self, sql, message):
        recent = "\n".join(f"  {text[:300]}{' ...' if len(text) > 300 else ''}"
                           for text in self.history)
        raise AssertionError(
            f"case={self.label} seed={self.seed} dim={self.dim} "
            f"statement={self.count} mode={self.mode}\nSQL: {sql}\n"
            f"{message}\nReplay: --case {self.label} --seed {self.seed}\n"
            f"Recent SQL (long history entries abbreviated):\n{recent}"
        )

    def execute(self, sql):
        self.count += 1
        self.history.append(sql)
        try:
            return self.session.execute(sql)
        except Exception as error:
            self.fail(sql, f"protocol/process failure: {error}")

    def expect(self, sql, wanted):
        actual = self.execute(sql)
        if actual != wanted:
            self.fail(sql, f"actual={actual!r}\nexpected={wanted!r}")

    def control(self, command):
        self.expect(command, ["OK"])
        {"BEGIN": self.ref.begin, "COMMIT": self.ref.commit,
         "ROLLBACK": self.ref.rollback}[command]()

    def set_mode(self, mode):
        self.expect(f"SET vector_mode = {mode}", ["OK"])
        self.mode = mode

    def row(self, key=None, **changes):
        if key is None:
            self.next_id += 1
            key = self.next_id if self.next_id % 2 else -self.next_id
        row = Row(key, self.rng.choice((BIGINT_MIN, BIGINT_MAX, -2, -1, 0, 1, 2)),
                  self.rng.choice(TEXT), self.rng.choice(TEXT),
                  vector(self.rng.randint(-8, 8) / 4 for _ in range(self.dim)))
        return replace(row, **changes)

    def insert(self, rows, reordered=False):
        columns = ("emb", "body", "id", "tag", "score") if reordered else COLUMNS
        sql = "INSERT INTO items"
        if reordered:
            sql += " (" + ",".join(columns) + ")"
        sql += " VALUES " + ",".join(
            "(" + ",".join(literal(getattr(row, col)) for col in columns) + ")"
            for row in rows
        )
        snapshot = self.ref.rows.copy()
        try:
            self.ref.insert(rows)
        except DuplicateKey:
            assert self.ref.rows == snapshot, "reference INSERT was not atomic"
            actual = self.execute(sql)
            if len(actual) != 1 or not actual[0].startswith("ERROR DUPLICATE_PRIMARY_KEY:"):
                self.fail(sql, f"expected duplicate-key error; actual={actual!r}")
            # Check all prior transactional changes, not just absence of new IDs.
            self.audit()
        else:
            self.expect(sql, [f"OK {len(rows)}"])

    def update(self, changes, conditions=()):
        sql = "UPDATE items SET " + ",".join(
            f"{name} = {literal(value)}" for name, value in changes.items()
        ) + where(conditions)
        self.expect(sql, [f"OK {self.ref.update(changes, conditions)}"])

    def delete(self, conditions=()):
        sql = "DELETE FROM items" + where(conditions)
        self.expect(sql, [f"OK {self.ref.delete(conditions)}"])

    def query(self, columns=COLUMNS, conditions=(), order=(("id", False),),
              probes=None, limit=None, offset=0, expression=False, comma=False, star=False):
        probes = {name: vector(probe) for name, probe in (probes or {}).items()}

        def expr(name):
            return f"L2_DISTANCE(emb,{literal(probes[name])})"

        projection = "*" if star else ",".join(
            f"{expr(name)} AS {name}" if name in probes else name for name in columns
        )
        sql = f"SELECT {projection} FROM items" + where(conditions)
        if order:
            sql += " ORDER BY " + ",".join(
                (expr(name) if expression and name in probes else name) +
                (" DESC" if descending else " ASC") for name, descending in order
            )
        if limit is not None:
            sql += f" LIMIT {offset},{limit}" if comma else f" LIMIT {limit} OFFSET {offset}"
        elif offset:
            raise ValueError("OFFSET requires LIMIT in the supported SQL subset")
        wanted = self.ref.select(columns, conditions, order, probes, limit, offset)
        actual = self.execute(sql)
        try:
            self.check_result(sql, columns, probes, wanted, actual)
        except AssertionError as error:
            # SELECT cannot change reference state, so collect its mismatch and
            # still exercise later transaction/recovery/index regressions.
            self.query_failures += 1
            if len(self.failure_examples) < 2:
                self.failure_examples.append(str(error))

    def check_result(self, sql, columns, probes, wanted, actual):
        if len(actual) != len(wanted):
            self.fail(sql, f"row count {len(actual)} != {len(wanted)}; "
                      f"actual={actual!r}; expected={wanted!r}")
        for index, (line, expected) in enumerate(zip(actual, wanted)):
            fields = line.split("\t")
            if len(fields) != len(columns):
                self.fail(sql, f"row {index}: invalid response {line!r}")
            for name, field, value in zip(columns, fields, expected):
                if name in probes:
                    try:
                        got = float(field)
                    except ValueError:
                        self.fail(sql, f"row {index}: nonnumeric distance {field!r}")
                    # Output has six decimals; relative slack only covers double
                    # evaluation/formatting, not float32 distance accumulation.
                    good = math.isfinite(got) and math.isclose(
                        got, value, rel_tol=2e-12, abs_tol=5.1e-7)
                elif name == "emb":
                    try:
                        decoded = json.loads(field)
                        good = (isinstance(decoded, list) and len(decoded) == self.dim
                                and all(type(x) in (int, float) and math.isfinite(x)
                                        for x in decoded) and vector(decoded) == value)
                    except (ValueError, TypeError, OverflowError, struct.error):
                        good = False
                else:
                    good = field == str(value)
                if not good:
                    self.fail(sql, f"row {index} column={name}: actual={field!r}, "
                              f"expected={value!r}; expected row={expected!r}; "
                              f"actual rows={actual!r}")

    def audit(self):
        self.query(star=True)

    def restart(self, crash=False):
        self.history.append("<kill/restart>" if crash else "<close/restart>")
        try:
            self.session.shutdown(crash=crash)
            self.ref.rollback()
            self.session = TimedSession(self.executable, self.data_dir, self.timeout)
            self.mode = "exact"
        except Exception as error:
            self.fail("<restart>", str(error))
        self.audit()

    def pair(self, **kwargs):
        for mode in ("exact", "ann"):
            self.set_mode(mode)
            self.query(**kwargs)


def random_conditions(h):
    rng = h.rng
    conditions = []
    for _ in range(rng.randrange(3)):
        column = rng.choice(("id", "score", "tag", "body"))
        if h.ref.rows and rng.random() < 0.75:
            value = getattr(rng.choice(list(h.ref.rows.values())), column)
        elif column in ("tag", "body"):
            value = rng.choice(TEXT)
        else:
            value = rng.choice((BIGINT_MIN, BIGINT_MAX, -10000, -1, 0, 1, 10000))
        conditions.append((column, rng.choice(tuple(OPS)), value))
    return tuple(conditions)


def random_query(h):
    rng = h.rng
    with_distance = rng.random() < 0.8
    columns = ("id", "score", "tag", "d") if with_distance else COLUMNS
    probes = {"d": vector(rng.randint(-8, 8) / 4 for _ in range(h.dim))}
    candidates = ("id", "score", "tag", "body") + (("d",) if with_distance else ())
    keys = rng.sample(candidates, rng.randint(1, 3))
    order = tuple((key, rng.random() < 0.35) for key in keys)
    limit = rng.choice((None, 0, 1, 2, 5, 11, len(h.ref.rows) + 5))
    return dict(columns=columns, conditions=random_conditions(h), order=order,
                probes=probes if with_distance else {}, limit=limit,
                offset=rng.choice((0, 1, 3, len(h.ref.rows) + 2)) if limit is not None else 0,
                expression=rng.random() < 0.5, comma=rng.random() < 0.5)


def run_random(h, steps):
    # Dyadic small values keep random SQL/state coverage independent of the
    # deliberately adversarial precision regressions in run_numeric.
    h.insert([h.row(key) for key in (BIGINT_MIN, BIGINT_MAX, *range(-12, 13))], True)
    h.audit()
    h.pair(**random_query(h))
    for step in range(steps):
        if step == steps // 3:
            if h.ref.pending is not None:
                h.control("COMMIT")
            h.expect("CREATE VECTOR INDEX ix ON items(emb)", ["OK"])
        if step % 41 == 0 and h.ref.pending is None:
            h.control("BEGIN")
        elif step % 41 == 23 and h.ref.pending is not None:
            h.control("COMMIT" if (step // 41) % 2 else "ROLLBACK")
            h.audit()
        if step in (steps // 2, steps * 3 // 4):
            # Includes restart while a transaction has visible changes.
            h.restart(crash=step == steps // 2)
            h.pair(**random_query(h))
        if step % 37 == 11:
            fresh = h.row()
            duplicate = (h.rng.choice(list(h.ref.rows.values()))
                         if h.ref.rows and step % 2 else fresh)
            h.insert([fresh, h.row(), replace(duplicate, body="duplicate")])
        else:
            choice = h.rng.random()
            if choice < 0.26:
                rows = [h.row() for _ in range(h.rng.randint(1, 3))]
                missing = [key for key in range(-12, 13) if key not in h.ref.rows]
                if missing and h.rng.random() < 0.3:
                    rows[0] = replace(rows[0], id=h.rng.choice(missing))
                h.insert(rows, reordered=bool(step % 2))
                h.audit()
            elif choice < 0.50:
                replacement = h.row()
                fields = h.rng.sample(("score", "tag", "body", "emb"),
                                      h.rng.randint(1, 4))
                h.update({name: getattr(replacement, name) for name in fields},
                         random_conditions(h))
                h.audit()
            elif choice < 0.62:
                conditions = random_conditions(h)
                if not conditions and h.ref.rows and step % 53:
                    conditions = (("id", "=", h.rng.choice(list(h.ref.rows))),)
                h.delete(conditions)
                h.audit()
            else:
                h.query(**random_query(h))
        if step % 13 == 0:
            h.pair(**random_query(h))
        if step % 17 == 0:
            h.pair(columns=("id", "d"), probes={"d": (0.0,) * h.dim},
                   conditions=random_conditions(h), order=(("d", False),),
                   limit=7, offset=step % 4)
    if h.ref.pending is not None:
        h.control("COMMIT")
    h.restart()
    h.restart(crash=True)
    h.pair(columns=("id", "d"), probes={"d": (0.0,) * h.dim},
           order=(("d", False),), limit=17)


def run_semantics(h):
    def emb(first):
        return vector((first,) + (0,) * (h.dim - 1))

    h.audit()
    h.query(columns=("id", "d"), probes={"d": emb(0)},
            order=(("d", False),), limit=0)
    h.insert([h.row(key, score=score, tag=tag, body=TEXT[index % len(TEXT)], emb=emb(x))
              for index, (key, score, tag, x) in enumerate((
                  (BIGINT_MIN, BIGINT_MIN, "drop", 0),
                  (BIGINT_MAX, BIGINT_MAX, "drop", 0.25),
                  (-3, 0, "drop", 0.5), (10, 1, "keep", 2),
                  (20, 3, "keep", -2), (30, 3, "keep", 2),
                  (40, -1, "keep", -2), (0, 0, "中文😀", 3)))], True)
    h.audit()
    repeated = h.row()
    h.insert([h.row(), repeated, replace(repeated, body="autocommit duplicate")])
    h.insert([h.row(), replace(h.ref.rows[20], body="existing duplicate"), h.row()])
    for indexed in (False, True):
        if indexed:
            h.expect("CREATE VECTOR INDEX ix ON items(emb)", ["OK"])
        for order in ((("d", False),),
                      (("d", False), ("score", True), ("id", True)),
                      (("score", True), ("d", True), ("id", False)),
                      (("d", True), ("tag", True), ("id", True))):
            for limit, offset in ((0, 0), (1, 0), (2, 1), (9, 0), (2, 99)):
                h.pair(columns=("id", "score", "d"),
                       conditions=(("tag", "=", "keep"),), probes={"d": emb(0)},
                       order=order, limit=limit, offset=offset,
                       expression=bool(offset % 2), comma=bool(limit % 2))
        h.pair(columns=("id", "d"), conditions=(("tag", "=", "absent"),),
               probes={"d": emb(0)}, order=(("d", False),), limit=3)
    # Multiple independent selected distances and ORDER BY expressions.
    h.pair(columns=("id", "left_d", "right_d"),
           probes={"left_d": emb(0), "right_d": emb(2)},
           order=(("left_d", False), ("right_d", True), ("id", True)),
           expression=True, limit=5, offset=1)
    h.update({"emb": emb(0), "tag": "keep", "body": "索引更新😀"}, (("id", "=", -3),))
    h.delete((("id", "=", 10),))
    h.insert([h.row(10, tag="keep", emb=emb(1))])
    h.audit()
    h.pair(columns=("id", "d"), probes={"d": emb(0)},
           conditions=(("tag", "=", "keep"),), order=(("d", False),), limit=3)
    for finish in ("COMMIT", "ROLLBACK", "crash", "clean"):
        h.control("BEGIN")
        pending = h.row(tag="事务😀", body="prior successful statement", emb=emb(0))
        h.insert([pending])
        h.update({"score": BIGINT_MAX, "body": "事务内修改", "emb": emb(1)},
                 (("id", "=", 20),))
        h.delete((("id", "=", 30),))
        h.insert([h.row(30, body="delete then reinsert", emb=emb(3))])
        for duplicate in (pending, h.ref.rows[20]):
            h.insert([h.row(), h.row(), replace(duplicate, body="must not replace"), h.row()])
        repeated = h.row()
        h.insert([h.row(), repeated, replace(repeated, tag="batch duplicate"), h.row()])
        h.pair(columns=("id", "score", "d"), probes={"d": emb(0)},
               order=(("d", False), ("score", True), ("id", True)), limit=6)
        # Work after errors must still belong to the same transaction.
        h.insert([h.row(body="after failed statement")])
        h.audit()
        if finish in ("COMMIT", "ROLLBACK"):
            h.control(finish)
            h.audit()
            h.restart(crash=finish == "COMMIT")
        else:
            h.restart(crash=finish == "crash")
        h.pair(columns=("id", "d"), probes={"d": emb(0)},
               order=(("d", False),), limit=7)
    # Every supported predicate, binary Unicode order, and conjunctions.
    h.insert([h.row(body=text, tag=text) for text in TEXT])
    for column, pivot in (("id", 0), ("score", BIGINT_MIN),
                          ("score", BIGINT_MAX), ("tag", "é"), ("body", "中文")):
        for op in OPS:
            h.query(conditions=((column, op, pivot),),
                    order=(("tag", False), ("score", True), ("id", True)),
                    limit=11, offset=1)
    h.query(conditions=(("score", ">=", BIGINT_MIN), ("score", "<=", BIGINT_MAX),
                        ("tag", ">=", "")), order=(("body", True), ("id", False)))
    h.update({"tag": "all😀"})
    h.audit()
    h.control("BEGIN")
    h.delete()
    h.audit()
    h.control("ROLLBACK")
    h.audit()
    h.delete()
    h.audit()
    h.restart()


def run_numeric(h):
    def emb(first, second=0, tail=0):
        values = [tail] * h.dim
        values[0] = first
        if h.dim > 1:
            values[1] = second
        return vector(values)

    zero = emb(0)
    rows = []
    # Farther points intentionally have smaller primary keys. The first two
    # distances both print 1.000000, but must NOT be treated as a distance tie.
    for key, delta in ((1, 0.0003), (2, 0.0002), (3, 0.0001)):
        values = emb(1, delta) if h.dim > 1 else emb(1 + (4 - key) * 2 ** -23)
        rows.append(h.row(key, tag="near", score=key, emb=values))
    for key, value in enumerate((3e38, -3e38, 2e30, 1e30, -1e30, 0.0), 10):
        rows.append(h.row(key, tag="large", emb=emb(value)))
    for key, value in enumerate((3e-20, 2e-20, 1e-20, 1e-40, 0.0), 30):
        rows.append(h.row(key, tag="tiny", emb=emb(value)))
    for key in range(40, 52):
        rows.append(h.row(key, tag="decimal",
                          emb=vector(h.rng.uniform(-100, 100) for _ in range(h.dim))))
    rows.append(h.row(60, tag="mixed", emb=emb(8192, 1, 1)))
    rows.append(h.row(61, tag="mixed", emb=emb(8192, 0, 0)))
    h.insert(rows, True)
    h.audit()
    for indexed in (False, True):
        if indexed:
            h.expect("CREATE VECTOR INDEX ix ON items(emb)", ["OK"])
        for tag, probe in (("near", zero), ("large", zero),
                           ("large", emb(-3e38)), ("tiny", zero),
                           ("decimal", emb(0.1, -0.2, 0.3)), ("mixed", zero)):
            for limit, offset in ((None, 0), (1, 0), (2, 1), (50, 0), (0, 0), (3, 99)):
                h.pair(columns=("id", "d"), conditions=(("tag", "=", tag),),
                       probes={"d": probe}, order=(("d", False),),
                       limit=limit, offset=offset, expression=offset == 1, comma=limit == 50)
            h.pair(columns=("id", "score", "d"),
                   conditions=(("tag", "=", tag),), probes={"d": probe},
                   order=(("d", False), ("score", True), ("id", True)), limit=3)
            h.pair(columns=("id", "d"), conditions=(("tag", "=", tag),),
                   probes={"d": probe}, order=(("d", True),), limit=3, offset=1)
    h.update({"emb": emb(-1e30)}, (("id", "=", 10),))
    h.delete((("id", "=", 15),))
    h.insert([h.row(15, tag="large", emb=emb(1e-20))])
    h.control("BEGIN")
    h.update({"emb": zero}, (("id", "=", 11),))
    h.pair(columns=("id", "d"), probes={"d": emb(1e30)},
           order=(("d", False),), limit=10)
    h.control("ROLLBACK")
    h.restart(crash=True)
    h.pair(columns=("id", "d"), probes={"d": zero}, order=(("d", False),), limit=20)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--seed", type=lambda value: int(value, 0), action="append",
                        help="repeat to override the three fixed random seeds")
    parser.add_argument("--steps", type=int, default=360,
                        help="random operations per seed (default: 360)")
    parser.add_argument("--timeout", type=float, default=10,
                        help="real per-statement timeout in seconds (default: 10)")
    parser.add_argument("--case", choices=("random", "semantics", "numeric"),
                        action="append", help="repeat to select groups; default: all")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"executable does not exist: {executable}")
    if args.steps < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--steps and --timeout must be positive and finite")
    check_exact_distance_oracle()
    print("PASS exact distance oracle: 1D large-probe order and Fraction boundaries", flush=True)
    seeds = args.seed or SEEDS
    cases = args.case or ("random", "semantics", "numeric")
    jobs = []
    if "random" in cases:
        jobs.extend(("random", seed, SEED_DIMENSIONS.get(seed, (4, 8, 33)[seed % 3]),
                     lambda h: run_random(h, args.steps)) for seed in seeds)
    if "semantics" in cases:
        jobs.append(("semantics", seeds[0], 4, run_semantics))
    if "numeric" in cases:
        jobs.extend(("numeric", seeds[-1], dim, run_numeric)
                    for dim in (1, 3, 4, 8, 31, 32, 33, 128))
    failures = 0
    statements = 0
    query_mismatches = 0
    for label, seed, dim, test in jobs:
        h = Harness(executable, seed, dim, args.timeout, label)
        try:
            with h:
                test(h)
                if h.query_failures:
                    raise AssertionError(
                        f"{h.query_failures} query mismatches; first "
                        f"{len(h.failure_examples)} examples:\n" +
                        "\n\n".join(h.failure_examples)
                    )
        except Exception as error:
            failures += 1
            print(f"FAIL case={label} seed={seed} dim={dim} "
                  f"statements={h.count}\n{error}",
                  file=sys.stderr, flush=True)
        else:
            print(f"PASS case={label} seed={seed} dim={dim} statements={h.count}", flush=True)
        statements += h.count
        query_mismatches += h.query_failures
    print(f"{'FAIL' if failures else 'PASS'} differential: "
          f"{len(jobs) - failures}/{len(jobs)} cases, {statements} SQL statements, "
          f"{query_mismatches} query mismatches", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
