#!/usr/bin/env python3
"""Decimal-to-float32 boundary checks with an independent rational oracle.

Every database owns a fresh temporary directory. Decimal literals are compared
to exact binary32 midpoints; Python's decimal-to-double conversion is not the
rounding oracle. Only finite numeric values and ordinary SQL are exercised.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import json
from pathlib import Path
import struct
import tempfile
import unittest

from test_differential import TimedSession


MAXIMUM = 0x7F7FFFFF


def as_fraction(bits):
    return Fraction.from_float(struct.unpack("<f", struct.pack("<I", bits))[0])


def decimal_text(value):
    """Exact terminating decimal for this suite's power-of-two denominators."""
    denominator = value.denominator
    exponent = denominator.bit_length() - 1
    if denominator != 1 << exponent:
        raise ValueError("fixture denominator must be a power of two")
    sign = "-" if value < 0 else ""
    digits = str(abs(value.numerator) * 5 ** exponent)
    if exponent == 0:
        return sign + digits
    digits = digits.zfill(exponent + 1)
    return sign + digits[:-exponent] + "." + digits[-exponent:]


def nearest_bits(text):
    """Search the finite float32 lattice and round exact fractions to even."""
    value = Fraction(text)
    sign = 0x80000000 if text.startswith("-") else 0
    value = abs(value)
    maximum = as_fraction(MAXIMUM)
    overflow_midpoint = maximum + (maximum - as_fraction(MAXIMUM - 1)) / 2
    if value >= overflow_midpoint:
        raise OverflowError("rounded float32 overflows")
    if value >= maximum:
        return sign | MAXIMUM
    low, high = 0, MAXIMUM
    while low + 1 < high:
        middle = (low + high) // 2
        if as_fraction(middle) <= value:
            low = middle
        else:
            high = middle
    below = value - as_fraction(low)
    above = as_fraction(high) - value
    chosen = low if below < above or (below == above and low % 2 == 0) else high
    return sign | chosen


def result_bits(text):
    value = json.loads(text, parse_int=float)
    if not isinstance(value, list) or len(value) != 1:
        raise AssertionError(f"not a one-dimensional vector: {text!r}")
    return struct.unpack("<I", struct.pack("<f", value[0]))[0]


def canonical_bits(bits):
    # The output contract may spell both signed zeros as 0. Their numerical
    # value and distance are identical; sign preservation is not required.
    return 0 if bits & 0x7FFFFFFF == 0 else bits


def boundary_literals():
    values = ["1.0000000596046448", "-1.0000000596046448", "1e-80", "-1e-80"]
    for low_bits in (0, 1, 0x007FFFFF, 0x00800000, 0x3F800000,
                     0x3F800001, 0x4B800000, MAXIMUM - 1):
        low, high = as_fraction(low_bits), as_fraction(low_bits + 1)
        midpoint = (low + high) / 2
        epsilon = (high - low) / (1 << 80)
        for value in (midpoint - epsilon, midpoint, midpoint + epsilon):
            values.extend((decimal_text(value), decimal_text(-value)))
    return values


class LiteralOracle(unittest.TestCase):
    def test_midpoint_ties_and_direction(self):
        midpoint = (as_fraction(0x3F800000) + as_fraction(0x3F800001)) / 2
        self.assertGreater(Fraction("1.0000000596046448"), midpoint)
        self.assertEqual(nearest_bits("1.0000000596046448"), 0x3F800001)
        self.assertEqual(nearest_bits("-1.0000000596046448"), 0xBF800001)
        self.assertEqual(nearest_bits(decimal_text(midpoint)), 0x3F800000)
        self.assertEqual(nearest_bits("1e-80"), 0)
        self.assertEqual(nearest_bits("-1e-80"), 0x80000000)
        for low_bits in (0, 1, 0x007FFFFF, 0x3F800000, 0x3F800001):
            midpoint = (as_fraction(low_bits) + as_fraction(low_bits + 1)) / 2
            even = low_bits if low_bits % 2 == 0 else low_bits + 1
            self.assertEqual(nearest_bits(decimal_text(midpoint)), even)


