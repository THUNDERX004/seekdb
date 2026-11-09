#!/usr/bin/env python3
"""Serial, independently verified local comparison; never an official score.

Example: python -B bench/evaluate.py --baseline old.exe --candidate new.exe
         --runs 3 --rows 10000 --dim 128 --queries 1000 --batch 100 --k 10
         --seed 20261109 --report comparison.json

Only complete pipe request/response exchanges are timed. SQL preparation,
startup, DDL, the float64 oracle, validation, shutdown and restart are excluded.
The workload is exact mode, warm after insertion, with one client and no
concurrent database processes. Resource diagnostics are not cgroup scores.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import platform
import queue
import random
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import numpy as np
except ImportError:
    np = None


class EvaluationError(RuntimeError):
    pass


class WindowsDiagnostics:
    """An owned query handle to this client's PID, never a service handle."""

    def __init__(self, pid: int):
        self.pid = pid
        self.handle = None
        self.reason = "Windows process APIs are unavailable on this platform"
        if os.name != "nt":
            return
        from ctypes import wintypes as wt

        class MemoryCounters(ctypes.Structure):
            _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [
                (name, ctypes.c_size_t)
                for name in (
                    "PeakWorkingSetSize", "WorkingSetSize",
                    "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                    "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                    "PagefileUsage", "PeakPagefileUsage",
                )
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount",
            )]

        try:
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
            self.kernel.OpenProcess.restype = wt.HANDLE
            self.kernel.CloseHandle.argtypes = [wt.HANDLE]
            self.kernel.CloseHandle.restype = wt.BOOL
            self.kernel.GetProcessTimes.argtypes = [wt.HANDLE] + [
                ctypes.POINTER(wt.FILETIME)
            ] * 4
            self.kernel.GetProcessTimes.restype = wt.BOOL
            self.kernel.GetProcessIoCounters.argtypes = [
                wt.HANDLE, ctypes.POINTER(IoCounters)
            ]
            self.kernel.GetProcessIoCounters.restype = wt.BOOL
            self.psapi.GetProcessMemoryInfo.argtypes = [
                wt.HANDLE, ctypes.POINTER(MemoryCounters), wt.DWORD
            ]
            self.psapi.GetProcessMemoryInfo.restype = wt.BOOL
            self.handle = self.kernel.OpenProcess(0x0400 | 0x0010, False, pid)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
            self.memory_type, self.io_type, self.time_type = (
                MemoryCounters, IoCounters, wt.FILETIME
            )
        except (OSError, AttributeError) as error:
            self.reason = str(error)
            self.close()

    def snapshot(self) -> dict[str, Any]:
        if not self.handle:
            return {"available": False, "pid": self.pid, "reason": self.reason}
        try:
            times = [self.time_type() for _ in range(4)]
            if not self.kernel.GetProcessTimes(
                self.handle, *(ctypes.byref(value) for value in times)
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            seconds = lambda value: (
                (value.dwHighDateTime << 32) | value.dwLowDateTime
            ) / 10_000_000
            memory = self.memory_type()
            memory.cb = ctypes.sizeof(memory)
            if not self.psapi.GetProcessMemoryInfo(
                self.handle, ctypes.byref(memory), memory.cb
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            io = self.io_type()
            if not self.kernel.GetProcessIoCounters(self.handle, ctypes.byref(io)):
                raise ctypes.WinError(ctypes.get_last_error())
            return {
                "available": True, "pid": self.pid,
                "cpu_kernel_seconds": seconds(times[2]),
                "cpu_user_seconds": seconds(times[3]),
                "cpu_total_seconds": seconds(times[2]) + seconds(times[3]),
                "PeakWorkingSetSize_bytes": memory.PeakWorkingSetSize,
                "PeakPagefileUsage_bytes": memory.PeakPagefileUsage,
                "WorkingSetSize_bytes": memory.WorkingSetSize,
                "io_counters": {name: getattr(io, name) for name, _ in io._fields_},
            }
        except OSError as error:
            return {"available": False, "pid": self.pid, "reason": str(error)}

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def diagnostic_delta(before: dict, after: dict) -> dict:
    if not before.get("available") or not after.get("available"):
        return {"available": False}
    return {
        "available": True,
        **{key: after[key] - before[key] for key in (
            "cpu_kernel_seconds", "cpu_user_seconds", "cpu_total_seconds"
        )},
        "io_counters": {
            key: value - before["io_counters"][key]
            for key, value in after["io_counters"].items()
        },
    }


class Client:
    """Timeouts cover writes and reads; stderr is continuously drained."""

    def __init__(self, executable: Path, data_dir: Path, timeout: float):
        self.timeout = timeout
        self.closed = False
        self.requests: queue.Queue = queue.Queue()
        self.responses: queue.Queue = queue.Queue()
        self.stderr_tail: deque[bytes] = deque(maxlen=32)
        token = uuid.uuid4().hex
        self.marker = f"__MVD_END_{token}__".encode()
        self.startup_marker = f"__MVD_READY_{token}__".encode()
        self.process = subprocess.Popen(
            [str(executable), f"--data-dir={data_dir}", "--vector-mode=exact",
             f"--ready-marker={self.marker.decode()}",
             f"--startup-marker={self.startup_marker.decode()}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.diagnostics = WindowsDiagnostics(self.process.pid)
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.io_thread = threading.Thread(target=self._exchange, daemon=True)
        self.stderr_thread.start()
        self.io_thread.start()
        try:
            startup = self._receive("startup")
            if startup:
                raise EvaluationError(f"unexpected startup stdout: {startup[:3]!r}")
        except BaseException:
            self.abort()
            raise

    def _drain_stderr(self):
        try:
            while chunk := self.process.stderr.read1(4096):
                self.stderr_tail.append(chunk)
        except (OSError, ValueError):
            pass

    def _read_response(self, marker: bytes) -> list[bytes]:
        lines = []
        size = 0
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise EvaluationError("database exited before its response marker")
            size += len(line)
            if size > 64 * 1024 * 1024:
                raise EvaluationError("response exceeded the 64 MiB safety limit")
            line = line.rstrip(b"\r\n")
            if line == marker:
                return lines
            lines.append(line)

    def _exchange(self):
        try:
            self.responses.put((True, self._read_response(self.startup_marker)))
            while True:
                statement = self.requests.get()
                if statement is None:
                    break
                self.process.stdin.write(statement)
                self.process.stdin.flush()
                self.responses.put((True, self._read_response(self.marker)))
        except Exception as error:
            self.responses.put((False, error))
        finally:
            try:
                self.process.stdin.close()
            except (OSError, ValueError):
                pass

    def _receive(self, context: str) -> list[bytes]:
        try:
            success, result = self.responses.get(timeout=self.timeout)
        except queue.Empty:
            raise EvaluationError(f"PID {self.process.pid}: {context} timed out") from None
        if not success:
            raise EvaluationError(f"PID {self.process.pid}: {context}: {result}")
        return result

    def execute(self, statement: bytes) -> list[bytes]:
        if self.closed:
            raise EvaluationError("request on a closed client")
        self.requests.put(statement)
        try:
            return self._receive("SQL request")
        except BaseException:
            self.abort()
            raise

    def _release(self):
        self.io_thread.join(timeout=2)
        self.stderr_thread.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()
        self.diagnostics.close()

    def abort(self):
        if self.closed:
            return
        self.closed = True
        self.requests.put(None)
        try:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=5)
        finally:
            self._release()

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.requests.put(None)
        try:
            try:
                self.process.wait(timeout=min(self.timeout, 10))
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise EvaluationError("database did not exit after stdin EOF") from None
            if self.process.returncode:
                raise EvaluationError(f"database exited with code {self.process.returncode}")
        finally:
            self._release()

    def __enter__(self):
        return self

    def __exit__(self, kind, error, traceback):
        if kind is None:
            self.close()
        else:
            self.abort()
        if kind is not None and self.stderr_tail and hasattr(error, "add_note"):
            error.add_note(b"".join(self.stderr_tail).decode("utf-8", "replace")[-4096:])


def sql_bytes(sql: str) -> bytes:
    return (sql.rstrip().rstrip(";") + ";\n").encode("utf-8")


def check_success(output: list[bytes], context: str):
    for line in output:
        if line.startswith(b"ERROR "):
            raise EvaluationError(f"{context}: {line.decode('utf-8', 'replace')}")


@dataclass
class Query:
    sql: bytes
    group: str
    category: str | None
    probe: Any
    truth_ids: tuple[int, ...] = ()
    truth_distances: tuple[float, ...] = ()


@dataclass
class Workload:
    create_sql: bytes
    inserts: list[tuple[bytes, int]]
    queries: list[Query]
    matrix: Any
    categories: list[str]
    persistence_pages: list[tuple[bytes, int, int]]
    persistence_edges: list[bytes]
    sql_sha256: str


def prepare_workload(args, seed: int) -> Workload:
    rng = random.Random(seed)
    matrix = np.empty((args.rows, args.dim), dtype=np.float32)
    categories = [f"c{index % 10}" for index in range(args.rows)]
    create = sql_bytes(
        "CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(16), "
        f"payload VARCHAR(64), emb VECTOR({args.dim}))"
    )
    inserts = []
    rows = []
    for index in range(args.rows):
        # Reconstruct float32 from the exact decimal tokens sent to the DB.
        tokens = [f"{rng.random():.6f}" for _ in range(args.dim)]
        matrix[index] = np.asarray(tokens, dtype=np.float32)
        literal = "[" + ",".join(tokens) + "]"
        rows.append(f"({index + 1},'{categories[index]}','row{index}','{literal}')")
        if len(rows) == args.batch or index == args.rows - 1:
            inserts.append((sql_bytes("INSERT INTO bench VALUES " + ",".join(rows)), len(rows)))
            rows = []
    queries = []
    for index in range(args.queries):
        tokens = [f"{rng.random():.6f}" for _ in range(args.dim)]
        probe = np.asarray(tokens, dtype=np.float32)
        category = categories[rng.randrange(args.rows)]
        filtered = index % 2 == 1
        literal = "[" + ",".join(tokens) + "]"
        sql = f"SELECT id,L2_DISTANCE(emb,'{literal}') AS d FROM bench "
        if filtered:
            sql += f"WHERE cat = '{category}' "
        sql += f"ORDER BY d LIMIT {args.k}"
        queries.append(Query(sql_bytes(sql), "filtered" if filtered else "unfiltered",
                             category if filtered else None, probe))
    pages = []
    page_size = min(args.batch, 64)
    for first in range(1, args.rows + 1, page_size):
        end = min(args.rows + 1, first + page_size)
        pages.append((sql_bytes(
            "SELECT id,cat,payload,emb FROM bench "
            f"WHERE id >= {first} AND id < {end} ORDER BY id LIMIT {end - first + 1}"
        ), first, end))
    edges = [sql_bytes(f"SELECT id FROM bench WHERE id {op} {bound} ORDER BY id LIMIT 1")
             for op, bound in (("<", 1), (">", args.rows))]
    digest = hashlib.sha256()
    for statement in [create, *(sql for sql, _ in inserts),
                      *(query.sql for query in queries),
                      *(sql for sql, _, _ in pages), *edges]:
        digest.update(len(statement).to_bytes(8, "little"))
        digest.update(statement)
    return Workload(create, inserts, queries, matrix, categories, pages, edges, digest.hexdigest())


def prepare_oracle(workload: Workload, k: int):
    matrix64 = workload.matrix.astype(np.float64)
    ids = np.arange(1, len(workload.matrix) + 1, dtype=np.int64)
    categories = np.asarray(workload.categories)
    for query in workload.queries:
        mask = categories == query.category if query.category is not None else slice(None)
        candidate_ids = ids[mask]
        delta = matrix64[mask] - query.probe.astype(np.float64)
        squared = np.sum(delta * delta, axis=1, dtype=np.float64)
        order = np.lexsort((candidate_ids, squared))[:k]
        query.truth_ids = tuple(int(value) for value in candidate_ids[order])
        query.truth_distances = tuple(float(value) for value in np.sqrt(squared[order]))


def validate_queries(workload: Workload, outputs: list[list[bytes]]) -> dict:
    if len(outputs) != len(workload.queries):
        raise EvaluationError("not every query produced an output")
    totals = {group: {"queries_checked": 0, "hits": 0, "truth_ids": 0}
              for group in ("unfiltered", "filtered")}
    for index, (query, output) in enumerate(zip(workload.queries, outputs)):
        context = f"query {index} ({query.group})"
        check_success(output, context)
        if len(output) != len(query.truth_ids):
            raise EvaluationError(f"{context}: row count {len(output)} != {len(query.truth_ids)}")
        ids, distances = [], []
        for line in output:
            fields = line.split(b"\t")
            if len(fields) != 2:
                raise EvaluationError(f"{context}: expected ID and distance")
            try:
                primary, distance = int(fields[0]), float(fields[1])
            except ValueError as error:
                raise EvaluationError(f"{context}: invalid ID or distance") from error
            if not 1 <= primary <= len(workload.matrix):
                raise EvaluationError(f"{context}: out-of-dataset ID {primary}")
            if query.category is not None and workload.categories[primary - 1] != query.category:
                raise EvaluationError(f"{context}: ID {primary} violates the filter")
            if not math.isfinite(distance) or distance < 0:
                raise EvaluationError(f"{context}: non-finite or negative distance")
            ids.append(primary)
            distances.append(distance)
        if len(set(ids)) != len(ids):
            raise EvaluationError(f"{context}: duplicate IDs")
        group = totals[query.group]
        group["queries_checked"] += 1
        group["hits"] += len(set(ids).intersection(query.truth_ids))
        group["truth_ids"] += len(query.truth_ids)
        # Ordered IDs simultaneously enforce exact membership and primary-key ties.
        if tuple(ids) != query.truth_ids:
            raise EvaluationError(
                f"{context}: exact IDs/order differ: {ids[:20]} != {list(query.truth_ids[:20])}"
            )
        for primary, actual, expected in zip(ids, distances, query.truth_distances):
            if not math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-5):
                raise EvaluationError(
                    f"{context}: ID {primary} distance {actual} != {expected}"
                )
        if any(right < left for left, right in zip(distances, distances[1:])):
            raise EvaluationError(f"{context}: reported distances are not ascending")
    for group, values in totals.items():
        values["recall_at_k"] = values["hits"] / values["truth_ids"]
        if values["recall_at_k"] < 0.95:
            raise EvaluationError(f"{group}: Recall@K below 0.95")
    return {"mode": "exact", "all_queries_checked": True,
            "distance_rtol": 1e-5, "distance_atol": 1e-5,
            "recall_threshold": 0.95, "groups": totals}


def validate_persistence(client: Client, workload: Workload) -> dict:
    checked = 0
    for statement, first, end in workload.persistence_pages:
        output = client.execute(statement)
        check_success(output, "persistence page")
        if len(output) != end - first:
            raise EvaluationError(f"persistence IDs [{first},{end}): wrong row count")
        for expected_id, line in zip(range(first, end), output):
            fields = line.decode("utf-8", "strict").split("\t")
            if len(fields) != 4 or fields[:3] != [
                str(expected_id), workload.categories[expected_id - 1], f"row{expected_id - 1}"
            ]:
                raise EvaluationError(f"persistence row {expected_id}: ID/category/payload differ")
            try:
                vector = json.loads(fields[3])
                if not isinstance(vector, list) or any(type(value) not in (int, float) for value in vector):
                    raise ValueError("vector must be a numeric JSON array")
                stored = np.asarray(vector, dtype=np.float32)
            except (ValueError, TypeError, OverflowError) as error:
                raise EvaluationError(f"persistence row {expected_id}: invalid vector") from error
            if not np.array_equal(stored, workload.matrix[expected_id - 1]):
                raise EvaluationError(f"persistence row {expected_id}: float32 vector differs")
            checked += 1
    for statement in workload.persistence_edges:
        output = client.execute(statement)
        check_success(output, "persistence outside primary-key range")
        if output:
            raise EvaluationError("persistence: unexpected rows outside the inserted ID range")
    return {"passed": True, "rows_checked": checked, "all_columns_checked": True,
            "float32_vectors_exact": True, "page_queries": len(workload.persistence_pages),
            "extra_rows_checked": True}


def evaluate_binary(executable: Path, workload: Workload, timeout: float, result: dict):
    result.update(status="running", write_batch_ns=[], queries_raw=[])
    with tempfile.TemporaryDirectory(prefix="mvd-evaluate-") as directory:
        data_dir = Path(directory)
        with Client(executable, data_dir, timeout) as client:
            result["pid"] = client.process.pid
            output = client.execute(workload.create_sql)
            if output != [b"OK"]:
                raise EvaluationError(f"CREATE TABLE failed: {output[:3]!r}")
            before_write = client.diagnostics.snapshot()
            for statement, count in workload.inserts:
                started = time.perf_counter_ns()
                output = client.execute(statement)
                elapsed = time.perf_counter_ns() - started
                result["write_batch_ns"].append(elapsed)
                # Validation and bookkeeping are outside every measured exchange.
                if output != [f"OK {count}".encode()]:
                    raise EvaluationError(f"INSERT acknowledgement differs: {output[:3]!r}")
            after_write = client.diagnostics.snapshot()
            query_outputs = []
            for index, query in enumerate(workload.queries):
                started = time.perf_counter_ns()
                output = client.execute(query.sql)
                elapsed = time.perf_counter_ns() - started
                result["queries_raw"].append(
                    {"index": index, "group": query.group, "elapsed_ns": elapsed}
                )
                check_success(output, f"query {index}")
                query_outputs.append(output)
            after_query = client.diagnostics.snapshot()
            result["write_seconds"] = sum(result["write_batch_ns"]) / 1e9
            result["query_seconds"] = sum(row["elapsed_ns"] for row in result["queries_raw"]) / 1e9
            result["query_group_seconds"] = {
                group: sum(row["elapsed_ns"] for row in result["queries_raw"] if row["group"] == group) / 1e9
                for group in ("unfiltered", "filtered")
            }
            result["diagnostics"] = {
                "before_write": before_write, "after_write": after_write,
                "after_query": after_query,
                "write_delta": diagnostic_delta(before_write, after_write),
                "query_delta": diagnostic_delta(after_write, after_query),
            }
        result["validation"] = validate_queries(workload, query_outputs)
        # A fresh process opens the same directory; every column of every row is read.
        with Client(executable, data_dir, timeout) as recovered:
            result["restart_pid"] = recovered.process.pid
            result["persistence"] = validate_persistence(recovered, workload)
            result["restart_diagnostics"] = recovered.diagnostics.snapshot()
    result["status"] = "passed"


def fingerprint(path: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    info = path.stat()
    return {"path": str(path), "sha256": digest.hexdigest(),
            "size_bytes": info.st_size, "mtime_ns": info.st_mtime_ns}


def statistics_for(values: list[float]) -> dict:
    return {"samples": len(values), "median": statistics.median(values),
            "min": min(values), "max": max(values)}


def summarize(runs: list[dict]) -> dict:
    summary = {}
    metrics = {
        "write_seconds": lambda result: result["write_seconds"],
        "query_seconds": lambda result: result["query_seconds"],
        "unfiltered_query_seconds": lambda result: result["query_group_seconds"]["unfiltered"],
        "filtered_query_seconds": lambda result: result["query_group_seconds"]["filtered"],
    }
    for label in ("baseline", "candidate"):
        passed = [run["results"][label] for run in runs
                  if run["results"].get(label, {}).get("status") == "passed"]
        if passed:
            summary[label] = {name: statistics_for([get(result) for result in passed])
                              for name, get in metrics.items()}
            peaks = [result["diagnostics"]["after_query"] for result in passed
                     if result["diagnostics"]["after_query"].get("available")]
            if peaks:
                summary[label]["windows_lifetime_peaks"] = {
                    name: statistics_for([peak[name] for peak in peaks])
                    for name in ("PeakWorkingSetSize_bytes", "PeakPagefileUsage_bytes")
                }
    paired = [run for run in runs if all(
        run["results"].get(label, {}).get("status") == "passed"
        for label in ("baseline", "candidate")
    )]
    if paired:
        summary["paired_speedup_baseline_over_candidate"] = {
            name: {**statistics_for(values := [
                get(run["results"]["baseline"]) / get(run["results"]["candidate"])
                for run in paired
            ]), "per_run": values}
            for name, get in metrics.items()
        }
    return summary


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    for name, default in (("runs", 3), ("rows", 10000), ("dim", 128),
                          ("queries", 1000), ("batch", 100), ("k", 10),
                          ("seed", 20261109)):
        parser.add_argument(f"--{name}", type=int, default=default)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=60, help="seconds per startup/request (default: 60)")
    # Accept the compact --batch100 / --k10 spellings as well as normal arguments.
    argv = [re.sub(r"^--(batch|k)([0-9]+)$", r"--\1=\2", item)
            for item in (sys.argv[1:] if argv is None else argv)]
    args = parser.parse_args(argv)
    for name in ("runs", "rows", "dim", "queries", "batch", "k"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name} must be positive")
    if args.queries < 2:
        parser.error("--queries must be at least 2 to cover both filter groups")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    for name in ("baseline", "candidate"):
        path = getattr(args, name).resolve()
        if not path.is_file():
            parser.error(f"--{name} executable does not exist: {path}")
        setattr(args, name, path)
    if args.report is not None:
        args.report = args.report.resolve()
        if args.report in (args.baseline, args.candidate, Path(__file__).resolve()):
            parser.error("--report must not overwrite a binary or this script")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    if np is None:
        print("ERROR: NumPy is required; install numpy in this Python environment. No processes started.", file=sys.stderr)
        return 2
    report = {
        "schema_version": "minivectordb-local-evaluation-v1",
        "status": "running", "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "official_cgroup_metrics_collected": False, "official_score": None, "official_rank": None,
        "parameters": {name: getattr(args, name) for name in
                       ("runs", "rows", "dim", "queries", "batch", "k", "seed", "timeout")},
        "environment": {
            "platform": platform.platform(), "machine": platform.machine(),
            "processor": platform.processor(), "logical_cpus": os.cpu_count(),
            "python": sys.version, "python_executable": sys.executable,
            "numpy": np.__version__, "cwd": str(Path.cwd()),
            "MVD_CACHE_LIMIT_MB": os.environ.get("MVD_CACHE_LIMIT_MB"),
            "perf_counter": vars(time.get_clock_info("perf_counter")),
        },
        "methodology": {
            "scope": "local development comparison only; not official cgroup E, score or rank",
            "timing": "sum of serial pipe request/response wall times, including client IPC overhead",
            "excluded": ["SQL generation", "oracle", "validation", "startup",
                         "DDL", "shutdown", "restart and persistence reads"],
            "query_cache_state": "same process immediately after durable inserts; no cold-cache claim",
            "data": "fresh directory per executable/run; seed + zero-based run index",
            "order": "baseline,candidate on odd rounds; candidate,baseline on even rounds; never parallel",
            "oracle": "all queries; exact sent 6-decimal tokens -> float32 storage -> NumPy float64 squared sums; distance/PK lexsort",
            "mode": "exact; ordered IDs must equal oracle (stricter than Recall >= 0.95)",
            "windows_cpu": "owned child process CPU only, sampled outside timers; no evaluator CPU",
            "windows_memory": "PeakWorkingSetSize and PeakPagefileUsage are process lifetime peaks, not phase deltas or cgroup memory; PeakPagefileUsage is commit charge, not disk I/O",
            "windows_io": "GetProcessIoCounters transfer/operation counts include pipes and other I/O; NOT physical disk bytes or official cgroup I/O",
        },
        "binaries": {}, "runs": [],
    }
    exit_code = 0
    try:
        report["binaries"] = {name: fingerprint(getattr(args, name))
                              for name in ("baseline", "candidate")}
        for index in range(args.runs):
            seed = args.seed + index
            print(f"Preparing round {index + 1}/{args.runs}, seed={seed}", file=sys.stderr, flush=True)
            workload = prepare_workload(args, seed)
            prepare_oracle(workload, args.k)
            order = ["baseline", "candidate"] if index % 2 == 0 else ["candidate", "baseline"]
            run = {"round": index + 1, "seed": seed, "order": order,
                   "sql_sha256": workload.sql_sha256, "results": {}}
            report["runs"].append(run)
            for label in order:
                print(f"Round {index + 1}: {label}", file=sys.stderr, flush=True)
                result = run["results"][label] = {}
                try:
                    evaluate_binary(getattr(args, label), workload, args.timeout, result)
                except BaseException as error:
                    result.update(status="failed", error=f"{type(error).__name__}: {error}")
                    raise
        report["binary_sha256_after"] = {
            name: fingerprint(getattr(args, name))["sha256"]
            for name in ("baseline", "candidate")
        }
        for name in ("baseline", "candidate"):
            if report["binary_sha256_after"][name] != report["binaries"][name]["sha256"]:
                raise EvaluationError(f"{name} binary changed during evaluation")
        report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        report["status"] = "failed"
        report["error"] = f"{type(error).__name__}: {error}"
        report["error_notes"] = getattr(error, "__notes__", [])
        exit_code = 130 if isinstance(error, KeyboardInterrupt) else 1
        print(report["error"], file=sys.stderr, flush=True)
    report["summary"] = summarize(report["runs"])
    report["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    rendered = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if args.report is not None:
        try:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(rendered, encoding="utf-8")
        except OSError as error:
            print(f"ERROR writing report: {error}", file=sys.stderr)
            exit_code = 1
    print(rendered, end="")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
