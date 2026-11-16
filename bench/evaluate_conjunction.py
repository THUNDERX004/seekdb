#!/usr/bin/env python3
"""Serial exact-query comparison with an independent integer-distance oracle.

Local evidence only, never an official score. Uses the JSON protocol and
existing bounded pipe client. Each binary runs alone in a fresh directory;
three paired rounds alternate execution order. All timed responses are checked.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import math
from pathlib import Path
import platform
import random
import statistics
import tempfile
import time

from evaluate import Client, check_success, fingerprint, sql_bytes


def prepare(rows, dimension, unique_probes, seed):
    rng = random.Random(seed)
    primaries = list(range(1, rows + 1))
    rng.shuffle(primaries)
    vectors = [tuple(rng.randrange(256) for _ in range(dimension))
               for _ in range(rows)]
    probes = [tuple(rng.randrange(256) for _ in range(dimension))
              for _ in range(unique_probes)]

    def literal(vector):
        # Denominator 256 is exact in binary32; squared distances below use
        # integer arithmetic before rescaling, independent of database kernels.
        return "[" + ",".join(str(value / 256) for value in vector) + "]"

    categories = [f"c{i % 4}" for i in range(rows)]
    scores = [(i // 4) % 100 for i in range(rows)]
    inserts = []
    for start in range(0, rows, 100):
        values = [f"({primaries[i]},'all','{categories[i]}',{scores[i]},"
                  f"'{literal(vectors[i])}')"
                  for i in range(start, min(start + 100, rows))]
        inserts.append(sql_bytes("INSERT INTO bench VALUES " + ",".join(values)))
    cases = (
        ("unfiltered", "", lambda i: True),
        ("single_equality", "cat='c1'", lambda i: categories[i] == "c1"),
        ("conjunction_1pct", "common='all' AND score<1", lambda i: scores[i] < 1),
        ("conjunction_10pct", "score>=10 AND common='all' AND score<20",
         lambda i: 10 <= scores[i] < 20),
        ("conjunction_50pct", "common='all' AND score>=50", lambda i: scores[i] >= 50),
        ("selective_bucket", "cat='c1' AND score<50",
         lambda i: categories[i] == "c1" and scores[i] < 50),
    )
    groups = {}
    for name, condition, predicate in cases:
        selected = [i for i in range(rows) if predicate(i)]
        queries = []
        for probe in probes:
            ranked = heapq.nsmallest(10, (
                (sum((a - b) ** 2 for a, b in zip(vectors[i], probe)), primaries[i])
                for i in selected))
            truth = [(primary, math.sqrt(squared) / 256)
                     for squared, primary in ranked]
            sql = (f"SELECT id,L2_DISTANCE(v,'{literal(probe)}') AS d FROM bench "
                   + (f"WHERE {condition} " if condition else "")
                   + "ORDER BY d LIMIT 10")
            queries.append((sql_bytes(sql), truth))
        groups[name] = {"candidate_rows": len(selected), "queries": queries}
    return inserts, groups


def validate(output, truth):
    if len(output) != len(truth):
        raise AssertionError(f"row count differs: {len(output)} != {len(truth)}")
    for line, (primary, distance) in zip(output, truth):
        fields = line.split(b"\t")
        if (len(fields) != 2 or int(fields[0]) != primary or
                not math.isclose(float(fields[1]), distance, rel_tol=1e-5, abs_tol=1e-5)):
            raise AssertionError(f"exact query differs: {line!r} != {(primary, distance)!r}")


def run(binary, directory, args, inserts, groups):
    result = {}
    with Client(binary, directory, args.timeout, protocol="json") as client:
        check_success(client.execute(sql_bytes(
            "CREATE TABLE bench(id BIGINT PRIMARY KEY,common VARCHAR(16),"
            f"cat VARCHAR(16),score BIGINT,v VECTOR({args.dim}))")), "create")
        for statement in inserts:
            check_success(client.execute(statement), "insert")
        for name, group in groups.items():
            queries = group["queries"]
            start = time.perf_counter_ns()
            first = client.execute(queries[0][0])
            cold_ns = time.perf_counter_ns() - start
            validate(first, queries[0][1])
            # A complete warm cycle gives every prepared probe the same state.
            for statement, truth in queries:
                validate(client.execute(statement), truth)
            outputs = []
            elapsed_ns = 0
            for index in range(args.queries):
                statement, truth = queries[index % len(queries)]
                start = time.perf_counter_ns()
                output = client.execute(statement)
                elapsed_ns += time.perf_counter_ns() - start
                outputs.append((output, truth))
            for output, truth in outputs:
                validate(output, truth)
            result[name] = {"first_query_ns": cold_ns,
                            "warm_query_seconds": elapsed_ns / 1e9,
                            "queries_checked": args.queries + len(queries) + 1,
                            "matching_rows": group["candidate_rows"]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    for name, default in (("rows", 10000), ("dim", 128), ("runs", 3),
                          ("queries", 320), ("unique-probes", 16), ("seed", 20261116)):
        parser.add_argument(f"--{name}", type=int, default=default)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    for name in ("rows", "dim", "runs", "queries", "unique_probes", "timeout"):
        if getattr(args, name) <= 0:
            parser.error(f"{name} must be positive")
    binaries = {name: getattr(args, name).resolve(strict=True)
                for name in ("baseline", "candidate")}
    report_path = args.report.resolve()
    if report_path in (*binaries.values(), Path(__file__).resolve()):
        parser.error("report path must not overwrite code or a binary")
    print("Preparing integer-distance oracle", flush=True)
    inserts, groups = prepare(args.rows, args.dim, args.unique_probes, args.seed)
    digest = hashlib.sha256()
    for statement in [*inserts, *(sql for group in groups.values()
                                 for sql, _ in group["queries"])]:
        digest.update(len(statement).to_bytes(8, "little"))
        digest.update(statement)
    report = {"schema": "exact-conjunction-local-v1", "official_score": None,
              "environment": platform.platform(), "parameters": {
                  key: value for key, value in vars(args).items() if not isinstance(value, Path)},
              "binaries": {name: fingerprint(path) for name, path in binaries.items()},
              "sql_sha256": digest.hexdigest(), "runs": [],
              "method": "serial, fresh process/data per run, alternating order; JSON exact; "
                        "first query reported separately; timed responses all checked; "
                        "integer squared-distance plus primary-key oracle"}
    for index in range(args.runs):
        order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        results = {}
        for label in order:
            print(f"Round {index + 1}/{args.runs}: {label}", flush=True)
            with tempfile.TemporaryDirectory(prefix="mvd-conjunction-") as temporary:
                results[label] = run(binaries[label], Path(temporary), args, inserts, groups)
        report["runs"].append({"order": order, "results": results})
    report["summary"] = {
        name: {"median_speedup": statistics.median(
            run["results"]["baseline"][name]["warm_query_seconds"] /
            run["results"]["candidate"][name]["warm_query_seconds"]
            for run in report["runs"])} for name in groups}
    report["status"] = "passed"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
