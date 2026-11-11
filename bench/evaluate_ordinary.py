#!/usr/bin/env python3
"""Serial paired ordinary ORDER BY/LIMIT comparison, never an official E score.

Example: python -B bench/evaluate_ordinary.py --baseline old.exe --candidate new.exe
         --runs 3 --seed 20261111 --report ordinary-comparison.json

Requires NumPy for the shared fixture and all-column persistence validator.
Only complete JSON pipe exchanges are query timed; setup and writes are separate.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import evaluate as evaluation


LABELS = ("baseline", "candidate")
# Group, projection, (sort column, descending) terms, category filter.
SPECS = (
    ("cat_payload", ("id", "payload"), (("cat", False), ("payload", False)), False),
    ("payload_desc_cat", ("payload", "id"), (("payload", True), ("cat", False)), False),
    ("filtered_payload_desc", ("id", "cat", "payload"), (("payload", True),), True),
    ("cat_desc_pk_ties", ("payload", "id", "cat"), (("cat", True),), False),
)
GROUPS = tuple(spec[0] for spec in SPECS)


@dataclass(frozen=True)
class Query:
    sql: bytes
    group: str
    columns: tuple[str, ...]
    category: str | None
    offset: int
    truth_ids: tuple[int, ...]
    truth_rows: tuple[tuple[str, ...], ...]


@dataclass(frozen=True)
class Workload:
    create_sql: bytes
    inserts: tuple[tuple[bytes, int], ...]
    queries: tuple[Query, ...]
    matrix: Any
    categories: tuple[str, ...]
    persistence_pages: tuple[tuple[bytes, int, int], ...]
    persistence_edges: tuple[bytes, ...]
    sql_sha256: str


def require_finite(value):
    if isinstance(value, float) and not math.isfinite(value):
        raise evaluation.EvaluationError("non-finite number in JSON")
    if isinstance(value, dict):
        for child in value.values():
            require_finite(child)
    elif isinstance(value, (list, tuple)):
        for child in value:
            require_finite(child)


class JsonClient(evaluation.Client):
    def _read_json(self):
        response = super()._read_json()
        require_finite(response)  # Also reject NaN/Infinity in unused response fields.
        return response


def prepare_workload(args) -> Workload:
    fixture_args = argparse.Namespace(**{**vars(args), "queries": 0})
    base = evaluation.prepare_workload(fixture_args, args.seed)
    records = tuple((index + 1, category, f"row{index}")
                    for index, category in enumerate(base.categories))
    slots = {"id": 0, "cat": 1, "payload": 2}
    rng = random.Random(args.seed)
    ordered_pools = {}
    queries = []
    for index in range(args.queries):
        group, columns, terms, filtered = SPECS[index % len(SPECS)]
        category = rng.choice(base.categories) if filtered else None
        key = (group, category)
        if key not in ordered_pools:
            # Stable Python sorts preserve ascending PK even for descending ties.
            ordered = sorted((row for row in records
                              if category is None or row[1] == category), key=lambda row: row[0])
            for column, descending in reversed(terms):
                ordered = sorted(ordered, key=lambda row, slot=slots[column]: row[slot],
                                 reverse=descending)
            ordered_pools[key] = tuple(ordered)
        ordered = ordered_pools[key]
        # All offsets are nonzero and <= K; tiny category pools may yield no rows.
        offset = rng.randint(1, min(args.k, max(1, len(ordered) - 1)))
        selected = ordered[offset:offset + args.k]
        sql = f"SELECT {','.join(columns)} FROM bench "
        if category is not None:
            sql += f"WHERE cat = '{category}' "
        sql += "ORDER BY " + ",".join(
            column + (" DESC" if descending else "") for column, descending in terms
        ) + f" LIMIT {args.k} OFFSET {offset}"
        queries.append(Query(
            evaluation.sql_bytes(sql), group, columns, category, offset,
            tuple(row[0] for row in selected),
            tuple(tuple(str(row[slots[column]]) for column in columns) for row in selected),
        ))
    digest = hashlib.sha256()
    for sql in (base.create_sql, *(sql for sql, _ in base.inserts),
                *(query.sql for query in queries),
                *(sql for sql, _, _ in base.persistence_pages), *base.persistence_edges):
        digest.update(len(sql).to_bytes(8, "little"))
        digest.update(sql)
    base.matrix.setflags(write=False)
    return Workload(base.create_sql, tuple(base.inserts), tuple(queries), base.matrix,
                    tuple(base.categories), tuple(base.persistence_pages),
                    tuple(base.persistence_edges), digest.hexdigest())


def validate_queries(workload: Workload, outputs: list[list[bytes]]) -> dict:
    if len(outputs) != len(workload.queries):
        raise evaluation.EvaluationError("not every ordinary query produced output")
    groups = {group: {"queries_checked": 0, "rows_checked": 0} for group in GROUPS}
    for index, (query, output) in enumerate(zip(workload.queries, outputs)):
        context = f"query {index} ({query.group})"
        evaluation.check_success(output, context)
        rows = tuple(tuple(line.decode("utf-8", "strict").split("\t")) for line in output)
        if len(rows) != len(query.truth_rows) or any(len(row) != len(query.columns) for row in rows):
            raise evaluation.EvaluationError(f"{context}: row/column count differs")
        try:
            ids = tuple(int(row[query.columns.index("id")]) for row in rows)
        except ValueError as error:
            raise evaluation.EvaluationError(f"{context}: invalid primary key") from error
        if ids != query.truth_ids:
            raise evaluation.EvaluationError(f"{context}: exact ordered IDs differ")
        if rows != query.truth_rows:
            raise evaluation.EvaluationError(f"{context}: returned fields differ from dataset")
        groups[query.group]["queries_checked"] += 1
        groups[query.group]["rows_checked"] += len(rows)
    return {"all_queries_checked": True, "all_columns_checked": True,
            "exact_ordered_ids": True, "implicit_primary_key_ascending": True,
            "groups": groups}


def elapsed_since(started: int) -> int:
    elapsed = time.perf_counter_ns() - started
    if elapsed <= 0:
        raise evaluation.EvaluationError("non-positive elapsed time")
    return elapsed


def exchange(client, sql):
    started = time.perf_counter_ns()
    output = client.execute(sql)
    return output, elapsed_since(started)


def evaluate_binary(executable: Path, workload: Workload, timeout: float, result: dict):
    result.update(status="running", protocol="json", write_batch_ns=[], queries_raw=[])
    with tempfile.TemporaryDirectory(prefix="mvd-ordinary-") as directory:
        data_dir = Path(directory)
        started = time.perf_counter_ns()
        with JsonClient(executable, data_dir, timeout, protocol="json") as client:
            result["pid"] = client.process.pid
            result["setup_raw_ns"] = {"startup": elapsed_since(started)}
            output, elapsed = exchange(client, workload.create_sql)
            result["setup_raw_ns"]["ddl"] = elapsed
            if output != [b"OK"]:
                raise evaluation.EvaluationError(f"CREATE failed: {output[:3]!r}")
            result["setup_seconds"] = sum(result["setup_raw_ns"].values()) / 1e9
            for sql, count in workload.inserts:
                output, elapsed = exchange(client, sql)
                result["write_batch_ns"].append(elapsed)
                if output != [f"OK {count}".encode()]:
                    raise evaluation.EvaluationError(f"INSERT acknowledgement differs: {output[:3]!r}")
            result["write_seconds"] = sum(result["write_batch_ns"]) / 1e9
            outputs = []
            for index, query in enumerate(workload.queries):
                output, elapsed = exchange(client, query.sql)
                result["queries_raw"].append({"index": index, "group": query.group,
                                              "elapsed_ns": elapsed, "rows": len(output)})
                outputs.append(output)
            result["query_seconds"] = sum(row["elapsed_ns"] for row in result["queries_raw"]) / 1e9
            result["query_group_seconds"] = {
                group: sum(row["elapsed_ns"] for row in result["queries_raw"]
                           if row["group"] == group) / 1e9 for group in GROUPS
            }
            started = time.perf_counter_ns()
        result["shutdown_ns"] = elapsed_since(started)
        started = time.perf_counter_ns()
        result["validation"] = validate_queries(workload, outputs)
        result["validation_ns"] = elapsed_since(started)
        started = time.perf_counter_ns()
        # Exactly one graceful restart per binary/run, with every stored field checked.
        with JsonClient(executable, data_dir, timeout, protocol="json") as recovered:
            result["restart_pid"] = recovered.process.pid
            result["restart_startup_ns"] = elapsed_since(started)
            started = time.perf_counter_ns()
            result["persistence"] = evaluation.validate_persistence(recovered, workload)
            result["persistence_ns"] = elapsed_since(started)
            started = time.perf_counter_ns()
        result["restart_shutdown_ns"] = elapsed_since(started)
    result["status"] = "passed"


def metrics(result: dict) -> dict:
    return {**{name: result[name] for name in ("setup_seconds", "write_seconds", "query_seconds")},
            **{f"{group}_query_seconds": result["query_group_seconds"][group] for group in GROUPS}}


def paired_ratios(results: dict) -> dict:
    baseline, candidate = (metrics(results[label]) for label in LABELS)
    if any(not math.isfinite(value) or value <= 0
           for values in (baseline, candidate) for value in values.values()):
        raise evaluation.EvaluationError("non-finite or non-positive timing")
    ratios = {name: baseline[name] / candidate[name] for name in baseline}
    require_finite(ratios)
    if any(value <= 0 for value in ratios.values()):
        raise evaluation.EvaluationError("non-positive paired ratio")
    return ratios


def summarize(runs: list[dict]) -> dict:
    pairs = [run for run in runs if "paired_baseline_over_candidate" in run]
    summary = {"complete_pairs": len(pairs)}
    if pairs:
        names = pairs[0]["paired_baseline_over_candidate"]
        for label in LABELS:
            summary[label] = {name: evaluation.statistics_for(
                [metrics(run["results"][label])[name] for run in pairs]) for name in names}
        summary["paired_speedup_baseline_over_candidate"] = {
            name: {**evaluation.statistics_for(values := [
                run["paired_baseline_over_candidate"][name] for run in pairs]),
                   "per_pair": values} for name in names
        }
    return summary


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for label in LABELS:
        parser.add_argument(f"--{label}", type=Path, required=True)
    for name, default in (("runs", 3), ("rows", 10000), ("dim", 128), ("queries", 1000),
                          ("batch", 100), ("k", 10), ("seed", 20261111)):
        parser.add_argument(f"--{name}", type=int, default=default)
    parser.add_argument("--timeout", type=float, default=60, help="finite seconds per startup/request")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv)
    if any(getattr(args, name) <= 0 for name in ("runs", "rows", "dim", "queries", "batch", "k")):
        parser.error("runs, rows, dim, queries, batch and k must be positive")
    if args.queries < len(SPECS):
        parser.error(f"--queries must be at least {len(SPECS)} to cover every ordinary group")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    for label in LABELS:
        path = getattr(args, label).resolve()
        if not path.is_file():
            parser.error(f"--{label} executable does not exist: {path}")
        setattr(args, label, path)
    args.report = args.report.resolve()
    if args.report in (args.baseline, args.candidate, Path(__file__).resolve(),
                       Path(evaluation.__file__).resolve()) or args.report.is_dir():
        parser.error("--report must be a file distinct from the binaries and evaluator sources")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    report = {
        "schema_version": "minivectordb-local-ordinary-v1", "status": "running",
        "started_at_utc": datetime.now(timezone.utc).isoformat(), "protocol": "json",
        "official_cgroup_metrics_collected": False, "official_E": None,
        "parameters": {name: getattr(args, name) for name in
                       ("runs", "rows", "dim", "queries", "batch", "k", "seed", "timeout")},
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "logical_cpus": os.cpu_count(), "python": sys.version,
                        "python_executable": sys.executable, "cwd": str(Path.cwd()),
                        "numpy": getattr(evaluation.np, "__version__", None),
                        "MVD_CACHE_LIMIT_MB": os.environ.get("MVD_CACHE_LIMIT_MB"),
                        "perf_counter": vars(time.get_clock_info("perf_counter"))},
        "methodology": {
            "scope": "local supplemental ordinary-query comparison, not official E, score or rank",
            "timing": "serial JSON request/response wall time including IPC and JSON parsing",
            "setup": "startup plus CREATE exchanges; INSERT exchanges reported separately",
            "query_excludes": ["workload/oracle preparation", "setup", "writes", "validation",
                               "shutdown", "restart", "all-column persistence reads"],
            "workload": "one deterministic frozen fixture reused for both binaries and all pairs",
            "order": "odd pairs baseline,candidate; even pairs candidate,baseline; never parallel",
            "oracle": "Python sorted over inserted dataset; stable keys after ascending PK; all fields",
            "cache": "fresh directory per binary/run; queries warm after inserts; no cold-cache claim",
            "limits": "single client, no cgroup resource metrics; fixed ASCII row payloads; host noise remains",
            "ratio": "median of within-pair baseline/candidate times; greater than one favors candidate",
        },
        "binaries_before": {}, "binaries_after": {}, "runs": [],
    }
    code = 0

    def fail(error):
        nonlocal code
        report["status"] = "failed"
        report.setdefault("errors", []).append(f"{type(error).__name__}: {error}")
        code = 130 if isinstance(error, KeyboardInterrupt) or code == 130 else 1
        print(report["errors"][-1], file=sys.stderr, flush=True)

    try:
        if evaluation.np is None:
            raise evaluation.EvaluationError("NumPy is required; no database processes started")
        for label in LABELS:
            report["binaries_before"][label] = evaluation.fingerprint(getattr(args, label))
        workload = prepare_workload(args)
        report["workload"] = {
            "sql_sha256": workload.sql_sha256,
            "insert_batch_rows": [count for _, count in workload.inserts],
            "queries": [{"index": index, "group": query.group, "columns": query.columns,
                         "category": query.category, "offset": query.offset,
                         "sql": query.sql.decode("utf-8").strip(), "expected_ids": query.truth_ids}
                        for index, query in enumerate(workload.queries)],
        }
        for index in range(args.runs):
            order = LABELS if index % 2 == 0 else tuple(reversed(LABELS))
            run = {"pair": index + 1, "seed": args.seed, "order": order,
                   "sql_sha256": workload.sql_sha256, "results": {}}
            report["runs"].append(run)
            for label in order:
                print(f"Pair {index + 1}/{args.runs}: {label}", file=sys.stderr, flush=True)
                result = run["results"][label] = {}
                try:
                    evaluate_binary(getattr(args, label), workload, args.timeout, result)
                except (Exception, KeyboardInterrupt) as error:
                    result.update(status="failed", error=f"{type(error).__name__}: {error}",
                                  error_notes=getattr(error, "__notes__", []))
                    raise
            run["paired_baseline_over_candidate"] = paired_ratios(run["results"])
        report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        fail(error)
    finally:
        # Preserve after fingerprints even when validation or an exchange failed.
        for label in LABELS:
            try:
                after = evaluation.fingerprint(getattr(args, label))
                report["binaries_after"][label] = after
                if label in report["binaries_before"] and after != report["binaries_before"][label]:
                    raise evaluation.EvaluationError(f"{label} binary changed during comparison")
            except (Exception, KeyboardInterrupt) as error:
                fail(error)
    report["summary"] = summarize(report["runs"])
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    try:
        require_finite(report)
        rendered = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered, encoding="utf-8")
    except (Exception, KeyboardInterrupt) as error:
        fail(error)
    speedups = report["summary"].get("paired_speedup_baseline_over_candidate", {})
    print(json.dumps({"status": report["status"], "report": str(args.report),
                      "complete_pairs": report["summary"]["complete_pairs"],
                      "median_paired_baseline_over_candidate":
                      {name: values["median"] for name, values in speedups.items()}}, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
