"""Independent exact-rank checks while disk codes are warm, stale or evicted."""
import argparse
from dataclasses import replace
import math
import os
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_cached_topk as cached
from test_exact_distance import exact_squared, float32, from_bits, vector_sql
from jsonl_client import JsonlClient


class DiskCodeBoundary(unittest.TestCase):
    executable = None
    setUp = cached.CachedTopKTests.setUp
    sql = cached.CachedTopKTests.sql
    query = cached.CachedTopKTests.query

    def open(self):
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": "1"}):
            client = JsonlClient([self.executable], self.data_dir, timeout=30)
        self.addCleanup(client.close)
        return client

    def create(self, client, dimension=127, count=2703, kind="random"):
        self.sql(client, "CREATE TABLE d(id BIGINT PRIMARY KEY,tag BIGINT,"
                 "category VARCHAR(16),payload VARCHAR(64),v VECTOR(" + str(dimension) + "))")
        rng = random.Random(20261116 + dimension)
        if kind == "random":
            distinct = [tuple(float32(rng.uniform(-1, 1)) for _ in range(dimension))
                        for _ in range(37)]
        elif kind == "shifted":
            distinct = [tuple(float32(1000 + rng.uniform(-1, 1)) for _ in range(dimension))
                        for _ in range(37)]
        elif kind == "subnormal":
            distinct = [tuple(from_bits(1 + (index * 7 + axis) % 97)
                              for axis in range(dimension)) for index in range(17)]
        elif kind == "swamped":
            huge = math.ldexp(1.0, 100)
            distinct = [tuple([huge, float(index - 8)] + [0.] * (dimension - 2))
                        for index in range(17)]
        else:
            raise AssertionError(kind)
        self.rows = [cached.Row(index * 5 % 10007 - 5000, index % 7,
                               "red" if index % 2 else "blue", f"r{index}",
                               distinct[index % len(distinct)]) for index in range(count)]
        for start in range(0, len(self.rows), 150):
            cached.CachedTopKTests.insert(self, client, "d", self.rows[start:start + 150])
        return distinct

    def check(self, client, probe, limit=7, offset=0, where="", predicate=None):
        distinct = {vector: exact_squared(vector, probe)
                    for vector in {row.vector for row in self.rows}}
        scores = {row.primary: distinct[row.vector] for row in self.rows}
        self.query(client, "d", probe, self.rows, scores, limit=limit, offset=offset,
                   where=where, predicate=predicate, include_vector=False)

    def test_warm_heap_groups_mutation_rollback_restart(self):
        client = self.open()
        vectors = self.create(client)
        for index, limit in enumerate((1, 2, 3, 4, 5, 7, 16, 255, 256)):
            self.check(client, vectors[(index * 5) % len(vectors)], limit=limit)
        self.check(client, vectors[4], limit=17, offset=13)
        self.check(client, [5.] * 127)
        self.check(client, vectors[4], where="tag=3", predicate=lambda row: row.tag == 3)
        self.check(client, vectors[7])
        self.sql(client, "BEGIN")
        victim = self.rows[17]
        updated = replace(victim, vector=vectors[2], payload="updated")
        self.sql(client, f"UPDATE d SET v={cached.quoted(vector_sql(updated.vector))},"
                 f"payload='updated' WHERE id={victim.primary}", 1)
        self.rows[17] = updated
        self.check(client, vectors[2])
        self.sql(client, "ROLLBACK")
        self.rows[17] = victim
        self.check(client, vectors[2])
        self.sql(client, f"UPDATE d SET v={cached.quoted(vector_sql(updated.vector))},"
                 f"payload='updated' WHERE id={victim.primary}", 1)
        self.rows[17] = updated
        self.check(client, vectors[3])
        removed = self.rows.pop(23)
        self.sql(client, f"DELETE FROM d WHERE id={removed.primary}", 1)
        self.check(client, vectors[7])
        replacement = replace(removed, vector=vectors[5])
        cached.CachedTopKTests.insert(self, client, "d", [replacement])
        self.rows.append(replacement)
        self.check(client, vectors[8])
        client.shutdown()
        client.close()
        client = self.open()
        self.check(client, vectors[12], limit=31, offset=4)
        # Three tables reduce the per-table budget below the complete codes.
        for name in ("s1", "s2", "s3"):
            self.sql(client, f"CREATE TABLE {name}(id BIGINT PRIMARY KEY)")
        self.check(client, vectors[17])

    def test_centered_tail_and_underflow(self):
        for dimension, kind in ((65, "shifted"), (128, "subnormal"), (17, "swamped")):
            with self.subTest(dimension=dimension, kind=kind):
                temporary = tempfile.TemporaryDirectory(prefix="mvd-disk-boundary-")
                self.addCleanup(temporary.cleanup)
                self.data_dir = Path(temporary.name)
                # Wider payload text forces even the 17D fixture off raw cache.
                client = self.open()
                count = 10000 if kind == "swamped" else 5000 if kind == "shifted" else 3000
                distinct = self.create(client, dimension=dimension, count=count, kind=kind)
                if kind == "swamped":
                    self.sql(client, "UPDATE d SET payload='" + "x" * 64 + "'")
                    self.rows = [replace(row, payload="x" * 64) for row in self.rows]
                probes = [distinct[0], distinct[7], tuple([0.] * dimension)]
                if kind == "swamped":
                    probes.append(tuple([-math.ldexp(1.0, 99)] + [0.] * (dimension - 1)))
                for probe in probes:
                    self.check(client, probe, limit=11, offset=3)
                client.shutdown()
                client.close()

    def test_changed_file_checks_even_rejected_rows(self):
        client = self.open()
        vectors = self.create(client)
        self.check(client, vectors[0])
        self.check(client, vectors[1])
        files = list(self.data_dir.glob("*.dat"))
        self.assertEqual(len(files), 1, list(self.data_dir.iterdir()))
        path = files[0]
        previous = path.stat()
        raw = bytearray(path.read_bytes())
        # Last record's last float remains structurally valid but its CRC fails.
        raw[-1] ^= 1
        path.write_bytes(raw)
        os.utime(path, ns=(previous.st_atime_ns, previous.st_mtime_ns + 1000000000))
        response = client.execute(
            "SELECT id,L2_DISTANCE(v," + cached.quoted(vector_sql(vectors[0]))
            + ") AS z FROM d ORDER BY z LIMIT 1")
        self.assertIs(response.get("ok"), False, response)
        self.assertEqual(response.get("error", {}).get("code"), "CORRUPT_DATA", response)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args, remaining = parser.parse_known_args()
    DiskCodeBoundary.executable = str(args.executable.resolve(strict=True))
    unittest.main(argv=[sys.argv[0], *remaining])
