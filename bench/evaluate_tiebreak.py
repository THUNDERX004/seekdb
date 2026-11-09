#!/usr/bin/env python3
"""Supplemental local ORDER BY distance,primary-key performance comparison.

Uses the unchanged local evaluator's client, workload, oracle, persistence
checks and timers. This is not a replacement official workload or wire driver.
Example: python -B bench/evaluate_tiebreak.py --baseline old.exe
         --candidate new.exe --runs 5 --report tie-comparison.json
"""
from __future__ import annotations

import hashlib
import json
import sys
from datetime import datetime, timezone

import evaluate as evaluation


def prepare_workload(args, seed):
    workload = evaluation.prepare_workload(args, seed)
    for query in workload.queries:
        original = b"ORDER BY d LIMIT "
        if query.sql.count(original) != 1:
            raise ValueError("unexpected source query; cannot add primary-key order")
        query.sql = query.sql.replace(original, b"ORDER BY d,id ASC LIMIT ", 1)
    digest = hashlib.sha256()
    for sql in (workload.create_sql, *(sql for sql, _ in workload.inserts),
                *(query.sql for query in workload.queries),
                *(sql for sql, _, _ in workload.persistence_pages),
                *workload.persistence_edges):
        digest.update(len(sql).to_bytes(8, "little"))
        digest.update(sql)
    workload.sql_sha256 = digest.hexdigest()
    return workload


def main(argv=None):
    args = evaluation.parse_args(argv)
    if args.report is None:
        raise SystemExit("--report is required for this supplemental comparison")
    if args.report == evaluation.Path(__file__).resolve():
        raise SystemExit("report must not overwrite this script")
    report = {
        "schema_version": "minivectordb-supplemental-tiebreak-v1",
        "status": "running",
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "local supplemental comparison only; not official score/rank",
        "order_by": "d,id ASC",
        "official_cgroup_metrics_collected": False,
        "parameters": {name: getattr(args, name) for name in
                       ("runs", "rows", "dim", "queries", "batch", "k", "seed", "timeout")},
        "MVD_CACHE_LIMIT_MB": evaluation.os.environ.get("MVD_CACHE_LIMIT_MB"),
        "methodology": "unchanged evaluate.py request/response timers, float64/PK "
                       "oracle for every query, all-column reload checks, "
                       "alternating serial order; Windows counters are not disk/cgroup metrics",
        "binaries": {}, "runs": [],
    }
    code = 0
    try:
        if evaluation.np is None:
            raise RuntimeError("NumPy is required")
        report["binaries"] = {label: evaluation.fingerprint(getattr(args, label))
                              for label in ("baseline", "candidate")}
        for index in range(args.runs):
            seed = args.seed + index
            workload = prepare_workload(args, seed)
            evaluation.prepare_oracle(workload, args.k)
            order = ["baseline", "candidate"] if index % 2 == 0 else ["candidate", "baseline"]
            run = {"round": index + 1, "seed": seed, "order": order,
                   "sql_sha256": workload.sql_sha256, "results": {}}
            report["runs"].append(run)
            for label in order:
                print(f"Round {index + 1}/{args.runs}: {label}", file=sys.stderr, flush=True)
                result = run["results"][label] = {}
                evaluation.evaluate_binary(getattr(args, label), workload, args.timeout, result)
        report["binary_sha256_after"] = {
            label: evaluation.fingerprint(getattr(args, label))["sha256"]
            for label in ("baseline", "candidate")}
        if any(report["binary_sha256_after"][label] != report["binaries"][label]["sha256"]
               for label in ("baseline", "candidate")):
            raise RuntimeError("binary changed during supplemental comparison")
        report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
        print(report["error"], file=sys.stderr, flush=True)
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    report["summary"] = evaluation.summarize(report["runs"])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "summary": report["summary"],
                      "report": str(args.report)}, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
