#!/usr/bin/env python3
"""Serial exact-query comparison with separate cold and warm query phases."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import time

import evaluate as evaluation


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measure(binary, data, workload, timeout):
    result = {}
    with evaluation.Client(binary, data, timeout, "json") as client:
        for phase in ("cold", "warm"):
            timings, outputs = [], []
            for query in workload.queries:
                start = time.perf_counter_ns()
                output = client.execute(query.sql)
                timings.append(time.perf_counter_ns() - start)
                outputs.append(output)
            result[phase] = {
                "seconds": sum(timings) / 1e9,
                "first_query_seconds": timings[0] / 1e9,
                "group_seconds": {
                    group: sum(value for query, value in zip(workload.queries, timings)
                               if query.group == group) / 1e9
                    for group in ("unfiltered", "filtered")
                },
                "validation": evaluation.validate_queries(workload, outputs),
            }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261810)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--aa-runs", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    for name in ("rows", "dim", "queries", "batch", "k", "runs"):
        if getattr(args, name) <= 0:
            parser.error(name + " must be positive")
    if args.queries < 2 or args.aa_runs < 0:
        parser.error("queries must be at least two and aa-runs nonnegative")
    root = Path(__file__).resolve().parents[1]
    source = [root / "src/minivectordb.cpp", *sorted((root / "src").glob("*.hpp"))]
    before = {str(path.relative_to(root)): fingerprint(path) for path in source}
    binaries = {label: getattr(args, label).resolve(strict=True)
                for label in ("baseline", "candidate")}
    binary_hashes = {label: fingerprint(path) for label, path in binaries.items()}
    workload = evaluation.prepare_workload(args, args.seed)
    evaluation.prepare_oracle(workload, args.k)
    report = {
        "status": "running",
        "scope": "serial JSON/exact cold and warm query phases; no write/memory score",
        "workload": {**{key: getattr(args, key) for key in
                         ("rows", "dim", "queries", "batch", "k", "seed")},
                     "sha256": workload.sql_sha256},
        "source": before, "binaries": binary_hashes, "runs": [], "aa_runs": [],
    }
    # A single baseline loader prepares immutable data. Every timed run opens a
    # fresh process, and its warm phase follows its own complete cold phase.
    # No database process or compilation runs concurrently with these queries.
    with tempfile.TemporaryDirectory(prefix="mvd-vector-phases-") as directory:
        data = Path(directory)
        with evaluation.Client(binaries["baseline"], data, args.timeout, "json") as loader:
            if loader.execute(workload.create_sql) != [b"OK"]:
                raise evaluation.EvaluationError("baseline loader CREATE failed")
            for sql, count in workload.inserts:
                if loader.execute(sql) != [f"OK {count}".encode()]:
                    raise evaluation.EvaluationError("baseline loader INSERT failed")
        for index in range(args.runs):
            order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
            run = {"index": index, "order": order, "results": {}}
            for label in order:
                run["results"][label] = measure(binaries[label], data, workload, args.timeout)
            report["runs"].append(run)
            print(json.dumps({"pair": index, "results": run["results"]}), flush=True)
        for index in range(args.aa_runs):
            first = measure(binaries["candidate"], data, workload, args.timeout)
            second = measure(binaries["candidate"], data, workload, args.timeout)
            report["aa_runs"].append({"index": index, "first": first, "second": second})
    report["ratios_candidate_over_baseline"] = {}
    for phase in ("cold", "warm"):
        ratios = [run["results"]["candidate"][phase]["seconds"] /
                  run["results"]["baseline"][phase]["seconds"] for run in report["runs"]]
        aa = [run["second"][phase]["seconds"] / run["first"][phase]["seconds"]
              for run in report["aa_runs"]]
        report["ratios_candidate_over_baseline"][phase] = {
            "paired": ratios, "median": statistics.median(ratios), "aa": aa,
        }
    after = {str(path.relative_to(root)): fingerprint(path) for path in source}
    if before != after or binary_hashes != {label: fingerprint(path) for label, path in binaries.items()}:
        raise evaluation.EvaluationError("source or binaries changed during serial measurement")
    report["status"] = "passed"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"],
                      "ratios": report["ratios_candidate_over_baseline"]}), flush=True)


if __name__ == "__main__":
    main()
