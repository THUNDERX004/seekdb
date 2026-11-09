#!/usr/bin/env python3
"""Small single-client benchmark matching the published contest shape."""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None


class Client:
    def __init__(self, executable: Path, data_dir: Path):
        self.process = subprocess.Popen(
            [
                str(executable),
                f"--data-dir={data_dir}",
                "--ready-marker=__END__",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

    def execute(self, sql: str) -> list[str]:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(sql.rstrip().rstrip(";") + ";\n")
        self.process.stdin.flush()
        lines: list[str] = []
        while True:
            line = self.process.stdout.readline()
            if line == "":
                stderr = "" if self.process.stderr is None else self.process.stderr.read()
                raise RuntimeError(
                    f"database exited {self.process.poll()} while running SQL: {stderr}"
                )
            line = line.rstrip("\r\n")
            if line == "__END__":
                for output in lines:
                    if output.startswith("ERROR "):
                        raise RuntimeError(output)
                return lines
            lines.append(line)

    def close(self):
        if self.process.stdin is not None:
            self.process.stdin.close()
        self.process.wait(timeout=30)
        if self.process.returncode != 0:
            stderr = "" if self.process.stderr is None else self.process.stderr.read()
            raise RuntimeError(f"database exited {self.process.returncode}: {stderr}")


def vector_literal(values: list[float]) -> str:
    return "[" + ",".join(f"{value:.6f}" for value in values) + "]"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--rows", type=int, default=10_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1_000)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    vectors = [
        [rng.random() for _ in range(args.dim)] for _ in range(args.rows)
    ]
    categories = [f"c{index % 10}" for index in range(args.rows)]
    data_dir = Path(tempfile.mkdtemp(prefix="mvd-bench-"))
    client = Client(args.executable.resolve(), data_dir)
    try:
        client.execute(
            "CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(16), "
            f"payload VARCHAR(64), emb VECTOR({args.dim}))"
        )

        started = time.perf_counter()
        for batch_start in range(0, args.rows, args.batch):
            rows = []
            for index in range(batch_start, min(args.rows, batch_start + args.batch)):
                rows.append(
                    f"({index + 1},'{categories[index]}','row{index}',"
                    f"'{vector_literal(vectors[index])}')"
                )
            output = client.execute(
                "INSERT INTO bench VALUES " + ",".join(rows)
            )
            expected = f"OK {len(rows)}"
            if output != [expected]:
                raise RuntimeError(f"unexpected insert output: {output!r}")
        write_seconds = time.perf_counter() - started

        query_specs = []
        for index in range(args.queries):
            probe = [rng.random() for _ in range(args.dim)]
            filtered = index % 2 == 1
            category = categories[rng.randrange(args.rows)]
            sql = (
                "SELECT id,L2_DISTANCE(emb,'"
                + vector_literal(probe)
                + "') AS d FROM bench "
            )
            if filtered:
                sql += f"WHERE cat = '{category}' "
            sql += f"ORDER BY d LIMIT {args.k}"
            query_specs.append((sql, filtered, probe, category))

        started = time.perf_counter()
        result_count = 0
        queried_ids = []
        for sql, _, _, _ in query_specs:
            output = client.execute(sql)
            if len(output) != args.k:
                raise RuntimeError(f"query returned {len(output)} rows, expected {args.k}")
            result_count += len(output)
            queried_ids.append({int(line.split("\t", 1)[0]) for line in output})
        query_seconds = time.perf_counter() - started
        client.close()
    finally:
        try:
            client.close()
        except Exception:
            pass
        if os.name != "nt":
            shutil.rmtree(data_dir, ignore_errors=True)

    recall = None
    sample_queries = 25
    if np is not None and args.queries > 0:
        matrix = np.asarray(
            [np.round(np.asarray(row, dtype=np.float64), 6) for row in vectors],
            dtype=np.float32,
        )
        ids = np.arange(1, args.rows + 1, dtype=np.int64)
        category_array = np.asarray(categories)
        recalled = {False: 0, True: 0}
        totals = {False: 0, True: 0}
        sample_count = min(sample_queries, args.queries)
        sample_indices = np.linspace(
            0, args.queries - 1, sample_count, dtype=int
        ).tolist()
        for query_index in sample_indices:
            sql, filtered, probe, category = query_specs[query_index]
            del sql
            probe_array = np.asarray(
                np.round(np.asarray(probe, dtype=np.float64), 6),
                dtype=np.float32,
            )
            mask = (
                category_array == category
                if filtered
                else np.ones(args.rows, dtype=bool)
            )
            candidate_ids = ids[mask]
            candidate_vectors = matrix[mask]
            distances = np.sum(
                (candidate_vectors - probe_array) ** 2, axis=1
            )
            order = np.lexsort((candidate_ids, distances))[: args.k]
            truth = set(candidate_ids[order].tolist())
            recalled[filtered] += len(truth & queried_ids[query_index])
            totals[filtered] += len(truth)
        recall = {
            "unfiltered": (
                recalled[False] / totals[False] if totals[False] else None
            ),
            "filtered": recalled[True] / totals[True] if totals[True] else None,
            "sample_queries": sample_count,
        }

    report = {
        "rows": args.rows,
        "dim": args.dim,
        "queries": args.queries,
        "k": args.k,
        "write_seconds": round(write_seconds, 6),
        "query_seconds": round(query_seconds, 6),
        "rows_per_second": round(args.rows / write_seconds, 2),
        "queries_per_second": round(args.queries / query_seconds, 2),
        "query_rows": result_count,
        "recall": recall,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if args.report is not None:
        args.report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
