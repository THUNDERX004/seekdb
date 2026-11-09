#!/usr/bin/env python3
"""Diagnostic-only query profile; not an official timing or CPU stack profile."""
from __future__ import annotations

import argparse
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from tempfile import TemporaryDirectory

import evaluate as evaluation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--kind", choices=("topk", "ordinary"), default="topk")
    for name, default in (("rows", 10000), ("dim", 128), ("queries", 1000),
                          ("batch", 100), ("k", 10), ("seed", 20261109)):
        parser.add_argument(f"--{name}", type=int, default=default)
    args = parser.parse_args()
    if any(getattr(args, name) <= 0 for name in
           ("rows", "dim", "queries", "batch", "k")):
        parser.error("workload dimensions must be positive")
    if evaluation.np is None:
        parser.error("NumPy is required")
    binary = args.executable.resolve(strict=True)
    before = evaluation.fingerprint(binary)
    workload = evaluation.prepare_workload(args, args.seed)
    if args.kind == "topk":
        evaluation.prepare_oracle(workload, args.k)
        statements = [query.sql for query in workload.queries]
    else:
        statements = [evaluation.sql_bytes(
            f"SELECT id,payload,emb FROM bench ORDER BY cat,payload LIMIT {args.k} OFFSET 7"
        )] * args.queries
    with TemporaryDirectory(prefix="mvd-profile-") as directory:
        outputs = []
        with evaluation.Client(binary, Path(directory), 60) as client:
            if client.execute(workload.create_sql) != [b"OK"]:
                raise AssertionError("CREATE failed")
            for sql, count in workload.inserts:
                if client.execute(sql) != [f"OK {count}".encode()]:
                    raise AssertionError("INSERT failed")
            durations = []
            for sql in statements:
                started = time.perf_counter_ns()
                output = client.execute(sql)
                durations.append(time.perf_counter_ns() - started)
                outputs.append(output)
        if args.kind == "topk":
            validation = evaluation.validate_queries(workload, outputs)
        else:
            ids = sorted(range(1, args.rows + 1), key=lambda primary: (
                workload.categories[primary - 1], f"row{primary - 1}", primary
            ))[7:7 + args.k]
            for output in outputs:
                if len(output) != len(ids):
                    raise AssertionError("ordinary selection count mismatch")
                for line, primary in zip(output, ids):
                    fields = line.decode("utf-8").split("\t")
                    if len(fields) != 3 or fields[:2] != [str(primary), f"row{primary - 1}"]:
                        raise AssertionError("ordinary selection ID/payload mismatch")
                    vector = evaluation.np.asarray(json.loads(fields[2]), dtype=evaluation.np.float32)
                    if not evaluation.np.array_equal(vector, workload.matrix[primary - 1]):
                        raise AssertionError("ordinary selection vector mismatch")
            validation = {"all_queries_checked": True, "all_columns_checked": True}
    raw_stderr = b"".join(client.stderr_tail).decode("utf-8", "replace")
    profiles = [json.loads(line[len("MVD_PROFILE "):])
                for line in raw_stderr.splitlines() if line.startswith("MVD_PROFILE ")]
    after = evaluation.fingerprint(binary)
    if before["sha256"] != after["sha256"]:
        raise AssertionError("binary changed during profiling")
    report = {
        "schema_version": "minivectordb-query-profile-v1",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "status": "passed", "binary": before,
        "parameters": {key: value for key, value in vars(args).items()
                       if key not in ("executable", "report")},
        "MVD_CACHE_LIMIT_MB": os.environ.get("MVD_CACHE_LIMIT_MB"),
        "timing_ns": durations, "query_seconds": sum(durations) / 1e9,
        "profile": profiles, "stderr": raw_stderr, "validation": validation,
        "limits": ["diagnostic build only, instrumentation changes timing",
                   "nested sampled scopes are not exclusive CPU stack samples",
                   "parse/execute also include setup inserts, SELECT/output scopes do not",
                   "deterministic row sampling may be biased; no hotspot percentage claim",
                   "ordinary workload is supplemental, not official benchmark"],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "query_seconds": report["query_seconds"],
                      "profile": profiles, "report": str(args.report)}))


if __name__ == "__main__":
    main()