class Float32Literals(unittest.TestCase):
    executable = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("provide the database executable")
        directory = tempfile.TemporaryDirectory(prefix="mvd-float32-literals-")
        self.addCleanup(directory.cleanup)
        self.session = TimedSession(self.executable, Path(directory.name), 8)
        self.addCleanup(self.session.shutdown)

    def expect(self, sql, expected):
        self.assertEqual(self.session.execute(sql), expected, sql)

    def test_vector_literal_whitespace_and_terminator(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(3))", ["OK"])
        self.expect(
            "INSERT INTO t VALUES(1,'[ 1 , -2e-1, 3. ] ;')", ["OK 1"]
        )
        self.expect("SELECT v FROM t WHERE id=1", ["[1,-0.200000003,3]"])

    def test_insert_rounds_exact_decimal_directly_to_float32(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        literals = boundary_literals()
        values = ",".join(f"({index},'[{text}]')" for index, text in enumerate(literals))
        self.expect("INSERT INTO t VALUES " + values, [f"OK {len(literals)}"])
        rows = self.session.execute("SELECT id,v FROM t ORDER BY id")
        self.assertEqual(len(rows), len(literals))
        for index, (text, row) in enumerate(zip(literals, rows)):
            with self.subTest(literal=text):
                primary, vector = row.split("\t")
                self.assertEqual(int(primary), index)
                self.assertEqual(canonical_bits(result_bits(vector)),
                                 canonical_bits(nearest_bits(text)), text)

    def test_literal_rounding_changes_stored_topk_membership(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        self.expect("INSERT INTO t VALUES(1,'[1.0000000596046448]'),(2,'[1]')", ["OK 2"])
        self.expect("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1]') LIMIT 1", ["2"])
        self.expect("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1]'),id ASC LIMIT 2", ["2", "1"])

    def test_probe_and_update_use_the_same_direct_rounding(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        next_one = decimal_text(as_fraction(0x3F800001))
        self.expect(f"INSERT INTO t VALUES(1,'[1]'),(2,'[{next_one}]')", ["OK 2"])
        for suffix in ("", ",id ASC"):
            with self.subTest(order_suffix=suffix):
                self.expect("SELECT id FROM t ORDER BY "
                            f"L2_DISTANCE(v,'[1.0000000596046448]'){suffix} LIMIT 1", ["2"])
        with self.subTest(projected_distance=True):
            self.expect("SELECT id,L2_DISTANCE(v,'[1.0000000596046448]') AS d "
                        "FROM t ORDER BY d LIMIT 1", ["2\t0.000000"])
        self.expect("UPDATE t SET v='[1.0000000596046448]' WHERE id=1", ["OK 1"])
        rows = self.session.execute("SELECT v FROM t WHERE id=1")
        self.assertEqual(len(rows), 1)
        self.assertEqual(result_bits(rows[0]), 0x3F800001)

    def test_overflow_midpoint_is_not_rounded_via_double(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        maximum = as_fraction(MAXIMUM)
        ulp = maximum - as_fraction(MAXIMUM - 1)
        midpoint = maximum + ulp / 2
        epsilon = ulp / (1 << 80)
        for primary, sign in ((1, 1), (2, -1)):
            text = decimal_text(sign * (midpoint - epsilon))
            self.expect(f"INSERT INTO t VALUES({primary},'[{text}]')", ["OK 1"])
            row = self.session.execute(f"SELECT v FROM t WHERE id={primary}")
            self.assertEqual(result_bits(row[0]), MAXIMUM | (0x80000000 if sign < 0 else 0))
        for boundary in (midpoint, midpoint + epsilon):
            for sign in (1, -1):
                text = decimal_text(sign * boundary)
                output = self.session.execute(f"INSERT INTO t VALUES(3,'[{text}]')")
                self.assertEqual(len(output), 1)
                self.assertRegex(output[0], r"^ERROR [^\s:]+: .+")
                self.expect("SELECT id FROM t ORDER BY id", ["1", "2"])

    def test_tiny_decimal_exponents_round_to_zero(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        literals = ("1e-999", "-1e-999", "0.00001e-500", "1000000000e-999",
                    "+1e-999", "1e-999999999999999999999", "-1e-999999999999999999999",
                    "0e999999999999999999999", "-0e-999999999999999999999")
        for primary, text in enumerate(literals):
            with self.subTest(literal=text):
                # All are exactly zero or have magnitude at most 1e-500,
                # strictly below half the smallest positive float32 value.
                self.expect(f"INSERT INTO t VALUES({primary},'[{text}]')", ["OK 1"])
                row = self.session.execute(f"SELECT v FROM t WHERE id={primary}")
                self.assertEqual(canonical_bits(result_bits(row[0])), 0)

    def test_overflow_and_nonfinite_values_remain_atomic_errors(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        self.expect("INSERT INTO t VALUES(1,'[1]')", ["OK 1"])
        for literal in ("3.5e38", "1e999", "1e999999999999999999999",
                        "0.00001e999999999999999999999", "NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                output = self.session.execute(f"INSERT INTO t VALUES(2,'[2]'),(3,'[{literal}]')")
                self.assertEqual(len(output), 1)
                self.assertRegex(output[0], r"^ERROR [^\s:]+: .+")
                self.expect("SELECT id FROM t ORDER BY id", ["1"])

    def test_numeric_token_boundaries_preserve_error_codes_and_atomicity(self):
        self.expect("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))", ["OK"])
        self.expect("INSERT INTO t VALUES(1,'[1]')", ["OK 1"])
        cases = (("1e", "SQL_SYNTAX"), ("1.e+", "SQL_SYNTAX"),
                 ("-1e-", "SQL_SYNTAX"), ("1e1e", "SQL_SYNTAX"),
                 ("1.2.3", "SQL_SYNTAX"), ("1e-999e", "SQL_SYNTAX"),
                 ("1e999e", "VALUE_OUT_OF_RANGE"),
                 (".5", "TYPE_MISMATCH"), ("+", "TYPE_MISMATCH"))
        for literal, code in cases:
            with self.subTest(literal=literal):
                output = self.session.execute(
                    f"INSERT INTO t VALUES(2,'[2]'),(3,'[{literal}]')")
                self.assertEqual(len(output), 1)
                self.assertTrue(output[0].startswith(f"ERROR {code}:"), output)
                self.expect("SELECT id FROM t ORDER BY id", ["1"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    Float32Literals.executable = args.executable.resolve(strict=True)
    suite = unittest.TestSuite((
        unittest.defaultTestLoader.loadTestsFromTestCase(LiteralOracle),
        unittest.defaultTestLoader.loadTestsFromTestCase(Float32Literals),
    ))
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
