#!/usr/bin/env python3
"""Verify all scalar rows and compare Windows process peaks during disk sorting.

This is a local diagnostic, not a cgroup memory-limit or physical-I/O test.
Each executable gets a fresh database and an explicit row-cache budget.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

from evaluate import Client, fingerprint, sql_bytes


def run(executable, rows, dim, cache_mib):
    with tempfile.TemporaryDirectory(prefix="mvd-projection-memory-") as directory:
        with patch.dict(os.environ, {"MVD_CACHE_LIMIT_MB": str(cache_mib)}):
            client = Client(executable, Path(directory), 30)
        with client:
            create = ("CREATE TABLE t(id BIGINT PRIMARY KEY,tag VARCHAR(8),"
                      f"v VECTOR({dim}))")
            assert client.execute(sql_bytes(create)) == [b"OK"]
            tail = ",0" * (dim - 1) + "]"
            for start in range(1, rows + 1, 100):
                end = min(rows + 1, start + 100)
                values = ",".join(f"({i},'tag','[{i}{tail}')" for i in range(start, end))
                assert client.execute(sql_bytes("INSERT INTO t VALUES " + values)) == [
                    f"OK {end - start}".encode()
                ]
            after_write = client.diagnostics.snapshot()
            result = client.execute(sql_bytes("SELECT id,tag FROM t ORDER BY id"))
            assert result == [f"{i}\ttag".encode() for i in range(1, rows + 1)]
            after_query = client.diagnostics.snapshot()
            return {"rows_verified": len(result), "after_write": after_write,
                    "after_ordered_projection": after_query}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument("--dim", type=int, default=1024)
    parser.add_argument("--cache-mib", type=int, default=8)
    args = parser.parse_args()
    if min(args.rows, args.dim, args.cache_mib) <= 0 or args.rows > 2**24:
        parser.error("positive sizes and row IDs exactly representable as float32 required")
    binaries = {label: getattr(args, label).resolve(strict=True)
                for label in ("baseline", "candidate")}
    if args.report.resolve() in (*binaries.values(), Path(__file__).resolve()):
        parser.error("report must not overwrite a binary or this script")
    report = {"scope": "local Windows process memory, not cgroup metrics",
              "parameters": {"rows": args.rows, "dim": args.dim,
                             "cache_mib": args.cache_mib,
                             "vector_payload_bytes": args.rows * args.dim * 4},
              "binaries": {label: fingerprint(path) for label, path in binaries.items()},
              "runs": {}}
    for label, executable in binaries.items():
        print("Running " + label, flush=True)
        report["runs"][label] = run(executable, args.rows, args.dim, args.cache_mib)
        assert fingerprint(executable) == report["binaries"][label], "binary changed"
    report["status"] = "passed"
    rendered = json.dumps(report, indent=2, allow_nan=False) + "\n"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
