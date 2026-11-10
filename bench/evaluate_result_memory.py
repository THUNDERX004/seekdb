#!/usr/bin/env python3
"""Compare Windows process memory for a large disk-backed text result.

These are development measurements, not container or official scoring metrics.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

from evaluate import Client, fingerprint, sql_bytes


def run(executable, rows, width, cache_mib):
    with tempfile.TemporaryDirectory(prefix="mvd-result-memory-") as directory:
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(cache_mib)}):
            client = Client(executable, Path(directory), 60)
        with client:
            assert client.execute(sql_bytes(
                f"CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR({width}))"
            )) == [b"OK"]
            text = "x" * width
            for first in range(1, rows + 1, 100):
                stop = min(first + 100, rows + 1)
                values = ",".join(f"({i},'{text}')" for i in range(first, stop))
                assert client.execute(sql_bytes("INSERT INTO t VALUES" + values)) == [
                    f"OK {stop - first}".encode()
                ]
            after_write = client.diagnostics.snapshot()
            result = client.execute(sql_bytes("SELECT id,s FROM t"))
            assert len(result) == rows
            ids = set()
            expected = text.encode()
            for row in result:
                primary, separator, payload = row.partition(b"\t")
                assert separator == b"\t" and payload == expected
                primary = int(primary)
                assert 1 <= primary <= rows and primary not in ids
                ids.add(primary)
            after_query = client.diagnostics.snapshot()
            assert not list(Path(directory).glob(".mvd-result-*"))
            return {"rows_verified": len(result),
                    "output_bytes": sum(len(row) + 1 for row in result),
                    "after_write": after_write, "after_select": after_query}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=7500)
    parser.add_argument("--width", type=int, default=8192)
    parser.add_argument("--cache-mib", type=int, default=8)
    args = parser.parse_args()
    if min(args.rows, args.width, args.cache_mib) <= 0:
        parser.error("sizes must be positive")
    if args.rows * (args.width + len(str(args.rows)) + 2) >= 63 * 2**20:
        parser.error("workload must fit the benchmark client's 64 MiB response guard")
    binaries = {label: getattr(args, label).resolve(strict=True)
                for label in ("baseline", "candidate")}
    if args.report.resolve() in (*binaries.values(), Path(__file__).resolve()):
        parser.error("report must not overwrite an executable or this script")
    report = {"scope": "Windows process peaks; not cgroup metrics or an official score",
              "parameters": {"rows": args.rows, "width": args.width,
                             "cache_mib": args.cache_mib},
              "binaries": {key: fingerprint(path) for key, path in binaries.items()},
              "runs": {}}
    for label, executable in binaries.items():
        print("Running " + label, flush=True)
        report["runs"][label] = run(executable, args.rows, args.width, args.cache_mib)
        assert fingerprint(executable) == report["binaries"][label], "binary changed"
    report["status"] = "passed"
    rendered = json.dumps(report, indent=2, allow_nan=False) + "\n"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
