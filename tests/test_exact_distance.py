#!/usr/bin/env python3
"""Small exact-distance ordering regressions with an independent integer oracle.

Run after the main agent has built the requested executable(s):
    python -X utf8 -B tests/test_exact_distance.py build/final/minivectordb.exe
    python -X utf8 -B tests/test_exact_distance.py build/final/minivectordb.exe --scalar-executable build/scalar/minivectordb.exe

Every database test owns a temporary directory and uses test_extended.Session.
All input components use float32 .9g round-trip literals. The oracle obtains
integer lattice coordinates from Python as_integer_ratio(), not the engine's
mantissa/exponent expansion or its fixed-width carry/borrow implementation.
Only exact integers determine expected IDs; double math checks display text and
proves that the adversarial fixtures cannot be ordered by double scores alone.
This is correctness coverage, not a timing or throughput benchmark.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import math
from pathlib import Path
import random
import struct
import tempfile
import unittest

from test_extended import Session


SCALE = 1 << 149


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def literal(value: float) -> str:
    value = float32(value)
    if not math.isfinite(value):
        raise ValueError("only finite float32 components are supported")
    text = format(value, ".9g")
    if struct.pack("<f", float(text)) != struct.pack("<f", value):
        raise AssertionError(f"float32 round-trip failed: {value!r} -> {text!r}")
    return text


def vector_sql(vector) -> str:
    return "[" + ",".join(literal(value) for value in vector) + "]"


def integer_coordinate(value: float) -> int:
    numerator, denominator = float32(value).as_integer_ratio()
    scale, remainder = divmod(SCALE, denominator)
    if remainder:
        raise AssertionError("finite float32 must lie on the 2^-149 lattice")
    return numerator * scale


def exact_squared(vector, probe) -> int:
    if len(vector) != len(probe):
        raise ValueError("oracle vector dimension mismatch")
    return sum(
        (integer_coordinate(value) - integer_coordinate(query)) ** 2
        for value, query in zip(vector, probe)
    )


def approximate_squared(vector, probe) -> float:
    return math.fsum(
        (float32(value) - float32(query)) ** 2
        for value, query in zip(vector, probe)
    )


@dataclass(frozen=True)
class Sample:
    primary: int
    category: str
    score: int
    vector: tuple[float, ...]


def sample(primary: int, vector, index: int) -> Sample:
    return Sample(primary, "bca"[index % 3], index % 5 - 2,
                  tuple(float32(value) for value in vector))


def swamped_fixture(dimension: int = 17):
    huge = math.ldexp(1.0, 100)
    probe = [-huge / 2] + [(index % 5 - 2) / 4 for index in range(1, dimension)]
    rows = []
    for index in range(13):
        rank = index * 5 % 13 + 1
        vector = list(probe)
        vector[0] = huge
        vector[1] += rank if index % 2 else -rank
        rows.append(sample(index - 6, vector, index))
    return rows, probe


class OracleTests(unittest.TestCase):
    def test_float32_roundtrip_and_lattice(self):
        patterns = (
            0, 0x80000000, 1, 2, 0x80000001, 0x007FFFFF, 0x00800000,
            0x00800001, 0x3F800000, 0xBF800000, 0x3F800001,
            0x7F7FFFFF, 0xFF7FFFFF,
        )
        rng = random.Random(0xD157A)
        patterns += tuple(rng.randrange(0xFF000000) for _ in range(80))
        for bits in patterns:
            with self.subTest(bits=f"{bits:08x}"):
                value = from_bits(bits)
                literal(value)
                numerator, denominator = value.as_integer_ratio()
                self.assertEqual(integer_coordinate(value) * denominator, numerator * SCALE)
        self.assertEqual(integer_coordinate(from_bits(1)), 1)
        self.assertEqual(integer_coordinate(from_bits(0x80000001)), -1)
        self.assertEqual(integer_coordinate(from_bits(0x00800000)), 1 << 23)
        self.assertEqual(integer_coordinate(1.0), SCALE)

    def test_exact_identities_and_double_collision(self):
        tiny = from_bits(1)
        self.assertEqual(exact_squared([tiny], [-tiny]), 4)
        self.assertEqual(exact_squared([3, 4], [0, 0]), 25 * SCALE * SCALE)
        self.assertEqual(exact_squared([3, 4], [0, 0]), exact_squared([0, 5], [0, 0]))
        huge = math.ldexp(1.0, 100)
        self.assertLess(exact_squared([huge], [tiny]), exact_squared([-huge], [tiny]))
        rows, probe = swamped_fixture()
        scores = [exact_squared(row.vector, probe) for row in rows]
        self.assertEqual(len(set(scores)), len(rows))
        self.assertEqual(len({approximate_squared(row.vector, probe) for row in rows}), 1)
        self.assertEqual(len({math.sqrt(approximate_squared(row.vector, probe)) for row in rows}), 1)
        largest = from_bits(0x7F7FFFFF)
        total = exact_squared([largest] * 65, [-largest] * 65)
        self.assertEqual(total, 65 * (2 * integer_coordinate(largest)) ** 2)
        self.assertGreater(total.bit_length(), 550)
        self.assertLessEqual(total.bit_length(), 640)


class ExactDistanceTests(unittest.TestCase):
    executable: Path | None = None
    counters: dict[str, int] | None = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("supply a database executable to this script")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-exact-distance-")
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)

    @contextmanager
    def open_session(self):
        session = Session(self.executable, self.data_dir)
        try:
            yield session
        finally:
            try:
                session.close()
            finally:
                try:
                    session.kill()
                finally:
                    for stream in (session.process.stdin, session.process.stdout,
                                   session.process.stderr):
                        if stream is not None and not stream.closed:
                            stream.close()

    def execute(self, session: Session, sql: str):
        if self.counters is not None:
            self.counters["sql"] += 1
        return session.execute(sql)

    def expect_sql(self, session: Session, sql: str, expected):
        self.assertEqual(self.execute(session, sql), expected, msg=sql)

    def load_rows(self, session: Session, table: str, rows):
        dimension = len(rows[0].vector)
        self.assertTrue(all(len(row.vector) == dimension for row in rows))
        self.assertEqual(len({row.primary for row in rows}), len(rows))
        self.expect_sql(
            session,
            f"CREATE TABLE {table} (id BIGINT PRIMARY KEY, category VARCHAR(1), "
            f"score BIGINT, v VECTOR({dimension}))",
            ["OK"],
        )
        values = ",".join(
            f"({row.primary},'{row.category}',{row.score},'{vector_sql(row.vector)}')"
            for row in rows
        )
        self.expect_sql(session, f"INSERT INTO {table} VALUES {values}", [f"OK {len(rows)}"])
        self.expect_sql(session, f"CREATE VECTOR INDEX ix ON {table}(v)", ["OK"])

    def assert_swamped(self, rows, probe):
        exact = {exact_squared(row.vector, probe) for row in rows}
        self.assertGreater(len(exact), 1, "fixture needs distinct true distances")
        self.assertEqual(len({approximate_squared(row.vector, probe) for row in rows}), 1)
        self.assertEqual(len({math.sqrt(approximate_squared(row.vector, probe)) for row in rows}), 1)
        self.assertNotEqual(
            [row.primary for row in sorted(rows, key=lambda row: (exact_squared(row.vector, probe), row.primary))],
            sorted(row.primary for row in rows),
            "PK ordering must not accidentally satisfy the adversarial fixture",
        )

    def assert_order(self, session, sql, expected, probe, projected_distance):
        if self.counters is not None:
            self.counters["selects"] += 1
        output = self.execute(session, sql)
        self.assertEqual(len(output), len(expected), msg=f"{sql}\n{output!r}")
        actual_ids = []
        for line, row in zip(output, expected):
            fields = line.split("\t")
            self.assertEqual(len(fields), 2 if projected_distance else 1, msg=f"{sql}: {line!r}")
            actual_ids.append(int(fields[0]))
            if projected_distance:
                distance = float(fields[1])
                wanted = math.sqrt(math.ldexp(float(exact_squared(row.vector, probe)), -298))
                self.assertTrue(math.isfinite(distance) and distance >= 0.0, msg=sql)
                self.assertTrue(math.isclose(distance, wanted, rel_tol=1e-5, abs_tol=1e-5),
                                msg=f"{sql}: {distance!r} != {wanted!r}")
        self.assertEqual(actual_ids, [row.primary for row in expected], msg=sql)

    def exercise_orders(self, session, table, rows, probe, compact=False):
        probe = tuple(float32(value) for value in probe)
        second = list(probe)
        second[-1] = float32(second[-1] + 1.0)
        self.assertNotEqual(probe, tuple(second))
        expression = f"L2_DISTANCE(v,'{vector_sql(probe)}')"
        other_expression = f"L2_DISTANCE(v,'{vector_sql(second)}')"
        distance = lambda row: exact_squared(row.vector, probe)
        other_distance = lambda row: exact_squared(row.vector, second)
        specifications = (
            (False, expression, lambda row: (distance(row), row.primary)),
            (True, "d", lambda row: (distance(row), row.primary)),
            (False, expression + " DESC", lambda row: (-distance(row), row.primary)),
            (False, "category DESC," + expression + ",score DESC,id DESC",
             lambda row: (-ord(row.category), distance(row), -row.score, -row.primary)),
            (False, expression + ",score DESC,id DESC",
             lambda row: (distance(row), -row.score, -row.primary)),
            (False, expression + "," + other_expression + " DESC,score,id DESC",
             lambda row: (distance(row), -other_distance(row), row.score, -row.primary)),
            (False, expression + ",id ASC", lambda row: (distance(row), row.primary)),
            (True, "d,id ASC", lambda row: (distance(row), row.primary)),
            (True, "d,id DESC", lambda row: (distance(row), -row.primary)),
        )
        windows = (("", 0, None), (" LIMIT 1", 0, 1),
                   (" LIMIT 5 OFFSET 2", 2, 5), (" LIMIT 2,5", 2, 5))
        if compact:
            specifications = tuple(specifications[index] for index in (0, 1, 3, 5, 6, 7, 8))
            windows = (("", 0, None), (" LIMIT 3 OFFSET 1", 1, 3))
        for mode in ("exact", "ann"):
            self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
            for projected, order, key in specifications:
                ordered = sorted(rows, key=key)
                projection = f"id,{expression} AS d" if projected else "id"
                for suffix, begin, count in windows:
                    end = None if count is None else begin + count
                    sql = f"SELECT {projection} FROM {table} ORDER BY {order}{suffix}"
                    with self.subTest(mode=mode, sql=sql):
                        self.assert_order(session, sql, ordered[begin:end], probe, projected)

    def test_common_large_component_has_distinct_exact_topk(self):
        rows, probe = swamped_fixture()
        self.assert_swamped(rows, probe)
        with self.open_session() as session:
            self.load_rows(session, "swamped", rows)
            self.exercise_orders(session, "swamped", rows, probe)

    def test_opposite_signs_and_nonzero_tiny_probes(self):
        huge = math.ldexp(1.0, 100)
        rows = [sample(primary, [value], index) for index, (primary, value) in enumerate(
            ((-9, -huge), (17, huge), (9, -huge), (-2, huge),
             (25, math.ldexp(1.0, 101)), (-15, -math.ldexp(1.0, 101))))]
        with self.open_session() as session:
            self.load_rows(session, "signs", rows)
            for query in (1.0, -1.0, from_bits(1), -from_bits(1)):
                with self.subTest(probe=query):
                    probe = [query]
                    near = [row for row in rows if abs(row.vector[0]) == huge]
                    self.assert_swamped(near, probe)
                    self.exercise_orders(session, "signs", rows, probe, compact=True)

    def test_carry_borrow_and_extreme_exponent_mixture(self):
        largest = from_bits(0x7F7FFFFF)
        biased_exponents = (1, 2, 31, 32, 63, 64, 95, 96, 127, 128,
                            159, 160, 191, 192, 223, 224, 253, 254)
        probe = [largest]
        baseline = [-largest]
        for index in range(63):
            exponent = biased_exponents[index % len(biased_exponents)]
            mantissa = (0x7FFFFE, 0x3FFFFF, 1)[index % 3]
            bits = (exponent << 23) | mantissa
            if index % 2:
                bits |= 0x80000000
            probe.append(from_bits(bits))
            # Same-sign near-cancellation forces long borrow propagation;
            # opposite signs and repeated high terms also exercise carries.
            baseline.append(from_bits(bits + 1) if index % 4 else -from_bits(bits))
        probe.append(from_bits(2))
        baseline.append(0.0)
        rows = []
        for index in range(13):
            vector = list(baseline)
            vector[-1] = from_bits(index * 5 % 13 + 3)
            rows.append(sample(40 - index * 7, vector, index))
        self.assertGreater(exact_squared(rows[0].vector, probe).bit_length(), 550)
        self.assert_swamped(rows, probe)
        with self.open_session() as session:
            self.load_rows(session, "limbs", rows)
            self.exercise_orders(session, "limbs", rows, probe)

    def test_subnormal_boundaries_and_signed_zero(self):
        probe = [from_bits(1), -from_bits(2), from_bits(0x007FFFFF)] + [0.0] * 14
        patterns = (0, 1, 2, 3, 0x007FFFFF, 0x00800000, 0x00800001,
                    0x80000001, 0x80000002, 0x807FFFFF, 0x80800000)
        rows = []
        for index, bits in enumerate(patterns):
            vector = list(probe)
            vector[0] = from_bits(bits)
            vector[1] = from_bits(index % 5)
            rows.append(sample(50 - index * 9, vector, index))
        for primary, zero in ((91, 0.0), (-91, -0.0)):
            vector = list(probe)
            vector[4] = zero
            rows.append(sample(primary, vector, len(rows)))
        self.assertEqual(exact_squared(rows[-1].vector, probe), 0)
        self.assertEqual(exact_squared(rows[-2].vector, probe), 0)
        with self.open_session() as session:
            self.load_rows(session, "subnormals", rows)
            self.exercise_orders(session, "subnormals", rows, probe)

    def test_true_distance_ties_use_secondary_keys_then_pk(self):
        huge = math.ldexp(1.0, 100)
        probe = [-huge / 2, 1.0, -2.0] + [0.0] * 14
        displacements = ((3, 4), (0, 5), (-3, -4), (5, 0), (4, -3),
                         (-5, 0), (0, 0), (1, 7))
        primaries = (17, -5, 0, 33, -21, 9, 44, -10)
        rows = []
        for index, ((x, y), primary) in enumerate(zip(displacements, primaries)):
            vector = list(probe)
            vector[0] = huge
            vector[1] += x
            vector[2] += y
            rows.append(sample(primary, vector, index))
        self.assertEqual(len({exact_squared(row.vector, probe) for row in rows[:6]}), 1)
        self.assert_swamped(rows, probe)
        with self.open_session() as session:
            self.load_rows(session, "ties", rows)
            self.exercise_orders(session, "ties", rows, probe)

    def test_seeded_bit_patterns_cover_simd_blocks_and_tails(self):
        rng = random.Random(0xE7AC7)
        huge = math.ldexp(1.0, 100)
        with self.open_session() as session:
            for dimension in (2, 3, 4, 15, 16, 17, 31, 32, 33, 65):
                with self.subTest(dimension=dimension):
                    probe = [-huge / 2]
                    baseline = [huge]
                    for _ in range(dimension - 2):
                        bits = (rng.randrange(1, 255) << 23) | rng.randrange(0x7FFFFE)
                        bits |= rng.randrange(2) << 31
                        probe.append(from_bits(bits))
                        baseline.append(from_bits(bits + 1))
                    probe.append(from_bits(1))
                    baseline.append(0.0)
                    rows = []
                    for index in range(9):
                        vector = list(baseline)
                        vector[-1] = from_bits(index * 5 % 9 + 2)
                        rows.append(sample(31 - index * 8, vector, index))
                    self.assert_swamped(rows, probe)
                    table = f"tail_{dimension}"
                    self.load_rows(session, table, rows)
                    self.exercise_orders(session, table, rows, probe, compact=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", type=Path)
    parser.add_argument("--scalar-executable", type=Path,
                        help="also run the identical suite on a separately built MVD_FORCE_SCALAR binary")
    parser.add_argument("--oracle-only", action="store_true",
                        help="check the independent oracle without starting a database")
    args = parser.parse_args()
    if not args.oracle_only and args.executable is None:
        parser.error("provide an executable, or use --oracle-only")
    binaries = []
    if not args.oracle_only:
        for label, path in (("default", args.executable), ("scalar", args.scalar_executable)):
            if path is None:
                continue
            path = path.resolve()
            if not path.is_file():
                parser.error(f"database executable does not exist: {path}")
            binaries.append((label, path))
    runner = unittest.TextTestRunner(verbosity=2)
    oracle_result = runner.run(unittest.defaultTestLoader.loadTestsFromTestCase(OracleTests))
    if not oracle_result.wasSuccessful():
        return 1
    successful = True
    for label, executable in binaries:
        counters = {"sql": 0, "selects": 0}
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ExactDistanceTests)
        for test in suite:
            test.executable = executable
            test.counters = counters
        print(f"Running {label}: {executable}", flush=True)
        result = runner.run(suite)
        successful = successful and result.wasSuccessful()
        status = "PASS" if result.wasSuccessful() else "FAIL"
        print(f"{status} {label}: {result.testsRun} database tests, "
              f"{counters['sql']} SQL statements, {counters['selects']} exact-order SELECTs",
              flush=True)
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())
