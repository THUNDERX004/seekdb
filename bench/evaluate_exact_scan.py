#!/usr/bin/env python3
r"""Standard-library, independently checked local exact-scan comparison.

Example (the report must be a new file):
  python3 -B bench/evaluate_exact_scan.py --baseline /tmp/old \
      --candidate /tmp/new --report /tmp/exact-scan.json --rows 10000 \
      --dim 128 --queries 320 --unique-probes 16 --runs 3 --k 10

All three distributions run by default; --distribution positive-uniform selects one.
Queries are measured per group, per binary, per round. Each group gets a fresh
process/data directory, timed INSERTs, one separately timed first SELECT, one
untimed cycle of all unique probes, then --queries warm SELECTs. First SELECT
means cold query state after inserts, not a flushed OS/disk cache. Every response
is checked outside its timer; all oracle construction precedes database startup.
No NumPy, compilation, or concurrent database processes are required.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import math
import platform
import random
import statistics
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from evaluate import Client, EvaluationError, fingerprint, sql_bytes


GROUPS = ("unfiltered", "single_equality")
DISTRIBUTIONS = ("signed-uniform", "positive-uniform", "shifted-uniform")
BATCH = 100
LATTICE_SCALE = 65536


def progress(message):
    print(message, file=sys.stderr, flush=True)


class Heartbeat:
    def __init__(self, label):
        self.label = label
        self.next_message = time.monotonic() + 5

    def update(self, done, total):
        now = time.monotonic()
        if done == total or now >= self.next_message:
            progress(f"{self.label}: {done}/{total}")
            self.next_message = now + 5


def literal(vector):
    # Numerators, including the shifted distribution, fit 24 bits. Division
    # by 2**16 is exact in both
    # Python binary64 and stored binary32; str preserves the exact value.
    return "[" + ",".join(str(value / LATTICE_SCALE) for value in vector) + "]"


def prepare(args, distribution):
    rng = random.Random(args.seed)
    low = -LATTICE_SCALE if distribution == "signed-uniform" else 0
    high = LATTICE_SCALE
    if distribution == "shifted-uniform":
        low, high = 32 * LATTICE_SCALE, 33 * LATTICE_SCALE
    if args.unique_probes > (high - low) ** args.dim:
        raise ValueError("--unique-probes exceeds the distribution's vector space")
    primaries = list(range(1, args.rows + 1))
    rng.shuffle(primaries)
    vectors = []
    beat = Heartbeat(f"{distribution}: generate rows")
    for index in range(args.rows):
        vectors.append(tuple(rng.randrange(low, high) for _ in range(args.dim)))
        if (index + 1) % BATCH == 0 or index + 1 == args.rows:
            beat.update(index + 1, args.rows)
    probes, seen = [], set()
    while len(probes) < args.unique_probes:
        probe = tuple(rng.randrange(low, high) for _ in range(args.dim))
        if probe not in seen:
            seen.add(probe)
            probes.append(probe)

    create = sql_bytes("CREATE TABLE bench(id BIGINT PRIMARY KEY,"
                       f"cat VARCHAR(16),v VECTOR({args.dim}))")
    inserts = []
    beat = Heartbeat(f"{distribution}: prepare INSERTs")
    for start in range(0, args.rows, BATCH):
        stop = min(start + BATCH, args.rows)
        values = [f"({primaries[i]},'c{i % 4}','{literal(vectors[i])}')"
                  for i in range(start, stop)]
        inserts.append((sql_bytes("INSERT INTO bench VALUES " + ",".join(values)),
                        stop - start))
        beat.update(stop, args.rows)

    groups = {name: [] for name in GROUPS}
    beat = Heartbeat(f"{distribution}: integer oracle")
    for probe_index, probe in enumerate(probes):
        scores = []
        for index, vector in enumerate(vectors):
            squared = sum((a - b) ** 2 for a, b in zip(vector, probe))
            scores.append((squared, primaries[index]))
            if (index + 1) % 1024 == 0:
                beat.update(probe_index * args.rows + index + 1,
                            len(probes) * args.rows)
        for name in GROUPS:
            selected = (scores if name == "unfiltered" else
                        (score for index, score in enumerate(scores) if index % 4 == 0))
            # Ranking uses Python integers only, including the PK tie break.
            ranked = heapq.nsmallest(args.k, selected)
            truth = tuple((primary, squared, math.sqrt(squared) / LATTICE_SCALE)
                          for squared, primary in ranked)
            condition = "WHERE cat='c0' " if name == "single_equality" else ""
            statement = sql_bytes(
                f"SELECT id,L2_DISTANCE(v,'{literal(probe)}') AS d FROM bench "
                f"{condition}ORDER BY d LIMIT {args.k}")
            groups[name].append((statement, truth))
        beat.update((probe_index + 1) * args.rows, len(probes) * args.rows)

    digest = hashlib.sha256()
    for statement in (create, *(sql for sql, _ in inserts),
                      *(sql for name in GROUPS for sql, _ in groups[name])):
        digest.update(len(statement).to_bytes(8, "little"))
        digest.update(statement)
    return {"create": create, "inserts": inserts, "groups": groups,
            "sql_sha256": digest.hexdigest(),
            "matching_rows": {"unfiltered": args.rows,
                              "single_equality": (args.rows + 3) // 4}}


def validate(output, truth, context):
    if len(output) != len(truth):
        raise EvaluationError(f"{context}: row count {len(output)} != {len(truth)}")
    for position, (line, (primary, squared, distance)) in enumerate(zip(output, truth)):
        fields = line.split(b"\t")
        try:
            correct = (len(fields) == 2 and int(fields[0]) == primary and
                       math.isfinite(actual := float(fields[1])) and
                       math.isclose(actual, distance, rel_tol=1e-6, abs_tol=1e-6))
        except (ValueError, OverflowError):
            correct = False
        if not correct:
            raise EvaluationError(
                f"{context}: row {position}: {line!r}; expected id={primary}, "
                f"integer_squared={squared}, distance={distance}")


def exchange(client, statement):
    started = time.perf_counter_ns()
    output = client.execute(statement)
    elapsed = time.perf_counter_ns() - started
    return output, elapsed


def run_group(binary, directory, args, workload, name, result, label):
    queries = workload["groups"][name]
    result.update(status="running", write_batch_ns=[], warm_query_ns=[],
                  queries_checked=0, matching_rows=workload["matching_rows"][name])
    with Client(binary, directory, args.timeout, protocol="json") as client:
        if client.execute(workload["create"]) != [b"OK"]:
            raise EvaluationError("CREATE acknowledgement differs")
        beat = Heartbeat(f"{label}/{name}: INSERT batches")
        for index, (statement, count) in enumerate(workload["inserts"]):
            output, elapsed = exchange(client, statement)
            result["write_batch_ns"].append(elapsed)
            if output != [f"OK {count}".encode()]:
                raise EvaluationError(f"INSERT acknowledgement differs: {output[:3]!r}")
            beat.update(index + 1, len(workload["inserts"]))
        result["write_seconds"] = sum(result["write_batch_ns"]) / 1e9

        progress(f"{label}/{name}: first query")
        output, elapsed = exchange(client, queries[0][0])
        result["first_query_ns"] = elapsed
        result["first_query_seconds"] = elapsed / 1e9
        validate(output, queries[0][1], "first query")
        result["queries_checked"] += 1

        beat = Heartbeat(f"{label}/{name}: warmup probes")
        for index, (statement, truth) in enumerate(queries):
            validate(client.execute(statement), truth, f"warmup {index}")
            result["queries_checked"] += 1
            beat.update(index + 1, len(queries))
        beat = Heartbeat(f"{label}/{name}: warm queries")
        for index in range(args.queries):
            statement, truth = queries[index % len(queries)]
            output, elapsed = exchange(client, statement)
            result["warm_query_ns"].append(elapsed)
            validate(output, truth, f"warm query {index}")
            result["queries_checked"] += 1
            beat.update(index + 1, args.queries)
        result["warm_query_seconds"] = sum(result["warm_query_ns"]) / 1e9
        if result["warm_query_seconds"] <= 0:
            raise EvaluationError("warm query clock did not advance")
        result["warm_queries_per_second"] = args.queries / result["warm_query_seconds"]
    result["status"] = "passed"


def source_fingerprints(root):
    paths = [root / "CMakeLists.txt", root / "Dockerfile",
             root / "bench/evaluate.py", Path(__file__).resolve()]
    paths.extend(path for path in (root / "src").rglob("*") if path.is_file())
    return {path.relative_to(root).as_posix(): fingerprint(path)
            for path in sorted(paths)}


def check_unchanged(root, binaries, report):
    report["fingerprints_unchanged"] = False
    report["sources_after"] = source_fingerprints(root)
    report["binaries_after"] = {name: fingerprint(path) for name, path in binaries.items()}
    for kind in ("sources", "binaries"):
        before, after = report[kind + "_before"], report[kind + "_after"]
        changed = [key for key in sorted(before.keys() | after.keys())
                   if before.get(key) != after.get(key)]
        if changed:
            raise EvaluationError(f"{kind} changed during evaluation: {', '.join(changed)}")
    report["fingerprints_unchanged"] = True


def summarize(runs):
    summary = {}
    for distribution in dict.fromkeys(run["distribution"] for run in runs):
        pairs = [run for run in runs if run["distribution"] == distribution]
        summary[distribution] = {}
        for name in GROUPS:
            entry = {"paired_runs": len(pairs)}
            for metric in ("write_seconds", "first_query_seconds", "warm_query_seconds"):
                values = {label: [run["results"][label][name][metric] for run in pairs]
                          for label in ("baseline", "candidate")}
                ratios = [baseline / candidate if candidate > 0 else None
                          for baseline, candidate in zip(values["baseline"], values["candidate"])]
                entry[metric] = {
                    "baseline_median": statistics.median(values["baseline"]),
                    "candidate_median": statistics.median(values["candidate"]),
                    "paired_speedup_baseline_over_candidate": ratios,
                    "median_speedup": (statistics.median(ratios)
                                       if all(value is not None for value in ratios) else None)}
            summary[distribution][name] = entry
    return summary


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("baseline", "candidate", "report"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    for name, default in (("rows", 10000), ("dim", 128), ("queries", 320),
                          ("unique-probes", 16), ("runs", 3), ("k", 10),
                          ("seed", 20261116)):
        parser.add_argument(f"--{name}", type=int, default=default)
    parser.add_argument("--distribution", choices=DISTRIBUTIONS, nargs="+",
                        default=list(DISTRIBUTIONS))
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    for name in ("rows", "dim", "queries", "unique_probes", "runs", "k"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.dim > 65535 or args.rows > (1 << 63) - 1:
        parser.error("--dim must be <= 65535 and --rows must fit a BIGINT primary key")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    for name in ("baseline", "candidate"):
        path = getattr(args, name).expanduser().resolve()
        if not path.is_file():
            parser.error(f"--{name} must be an existing executable file: {path}")
        setattr(args, name, path)
    # Exclusive creation below also closes the race between this check and open.
    report = args.report.expanduser().absolute()
    if report.exists() or report.is_symlink():
        parser.error(f"--report already exists; choose a new output file: {report}")
    args.report = report.resolve()
    root = Path(__file__).resolve().parents[1]
    if root / "src" in args.report.parents or args.report in (
            args.baseline, args.candidate, Path(__file__).resolve(),
            root / "bench/evaluate.py", root / "CMakeLists.txt", root / "Dockerfile"):
        parser.error("--report must not be an implementation source or binary path")
    args.distribution = list(dict.fromkeys(args.distribution))
    return args


def main(argv=None):
    args = parse_args(argv)
    # Reserve the output before doing expensive work; never truncate an old file,
    # including symlinks/hard links. Keep this owned descriptor for the final JSON.
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        stream = args.report.open("x", encoding="utf-8")
    except OSError as error:
        print(f"Cannot create report: {error}", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[1]
    binaries = {name: getattr(args, name) for name in ("baseline", "candidate")}
    report = {
        "schema": "exact-scan-local-v1", "status": "running", "official_score": None,
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "environment": {"platform": platform.platform(), "python": sys.version},
        "parameters": {key: value for key, value in vars(args).items()
                       if not isinstance(value, Path)},
        "method": {"mode": "JSON search_mode=exact; ORDER BY d; integer squared L2 + PK",
                   "data": "integer numerators / 65536, exactly binary32; four balanced cat buckets",
                   "oracle": "all unique probes prepared before any database startup",
                   "timing": "serial pipe exchanges only; validation/SQL/oracle outside timers",
                   "cold_first": "first SELECT after inserts, separate fresh process/dir per group; OS cache not flushed",
                   "warm": "one full untimed probe cycle, then cyclic timed probes per group",
                   "order": "baseline,candidate on odd rounds; reversed on even rounds",
                   "distance_tolerance": {"relative": 1e-6, "absolute": 1e-6},
                   "write_batch_rows": BATCH},
        "workloads": {}, "runs": []}
    code = 0
    try:
        report["sources_before"] = source_fingerprints(root)
        report["binaries_before"] = {name: fingerprint(path) for name, path in binaries.items()}
        workloads = {}
        for distribution in args.distribution:
            progress(f"Preparing {distribution}: rows={args.rows}, dim={args.dim}")
            workloads[distribution] = prepare(args, distribution)
            report["workloads"][distribution] = {
                key: workloads[distribution][key] for key in ("sql_sha256", "matching_rows")}
        check_unchanged(root, binaries, report)
        for distribution, workload in workloads.items():
            for index in range(args.runs):
                order = (["baseline", "candidate"] if index % 2 == 0 else
                         ["candidate", "baseline"])
                run = {"distribution": distribution, "round": index + 1,
                       "order": order, "results": {}}
                report["runs"].append(run)
                for label in order:
                    results = run["results"][label] = {}
                    progress(f"{distribution}: round {index + 1}/{args.runs}, {label}")
                    for name in GROUPS:
                        result = results[name] = {}
                        try:
                            with tempfile.TemporaryDirectory(prefix="mvd-exact-scan-") as directory:
                                run_group(binaries[label], Path(directory), args, workload,
                                          name, result, label)
                        except (Exception, KeyboardInterrupt) as error:
                            result.update(status="failed", error=f"{type(error).__name__}: {error}")
                            raise
                        check_unchanged(root, binaries, report)
        report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}",
                      error_notes=getattr(error, "__notes__", []))
        progress(report["error"])
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if "sources_before" in report and "binaries_before" in report:
            try:
                check_unchanged(root, binaries, report)
            except (Exception, KeyboardInterrupt) as error:
                report.update(status="failed", integrity_error=f"{type(error).__name__}: {error}")
                progress(report["integrity_error"])
                code = 130 if isinstance(error, KeyboardInterrupt) else (code or 1)
        report["summary"] = summarize(report["runs"]) if report["status"] == "passed" else {}
        report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        try:
            with stream:
                json.dump(report, stream, indent=2, allow_nan=False)
                stream.write("\n")
        except (OSError, ValueError) as error:
            progress(f"Cannot write report: {error}")
            code = code or 1
    print(json.dumps({"status": report["status"], "report": str(args.report),
                      "summary": report["summary"]}, allow_nan=False), flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
