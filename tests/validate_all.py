#!/usr/bin/env python3
"""Run the local correctness contract, preserve raw logs and version fingerprints."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def fingerprint(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "size_bytes": path.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--normal", type=Path, required=True)
    parser.add_argument("--scalar", type=Path, required=True)
    parser.add_argument("--fault", type=Path, required=True)
    parser.add_argument("--allocation-normal", type=Path)
    parser.add_argument("--allocation-scalar", type=Path)
    parser.add_argument("--commit-allocation-normal", type=Path)
    parser.add_argument("--commit-allocation-scalar", type=Path)
    parser.add_argument("--catalog-allocation-normal", type=Path)
    parser.add_argument("--catalog-allocation-scalar", type=Path)
    parser.add_argument("--prefix", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    binaries = {name: getattr(args, name).resolve(strict=True)
                for name in ("normal", "scalar", "fault")}
    for kind in ("allocation", "commit-allocation", "catalog-allocation"):
        for label in ("normal", "scalar"):
            allocation = getattr(args, kind.replace("-", "_") + "_" + label)
            if allocation is not None:
                binaries[kind + "-" + label] = allocation.resolve(strict=True)
    suites = ("run_public.py", "test_extended.py", "test_semantics.py",
              "test_disk_scan.py", "test_hidden_edges.py", "test_differential.py",
              "test_exact_distance.py", "test_selection_paths.py",
              "test_stdio_contract.py", "test_formal_sql_edges.py",
              "test_round6_sql_contract.py", "test_round6_stream_contract.py",
              "test_stream_followup.py", "test_float32_literals.py",
              "test_sql_round11.py", "test_protocol_round11.py",
              "test_selection_retention.py", "test_large_results.py",
              "test_json_protocol.py", "test_official_json_public.py",
              "test_primary_ranges.py", "test_cached_topk.py",
              "test_string_fast_paths.py", "test_quantized_screen.py")
    commands = []
    for label in ("normal", "scalar"):
        commands.extend((f"{label}/{suite}",
                         [sys.executable, "-X", "utf8", "-B", str(root / "tests" / suite),
                          str(binaries[label])]) for suite in suites)
    commands += [
        ("public-adapter-regressions", [sys.executable, "-X", "utf8", "-B",
                                        str(root / "tests/test_public_harness.py")]),
        ("commit-failure-regressions", [sys.executable, "-X", "utf8", "-B",
                                        str(root / "tests/test_commit_publication.py"),
                                        str(binaries["normal"]), str(binaries["fault"])]),
        ("catalog-failure-regressions", [sys.executable, "-X", "utf8", "-B",
                                         str(root / "tests/test_catalog_publication.py"),
                                         str(binaries["normal"]), str(binaries["fault"])]),
        ("result-replay-failure-regressions", [sys.executable, "-X", "utf8", "-B",
                                              str(root / "tests/test_result_spool_io.py"),
                                              str(binaries["normal"]), str(binaries["fault"])]),
        ("normal/long-random", [sys.executable, "-X", "utf8", "-B",
                               str(root / "tests/test_differential.py"),
                               str(binaries["normal"]), "--case", "random",
                               "--steps", "4000", "--seed", "13",
                               "--seed", "2147483647", "--seed", "20261129"]),
    ]
    for kind in ("allocation", "commit-allocation", "catalog-allocation"):
        for label in ("normal", "scalar"):
            key = kind + "-" + label
            if key in binaries:
                commands.append((key, [sys.executable, "-X", "utf8", "-B",
                                       str(root / "tests/test_statement_allocation.py"),
                                       str(binaries[key])]))
    paths = [root / "src/minivectordb.cpp", *sorted((root / "src").glob("*.hpp")),
             root / "CMakeLists.txt", root / "Dockerfile",
             root / "STDIO_PROTOCOL.md", *sorted((root / "protocol_check").glob("*.py")),
             root / "protocol_check/config.json",
             *sorted((root / "tests").glob("*.py")),
             *sorted((root / "tests").glob("*.cpp")), root / "mysqltest/manifest.json"]
    before = {str(path.relative_to(root)): fingerprint(path) for path in paths}
    binary_before = {label: fingerprint(path) for label, path in binaries.items()}
    record = {"started_at_utc": datetime.now(timezone.utc).isoformat(),
              "status": "running", "files": before, "binaries": binary_before,
              "scope": "local correctness only; no official hidden-case or Linux claim",
              "runs": []}
    environment = os.environ.copy()
    for key in ("MVD_TEST_FAIL_COMMIT_PUBLICATION", "MVD_TEST_FAIL_WAL_APPEND",
                "MVD_TEST_FAIL_CATALOG_APPEND", "MVD_TEST_FAIL_CATALOG_PUBLICATION",
                "MVD_TEST_FAIL_RESULT_READ", "MVD_TEST_FAIL_RESULT_SYNC", "MVD_CACHE_LIMIT_MB"):
        environment.pop(key, None)
    transcript = []
    passed = True
    for label, command in commands:
        print(f"Running {label}", flush=True)
        started = time.monotonic()
        try:
            result = subprocess.run(command, cwd=root, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                    errors="strict", timeout=240, check=False)
            output, code = result.stdout, result.returncode
        except subprocess.TimeoutExpired as error:
            output = str(error)
            code = 124
        elapsed = time.monotonic() - started
        record["runs"].append({"label": label, "command": command, "returncode": code,
                               "seconds": elapsed, "output": output})
        transcript.append(f"=== {label} exit={code} seconds={elapsed:.6f} ===\n{output}\n")
        print(f"{'PASS' if code == 0 else 'FAIL'} {label} ({elapsed:.3f}s)", flush=True)
        if code:
            passed = False
            print(output, flush=True)
    after = {str(path.relative_to(root)): fingerprint(path) for path in paths}
    binary_after = {label: fingerprint(path) for label, path in binaries.items()}
    if before != after or binary_before != binary_after:
        passed = False
        record["version_error"] = "source/test/binary changed during validation"
    record.update(status="passed" if passed else "failed",
                  finished_at_utc=datetime.now(timezone.utc).isoformat(),
                  files_after=after, binaries_after=binary_after)
    prefix = args.prefix.resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    prefix.with_suffix(".txt").write_text("".join(transcript), encoding="utf-8")
    prefix.with_suffix(".json").write_text(json.dumps(record, indent=2, allow_nan=False)
                                           + "\n", encoding="utf-8")
    print(f"{record['status'].upper()}: {len(commands)} validation commands; {prefix}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
