#!/usr/bin/env python3
"""Focused MiniVectorDB correctness regressions and query boundary tests.

Run with an explicit executable, for example:
    python -X utf8 -B tests/test_hidden_edges.py ./build/minivectordb.exe
    python -X utf8 -B tests/test_hidden_edges.py ./build/minivectordb.exe --case recovery

Every test owns an isolated temporary data directory. The recovery test restores
a real WAL prefix after a clean shutdown to SIMULATE an interrupted commit whose
row records reached disk but whose commit record did not. This storage fixture
is deterministic; it is not evidence of an actual forced-process crash.

Distance references round both operands to float32 using struct. Ordering uses
independent Fraction arithmetic on float.as_integer_ratio(), including cases
where double subtraction or math.fsum cannot distinguish the real distances.
Returned distance text uses math.fsum/sqrt and the published 1e-5 absolute/relative
tolerance. TopK IDs/order are exact even when response text is rounded identically.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from fractions import Fraction
import math
from pathlib import Path
import struct
import tempfile
import unittest

from test_extended import Session


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def reference_distance(vector: list[float], probe: list[float]) -> float:
    if len(vector) != len(probe):
        raise ValueError("reference vector dimension mismatch")
    return math.sqrt(
        math.fsum(
            (float32(value) - float32(query)) ** 2
            for value, query in zip(vector, probe)
        )
    )


def reference_squared_distance(vector: list[float], probe: list[float]) -> Fraction:
    """Independent rational truth; no engine mantissa, limb, or summation code."""
    if len(vector) != len(probe):
        raise ValueError("reference vector dimension mismatch")
    return sum(
        (Fraction(*float32(value).as_integer_ratio()) -
         Fraction(*float32(query).as_integer_ratio())) ** 2
        for value, query in zip(vector, probe)
    )


def vector_sql(vector: list[float]) -> str:
    return "[" + ",".join(repr(value) for value in vector) + "]"


@contextmanager
def open_session(executable: Path, data_dir: Path):
    session = Session(executable, data_dir)
    try:
        yield session
    finally:
        try:
            session.close()
        finally:
            try:
                session.kill()
            finally:
                for stream in (
                    session.process.stdin,
                    session.process.stdout,
                    session.process.stderr,
                ):
                    if stream is not None and not stream.closed:
                        stream.close()


class HiddenEdges(unittest.TestCase):
    executable: Path | None = None

    def setUp(self):
        if self.executable is None:
            self.skipTest("run this script with the database executable argument")
        temporary = tempfile.TemporaryDirectory(prefix="mvd-hidden-edges-")
        self.addCleanup(temporary.cleanup)
        self.data_dir = Path(temporary.name)

    def expect_sql(self, session: Session, sql: str, expected: list[str]):
        self.assertEqual(session.execute(sql), expected, msg=sql)

    def expect_error(self, session: Session, sql: str):
        output = session.execute(sql)
        self.assertTrue(
            len(output) == 1 and output[0].startswith("ERROR "),
            msg=f"expected error for {sql!r}, got {output!r}",
        )

    def expect_distances(
        self, session: Session, sql: str, expected: list[tuple[int, float]]
    ):
        output = session.execute(sql)
        self.assertEqual(len(output), len(expected), msg=f"{sql}: {output!r}")
        actual = []
        for line in output:
            fields = line.split("\t")
            self.assertEqual(len(fields), 2, msg=f"{sql}: {line!r}")
            primary, distance = int(fields[0]), float(fields[1])
            self.assertTrue(
                math.isfinite(distance) and distance >= 0.0,
                msg=f"{sql}: non-finite or negative distance {line!r}",
            )
            actual.append((primary, distance))
        self.assertEqual(
            [primary for primary, _ in actual],
            [primary for primary, _ in expected],
            msg=f"exact TopK/order mismatch for {sql}",
        )
        for (primary, distance), (_, wanted) in zip(actual, expected):
            self.assertTrue(
                math.isclose(distance, wanted, rel_tol=1e-5, abs_tol=1e-5),
                msg=f"{sql}: id={primary}, distance={distance!r}, expected={wanted!r}",
            )

    def test_recovery_does_not_reuse_cross_table_txid(self):
        """Simulated interrupted-commit storage fixture, not a real process crash."""
        tables = ("left_rows", "right_rows")
        for next_write_table in tables:
            with self.subTest(next_write_table=next_write_table):
                data_dir = self.data_dir / next_write_table
                wal_path = data_dir / "mvd.wal"
                with open_session(self.executable, data_dir) as session:
                    for table in tables:
                        self.expect_sql(
                            session,
                            f"CREATE TABLE {table} (id BIGINT PRIMARY KEY)",
                            ["OK"],
                        )
                    self.expect_sql(session, "BEGIN", ["OK"])
                    for table in tables:
                        self.expect_sql(
                            session, f"INSERT INTO {table} VALUES (1)", ["OK 1"]
                        )
                    self.expect_sql(session, "COMMIT", ["OK"])
                    committed_wal = wal_path.read_bytes()
                    self.assertTrue(committed_wal, "row1 must have a real commit record")
                    row1_files = {
                        path.name: path.read_bytes() for path in data_dir.glob("t*.dat")
                    }
                    self.assertEqual(len(row1_files), 2)
                    self.expect_sql(session, "BEGIN", ["OK"])
                    for table in tables:
                        self.expect_sql(
                            session, f"INSERT INTO {table} VALUES (2)", ["OK 1"]
                        )
                    self.expect_sql(session, "COMMIT", ["OK"])
                    for table in tables:
                        self.expect_sql(
                            session, f"SELECT id FROM {table} ORDER BY id", ["1", "2"]
                        )

                # The database is closed before the fixture changes any bytes.
                row2_wal = wal_path.read_bytes()
                self.assertGreater(len(row2_wal), len(committed_wal))
                self.assertTrue(row2_wal.startswith(committed_wal))
                row2_files = {
                    path.name: path.read_bytes() for path in data_dir.glob("t*.dat")
                }
                self.assertEqual(set(row2_files), set(row1_files))
                for name, before in row1_files.items():
                    self.assertGreater(len(row2_files[name]), len(before), msg=name)
                    self.assertTrue(row2_files[name].startswith(before), msg=name)

                # SIMULATION: preserve real row records in BOTH tables, but remove
                # only the last transaction's real commit record from this temp WAL.
                # No hand-encoded records, CRC edits, timing races, or process kills.
                self.assertEqual(wal_path.write_bytes(committed_wal), len(committed_wal))
                self.assertEqual(wal_path.read_bytes(), committed_wal)
                for name, expected_bytes in row2_files.items():
                    self.assertEqual((data_dir / name).read_bytes(), expected_bytes)

                expected_by_table = {table: ["1"] for table in tables}
                with open_session(self.executable, data_dir) as session:
                    for table in tables:
                        self.expect_sql(
                            session, f"SELECT id FROM {table} ORDER BY id", ["1"]
                        )
                    # A new commit in one table must not commit orphaned versions
                    # in that table OR in the other table through a reused txid.
                    self.expect_sql(
                        session, f"INSERT INTO {next_write_table} VALUES (3)", ["OK 1"]
                    )
                    expected_by_table[next_write_table] = ["1", "3"]
                    for table in tables:
                        self.expect_sql(
                            session,
                            f"SELECT id FROM {table} ORDER BY id",
                            expected_by_table[table],
                        )

                with open_session(self.executable, data_dir) as session:
                    for table in tables:
                        with self.subTest(recovered_table=table):
                            self.expect_sql(
                                session,
                                f"SELECT id FROM {table} ORDER BY id",
                                expected_by_table[table],
                            )

    def check_vector_dataset(self, vectors: list[list[float]], probe: list[float]):
        expected = sorted(
            (
                (primary, reference_distance(vector, probe))
                for primary, vector in enumerate(vectors, 1)
            ),
            key=lambda row: (row[1], row[0]),
        )
        dimension = len(probe)
        data_dir = self.data_dir / f"dimension_{dimension}"
        with open_session(self.executable, data_dir) as session:
            self.expect_sql(
                session,
                f"CREATE TABLE vectors (id BIGINT PRIMARY KEY, v VECTOR({dimension}))",
                ["OK"],
            )
            rows = ",".join(
                f"({primary},'{vector_sql(vector)}')"
                for primary, vector in enumerate(vectors, 1)
            )
            self.expect_sql(session, f"INSERT INTO vectors VALUES {rows}", [f"OK {len(vectors)}"])
            self.expect_sql(session, "CREATE VECTOR INDEX vx ON vectors(v)", ["OK"])
            select = (
                f"SELECT id,L2_DISTANCE(v,'{vector_sql(probe)}') AS d "
                "FROM vectors ORDER BY d"
            )
            for mode in ("exact", "ann"):
                self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
                for suffix, wanted in (
                    ("", expected),
                    (" LIMIT 1", expected[:1]),
                    (" LIMIT 2 OFFSET 1", expected[1:3]),
                ):
                    with self.subTest(mode=mode, query_suffix=suffix):
                        self.expect_distances(session, select + suffix, wanted)

    def test_large_finite_distances(self):
        for dimension in (2, 4, 32, 128):
            with self.subTest(dimension=dimension):
                padding = [0.0] * (dimension - 1)
                self.check_vector_dataset(
                    [[2e30] + padding, [1e30] + padding, [-3e30] + padding],
                    [0.0] * dimension,
                )

    def test_tiny_distances_preserve_exact_topk(self):
        for dimension in (2, 4, 32, 128):
            with self.subTest(dimension=dimension):
                padding = [0.0] * (dimension - 1)
                self.check_vector_dataset(
                    [[2e-30] + padding, [1e-30] + padding, [-3e-30] + padding],
                    [0.0] * dimension,
                )

    def test_near_distances_are_not_false_ties(self):
        for dimension in (2, 4, 32, 128):
            with self.subTest(dimension=dimension):
                padding = [0.0] * (dimension - 2)
                self.check_vector_dataset(
                    [[1.0, 1e-4] + padding, [1.0, 0.0] + padding, [1.0, 2e-4] + padding],
                    [0.0] * dimension,
                )

    def check_exact_order_dataset(self, rows, probe, fixture):
        """Exercise both heaps/full sorts with projected and hidden distances."""
        first = f"L2_DISTANCE(v,'{vector_sql(probe)}')"
        second_probe = list(probe)
        second_probe[-1] = 1.0 if probe[-1] != 1.0 else -1.0
        second = f"L2_DISTANCE(v,'{vector_sql(second_probe)}')"
        truth = {primary: reference_squared_distance(values, probe)
                 for primary, _, values in rows}
        other = {primary: reference_squared_distance(values, second_probe)
                 for primary, _, values in rows}
        distances = {primary: reference_distance(values, probe)
                     for primary, _, values in rows}
        orders = (
            ("{d}", lambda row: (truth[row[0]], row[0])),
            ("{d},score DESC,id DESC",
             lambda row: (truth[row[0]], -row[1], -row[0])),
            ("{d} DESC,score ASC", lambda row: (-truth[row[0]], row[1], row[0])),
            ("score DESC,{d},id DESC", lambda row: (-row[1], truth[row[0]], -row[0])),
            ("{d},{e} DESC,score ASC",
             lambda row: (truth[row[0]], -other[row[0]], row[1], row[0])),
        )
        windows = (
            ("", 0, len(rows)), (" LIMIT 1", 0, 1),
            (" LIMIT 2 OFFSET 1", 1, 3), (" LIMIT 1,2", 1, 3),
            (" LIMIT 0", 0, 0),
        )
        with open_session(self.executable, self.data_dir / fixture) as session:
            self.expect_sql(
                session,
                f"CREATE TABLE mixed (id BIGINT PRIMARY KEY, score BIGINT, v VECTOR({len(probe)}))",
                ["OK"],
            )
            values = ",".join(
                f"({primary},{score},'{vector_sql(values)}')"
                for primary, score, values in rows
            )
            self.expect_sql(session, f"INSERT INTO mixed VALUES {values}", [f"OK {len(rows)}"])
            for indexed in (False, True):
                if indexed:
                    self.expect_sql(session, "CREATE VECTOR INDEX mx ON mixed(v)", ["OK"])
                for mode in ("exact", "ann"):
                    self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
                    # The original two-row counterexample must choose ID 2,
                    # rather than hide the error behind a later exact tie.
                    pair = sorted((row for row in rows if row[0] <= 2),
                                  key=lambda row: (truth[row[0]], row[0]))[:1]
                    self.expect_sql(
                        session, f"SELECT id FROM mixed WHERE id<=2 ORDER BY {first} LIMIT 1",
                        [str(row[0]) for row in pair],
                    )
                    for projected in (False, True):
                        projection = f"id,{first} AS d" if projected else "id"
                        for template, key in orders:
                            ordered = sorted(rows, key=key)
                            order = template.format(d="d" if projected else first, e=second)
                            for suffix, begin, end in windows:
                                with self.subTest(fixture=fixture, indexed=indexed, mode=mode,
                                                  projected=projected, order=order, window=suffix):
                                    sql = f"SELECT {projection} FROM mixed ORDER BY {order}{suffix}"
                                    wanted = ordered[begin:end]
                                    if projected:
                                        self.expect_distances(
                                            session, sql,
                                            [(row[0], distances[row[0]]) for row in wanted],
                                        )
                                    else:
                                        self.expect_sql(session, sql, [str(row[0]) for row in wanted])

    def test_mixed_scale_16d_order_is_kernel_independent(self):
        farther = [1e8] + [1.0] * 15
        nearer = farther.copy()
        for index in (4, 8, 12):
            nearer[index] = 0.0
        probe = [0.0] * 16
        self.assertEqual(
            reference_squared_distance(farther, probe) -
            reference_squared_distance(nearer, probe), 3,
        )
        # Here fsum independently distinguishes the distances; scalar's former
        # four-lane accumulation incorrectly tied the two rows at 1e16+12.
        self.assertGreater(reference_distance(farther, probe), reference_distance(nearer, probe))
        self.check_exact_order_dataset(
            [(1, 9, farther), (2, 1, nearer), (3, 9, nearer.copy()),
             (4, 7, [-value for value in nearer])], probe, "mixed_16d",
        )

    def test_exact_order_when_double_and_fsum_tie(self):
        farther = [float32(1e30)] + [1.0] * 15
        nearer = farther.copy()
        for index in (4, 8, 12):
            nearer[index] = 0.0
        datasets = (
            ("huge_leading_term", [0.0] * 16,
             [(1, 9, farther), (2, 1, nearer), (3, 9, nearer.copy()),
              (4, 7, [-value for value in nearer])]),
            ("large_probe_1d", [float32(1e30)],
             [(1, 9, [-1.0]), (2, 1, [0.0]), (3, 9, [1.0]),
              (4, 7, [2.0]), (5, 7, [2.0])]),
        )
        for fixture, probe, rows in datasets:
            with self.subTest(fixture=fixture):
                self.assertEqual(len({reference_distance(row[2], probe) for row in rows}), 1)
                self.assertGreater(len({reference_squared_distance(row[2], probe) for row in rows}), 1)
                self.check_exact_order_dataset(rows, probe, fixture)

    def test_exact_key_maximum_subnormal_and_cancellation(self):
        maximum = float32(3.4028234663852886e38)
        tiny = float32(2**-149)
        datasets = (
            ("opposite_maximum", [maximum] + [0.0] * 15,
             [(1, 9, [-maximum, tiny] + [0.0] * 14),
              (2, 1, [-maximum] + [0.0] * 15),
              (3, 9, [-maximum, -tiny] + [0.0] * 14),
              (4, 7, [-maximum, 2 * tiny] + [0.0] * 14)]),
            ("cancel_maximum", [maximum] + [tiny] * 15,
             [(1, 9, [maximum] + [0.0] * 15),
              (2, 1, [maximum] + [tiny] * 15),
              (3, 9, [maximum] + [2 * tiny] * 15),
              (4, 7, [maximum] + [-tiny] * 15)]),
        )
        for fixture, probe, rows in datasets:
            with self.subTest(fixture=fixture):
                self.assertGreater(reference_squared_distance(rows[0][2], probe),
                                   reference_squared_distance(rows[1][2], probe))
                self.check_exact_order_dataset(rows, probe, fixture)

    def test_topk_respects_all_order_keys_and_offset(self):
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(
                session,
                "CREATE TABLE ordered (id BIGINT PRIMARY KEY, category VARCHAR(2), "
                "score BIGINT, v VECTOR(2))",
                ["OK"],
            )
            self.expect_sql(
                session,
                "INSERT INTO ordered VALUES "
                "(1,'a',10,'[0,0]'),(2,'b',30,'[0,0]'),"
                "(3,'b',20,'[0,0]'),(4,'a',40,'[0,0]'),(5,'z',100,'[1,0]')",
                ["OK 5"],
            )
            self.expect_sql(session, "CREATE VECTOR INDEX ox ON ordered(v)", ["OK"])
            orders = (
                ("d,id DESC", [4, 3, 2, 1, 5]),
                ("d,category DESC,score ASC", [3, 2, 1, 4, 5]),
                ("d,category DESC", [2, 3, 1, 4, 5]),
                ("d DESC,id DESC", [5, 4, 3, 2, 1]),
            )
            windows = (
                ("", 0, 5),
                (" LIMIT 1", 0, 1),
                (" LIMIT 2 OFFSET 1", 1, 3),
                (" LIMIT 1,2", 1, 3),
                (" LIMIT 20 OFFSET 3", 3, 5),
                (" LIMIT 0", 0, 0),
            )
            for mode in ("exact", "ann"):
                self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
                for order, primary_keys in orders:
                    for suffix, begin, end in windows:
                        with self.subTest(mode=mode, order=order, window=suffix):
                            sql = (
                                "SELECT id,L2_DISTANCE(v,'[0,0]') AS d "
                                f"FROM ordered ORDER BY {order}{suffix}"
                            )
                            expected = [
                                (primary, 1.0 if primary == 5 else 0.0)
                                for primary in primary_keys[begin:end]
                            ]
                            self.expect_distances(session, sql, expected)

    def test_ordinary_aliases_keep_the_source_column(self):
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(
                session,
                "CREATE TABLE aliases (id BIGINT PRIMARY KEY, x BIGINT, label VARCHAR(8))",
                ["OK"],
            )
            self.expect_sql(
                session, "INSERT INTO aliases VALUES (2,7,'two'),(1,99,'one')", ["OK 2"]
            )
            for sql, expected in (
                ("SELECT id AS renamed FROM aliases ORDER BY id", ["1", "2"]),
                ("SELECT id AS x FROM aliases ORDER BY id", ["1", "2"]),
                ("SELECT id AS x,x AS id FROM aliases ORDER BY x", ["1\t99", "2\t7"]),
                (
                    "SELECT x AS renamed FROM aliases ORDER BY renamed DESC LIMIT 1 OFFSET 1",
                    ["7"],
                ),
                ("SELECT label AS x FROM aliases ORDER BY id", ["one", "two"]),
            ):
                with self.subTest(sql=sql):
                    self.expect_sql(session, sql, expected)

    def create_predicates(self, session: Session, table: str):
        self.expect_sql(
            session,
            f"CREATE TABLE {table} (id BIGINT PRIMARY KEY, s VARCHAR(1), n BIGINT)",
            ["OK"],
        )
        self.expect_sql(
            session, f"INSERT INTO {table} VALUES (1,'b',0),(2,'z',0)", ["OK 2"]
        )

    def test_where_literals_can_exceed_varchar_length(self):
        with open_session(self.executable, self.data_dir) as session:
            self.create_predicates(session, "predicates")
            self.expect_sql(
                session, "INSERT INTO predicates VALUES (3,'甲',0),(4,'乙',0)", ["OK 2"]
            )
            for predicate, expected in (
                ("s < 'zz'", ["1", "2"]),
                ("s <= 'beyond'", ["1"]),
                ("s = 'long'", []),
                ("s > 'zz'", ["3", "4"]),
                ("s < '甲乙'", ["1", "2", "3", "4"]),
                ("s >= 'zz' AND id <= 3", ["3"]),
            ):
                with self.subTest(predicate=predicate):
                    self.expect_sql(
                        session, f"SELECT id FROM predicates WHERE {predicate} ORDER BY id", expected
                    )

    def test_long_where_literals_in_update_and_delete(self):
        with open_session(self.executable, self.data_dir) as session:
            cases = (
                ("UPDATE {table} SET n=7 WHERE s='long'", "OK 0", ["1\tb\t0", "2\tz\t0"]),
                ("UPDATE {table} SET s='x' WHERE s<'zz'", "OK 2", ["1\tx\t0", "2\tx\t0"]),
                ("DELETE FROM {table} WHERE s='long'", "OK 0", ["1\tb\t0", "2\tz\t0"]),
                ("DELETE FROM {table} WHERE s<'zz'", "OK 2", []),
            )
            for index, (template, status, rows) in enumerate(cases):
                with self.subTest(statement=template):
                    table = f"predicates_{index}"
                    self.create_predicates(session, table)
                    self.expect_sql(session, template.format(table=table), [status])
                    self.expect_sql(session, f"SELECT id,s,n FROM {table} ORDER BY id", rows)

    def test_varchar_write_lengths_remain_enforced(self):
        with open_session(self.executable, self.data_dir) as session:
            self.create_predicates(session, "predicates")
            for sql in (
                "INSERT INTO predicates VALUES (3,'long',0)",
                "UPDATE predicates SET s='long' WHERE id=1",
            ):
                with self.subTest(sql=sql):
                    self.expect_error(session, sql)
                    self.expect_sql(
                        session,
                        "SELECT id,s,n FROM predicates ORDER BY id",
                        ["1\tb\t0", "2\tz\t0"],
                    )

    def test_query_edges_invalid_create_is_atomic(self):
        definitions = (
            "id BIGINT PRIMARY KEY, ID BIGINT",
            "id BIGINT PRIMARY KEY, value BIGINT, VaLuE VARCHAR(1)",
            "id BIGINT PRIMARY KEY, other BIGINT PRIMARY KEY",
            "id BIGINT PRIMARY KEY, other BIGINT, PRIMARY KEY(other)",
            "id BIGINT, other BIGINT, PRIMARY KEY(id), PRIMARY KEY(other)",
            "id BIGINT, PRIMARY KEY(id), PRIMARY KEY(ID)",
            "id BIGINT PRIMARY KEY PRIMARY KEY",
        )
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(session, "CREATE TABLE guard (id BIGINT PRIMARY KEY)", ["OK"])
            self.expect_sql(session, "INSERT INTO guard VALUES (17)", ["OK 1"])
            for index, definition in enumerate(definitions):
                with self.subTest(definition=definition):
                    table = f"bad_schema_{index}"
                    self.expect_error(session, f"CREATE TABLE {table} ({definition})")
                    self.expect_error(session, f"SELECT id FROM {table}")
                    self.expect_sql(session, "SELECT id FROM guard ORDER BY id", ["17"])
                    # A rejected CREATE must not reserve the name or leave a table.
                    self.expect_sql(
                        session, f"CREATE TABLE {table} (id BIGINT PRIMARY KEY)", ["OK"]
                    )
                    self.expect_sql(session, f"INSERT INTO {table} VALUES (1)", ["OK 1"])
                    self.expect_sql(session, f"SELECT id FROM {table}", ["1"])

    def test_query_edges_null_writes_are_statement_atomic(self):
        baseline = ["1\tbase\t10\t[0,0]"]
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(
                session,
                "CREATE TABLE nulls (id BIGINT PRIMARY KEY, s VARCHAR(8), n BIGINT, v VECTOR(2))",
                ["OK"],
            )
            self.expect_sql(
                session, "INSERT INTO nulls VALUES (1,'base',10,'[0,0]')", ["OK 1"]
            )
            invalid_rows = (
                "(NULL,'new',20,'[1,0]')",
                "(3,nUlL,20,'[1,0]')",
                "(3,'new',NULL,'[1,0]')",
                "(3,'new',20,NULL)",
            )
            invalid_sql = [f"INSERT INTO nulls VALUES {row}" for row in invalid_rows]
            invalid_sql += [
                f"INSERT INTO nulls VALUES (2,'valid',20,'[1,0]'),{row}"
                for row in invalid_rows
            ]
            invalid_sql += [
                "UPDATE nulls SET id=NULL WHERE id=1",
                "UPDATE nulls SET s=NULL WHERE id=1",
                "UPDATE nulls SET n=NULL WHERE id=1",
                "UPDATE nulls SET v=NULL WHERE id=1",
                "UPDATE nulls SET n=77,s=NULL WHERE id=1",
                "UPDATE nulls SET s=NULL WHERE id=99",
            ]
            for sql in invalid_sql:
                with self.subTest(sql=sql):
                    self.expect_error(session, sql)
                    self.expect_sql(session, "SELECT * FROM nulls ORDER BY id", baseline)
            self.expect_sql(session, "BEGIN", ["OK"])
            self.expect_sql(
                session, "INSERT INTO nulls VALUES (2,'prior',20,'[1,0]')", ["OK 1"]
            )
            self.expect_error(
                session, "INSERT INTO nulls VALUES (3,'valid',30,'[2,0]'),(4,NULL,40,'[3,0]')"
            )
            committed = baseline + ["2\tprior\t20\t[1,0]"]
            self.expect_sql(session, "SELECT * FROM nulls ORDER BY id", committed)
            self.expect_sql(session, "COMMIT", ["OK"])
            # Quoted NULL is ordinary text, not a SQL NULL value.
            self.expect_sql(
                session, "INSERT INTO nulls VALUES (5,'NULL',50,'[4,0]')", ["OK 1"]
            )
            committed += ["5\tNULL\t50\t[4,0]"]
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(session, "SELECT * FROM nulls ORDER BY id", committed)

    def create_query_vectors(self, session: Session):
        rows = [
            (5, "a", 0, [2, 0]),
            (3, "a", 1, [0, 1]),
            (1, "a", 3, [1, 0]),
            (4, "b", 4, [0, 0]),
            (2, "b", 2, [-1, 0]),
            (6, "b", 1, [0, -1]),
        ]
        self.expect_sql(
            session,
            "CREATE TABLE v (id BIGINT PRIMARY KEY, category VARCHAR(1), score BIGINT, v VECTOR(2))",
            ["OK"],
        )
        values = ",".join(
            f"({primary},'{category}',{score},'{vector_sql(vector)}')"
            for primary, category, score, vector in rows
        )
        self.expect_sql(session, f"INSERT INTO v VALUES {values}", ["OK 6"])
        self.expect_sql(session, "CREATE VECTOR INDEX qx ON v(v)", ["OK"])
        return rows

    def query_vector_output(self, rows, projection: str) -> list[str]:
        if projection == "id":
            return [str(row[0]) for row in rows]
        return [
            f"{primary}\t{category}\t{score}\t{vector_sql(vector)}"
            for primary, category, score, vector in rows
        ]

    def test_query_edges_unprojected_distance_orders(self):
        origin, east, north = [0.0, 0.0], [1.0, 0.0], [0.0, 1.0]
        first = "L2_DISTANCE(v,'[0,0]')"
        second = "L2_DISTANCE(v,'[1,0]')"
        third = "L2_DISTANCE(v,'[0,1]')"
        orders = (
            (first, lambda row: (reference_distance(row[3], origin), row[0])),
            (first + " DESC", lambda row: (-reference_distance(row[3], origin), row[0])),
            (
                first + "," + second,
                lambda row: (
                    reference_distance(row[3], origin), reference_distance(row[3], east), row[0]
                ),
            ),
            (
                first + "," + second + "," + third + " DESC,id DESC",
                lambda row: (
                    reference_distance(row[3], origin), reference_distance(row[3], east),
                    -reference_distance(row[3], north), -row[0]
                ),
            ),
            (first + "," + first + " DESC,id DESC",
             lambda row: (reference_distance(row[3], origin), -row[0])),
        )
        windows = (
            ("", 0, 6), (" LIMIT 0", 0, 0), (" LIMIT 1", 0, 1),
            (" LIMIT 3 OFFSET 1", 1, 4), (" LIMIT 1,3", 1, 4),
        )
        with open_session(self.executable, self.data_dir) as session:
            rows = self.create_query_vectors(session)
            for mode in ("exact", "ann"):
                self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
                for order, key in orders:
                    ordered = sorted(rows, key=key)
                    for projection in ("id", "*"):
                        for suffix, begin, end in windows:
                            with self.subTest(mode=mode, order=order, projection=projection, window=suffix):
                                self.expect_sql(
                                    session, f"SELECT {projection} FROM v ORDER BY {order}{suffix}",
                                    self.query_vector_output(ordered[begin:end], projection),
                                )

    def test_query_edges_maximum_limit_and_offset(self):
        maximum = 9223372036854775807
        distance = "L2_DISTANCE(v,'[0,0]')"
        orders = (
            ("id", lambda row: row[0]),
            ("score DESC,id", lambda row: (-row[2], row[0])),
            (distance, lambda row: (reference_distance(row[3], [0.0, 0.0]), row[0])),
            (distance + ",score DESC,id",
             lambda row: (reference_distance(row[3], [0.0, 0.0]), -row[2], row[0])),
        )
        windows = (
            (f" LIMIT {maximum} OFFSET 1", 1),
            (f" LIMIT 1,{maximum}", 1),
            (f" LIMIT {maximum} OFFSET {maximum}", 6),
            (f" LIMIT 2 OFFSET {maximum}", 6),
        )
        with open_session(self.executable, self.data_dir) as session:
            rows = self.create_query_vectors(session)
            for mode in ("exact", "ann"):
                self.expect_sql(session, f"SET vector_mode={mode}", ["OK"])
                for order, key in orders:
                    ordered = sorted(rows, key=key)
                    for projection in ("id", "*"):
                        for suffix, begin in windows:
                            with self.subTest(mode=mode, order=order, projection=projection, window=suffix):
                                self.expect_sql(
                                    session, f"SELECT {projection} FROM v ORDER BY {order}{suffix}",
                                    self.query_vector_output(ordered[begin:], projection),
                                )

    def test_query_edges_vector_order_is_always_rejected(self):
        with open_session(self.executable, self.data_dir) as session:
            self.expect_sql(
                session, "CREATE TABLE v (id BIGINT PRIMARY KEY, v VECTOR(2))", ["OK"]
            )
            for populated in (False, True):
                if populated:
                    self.expect_sql(session, "INSERT INTO v VALUES (1,'[0,0]')", ["OK 1"])
                for sql in (
                    "SELECT id FROM v ORDER BY v",
                    "SELECT * FROM v ORDER BY v DESC LIMIT 1",
                    "SELECT id FROM v ORDER BY v LIMIT 0",
                    "SELECT v AS embedding FROM v ORDER BY embedding LIMIT 0",
                    "SELECT id FROM v WHERE id=99 ORDER BY v LIMIT 1",
                    "SELECT id FROM v ORDER BY id,v LIMIT 0",
                    "SELECT id FROM v ORDER BY L2_DISTANCE(v,'[0,0]'),v LIMIT 0",
                ):
                    with self.subTest(populated=populated, sql=sql):
                        self.expect_error(session, sql)
                        self.expect_sql(
                            session, "SELECT id FROM v ORDER BY id", ["1"] if populated else []
                        )


CASE_METHODS = {
    "recovery": ("test_recovery_does_not_reuse_cross_table_txid",),
    "distance": (
        "test_large_finite_distances",
        "test_tiny_distances_preserve_exact_topk",
        "test_near_distances_are_not_false_ties",
    ),
    "mixed_scale": (
        "test_mixed_scale_16d_order_is_kernel_independent",
        "test_exact_order_when_double_and_fsum_tie",
        "test_exact_key_maximum_subnormal_and_cancellation",
    ),
    "ordering": ("test_topk_respects_all_order_keys_and_offset",),
    "aliases": ("test_ordinary_aliases_keep_the_source_column",),
    "predicates": (
        "test_where_literals_can_exceed_varchar_length",
        "test_long_where_literals_in_update_and_delete",
        "test_varchar_write_lengths_remain_enforced",
    ),
    "query_edges": (
        "test_query_edges_invalid_create_is_atomic",
        "test_query_edges_null_writes_are_statement_atomic",
        "test_query_edges_unprojected_distance_orders",
        "test_query_edges_maximum_limit_and_offset",
        "test_query_edges_vector_order_is_always_rejected",
    ),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument(
        "--case",
        action="append",
        choices=tuple(CASE_METHODS),
        help="run only this group; may be repeated (default: all groups)",
    )
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"database executable does not exist: {executable}")
    HiddenEdges.executable = executable
    groups = list(dict.fromkeys(args.case or CASE_METHODS))
    suite = unittest.TestSuite(
        HiddenEdges(method) for group in groups for method in CASE_METHODS[group]
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
