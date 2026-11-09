#!/usr/bin/env python3
"""Mechanically summarize retained round-3 evidence, without an official score."""
from __future__ import annotations

import hashlib
import json
import statistics
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads((ROOT / path).read_text(encoding="utf-8"))


def stats(values):
    return {"values": values, "median": statistics.median(values),
            "mean": statistics.mean(values), "sample_stddev": statistics.stdev(values),
            "min": min(values), "max": max(values)}


def comparison(path):
    report = read(path)
    if report["status"] != "passed" or len(report["runs"]) != 3:
        raise AssertionError(f"incomplete comparison: {path}")
    fields = {}
    for metric in ("write_seconds", "query_seconds"):
        values = {label: [run["results"][label][metric] for run in report["runs"]]
                  for label in ("baseline", "candidate")}
        fields[metric] = {label: stats(series) for label, series in values.items()}
        fields[metric]["ratio_of_medians"] = (statistics.median(values["baseline"]) /
                                              statistics.median(values["candidate"]))
        fields[metric]["paired_ratios"] = stats([
            before / after for before, after in zip(values["baseline"], values["candidate"])])
    for run in report["runs"]:
        for label in ("baseline", "candidate"):
            result = run["results"][label]
            if (result["status"] != "passed" or not result["validation"]["all_queries_checked"]
                    or not result["persistence"]["passed"]):
                raise AssertionError(f"incomplete validation: {path}/{label}")
            if any(group["recall_at_k"] != 1.0 for group in
                   result["validation"]["groups"].values()):
                raise AssertionError(f"nonexact recall: {path}")
    return {"source": path, "binaries": report["binaries"],
            "parameters": report["parameters"], "statistics": fields}


def main():
    final_hash = hashlib.sha256((ROOT / "build/minivectordb.exe").read_bytes()).hexdigest()
    source_hash = hashlib.sha256((ROOT / "src/minivectordb.cpp").read_bytes()).hexdigest()
    output = {"schema_version": "minivectordb-round3-summary-v1",
              "timestamp_utc": datetime.now(timezone.utc).isoformat(),
              "official_score": None, "official_rank": None,
              "source_sha256": source_hash, "binary_sha256": final_hash,
              "scope": "local Windows diagnostics; no Linux/cgroup/hidden-case guarantee"}
    for name, path in (("main", "bench/results/round3-final-main.json"),
                       ("disk", "bench/results/round3-final-disk.json")):
        result = comparison(path)
        if result["binaries"]["candidate"]["sha256"] != final_hash:
            raise AssertionError(f"wrong final binary: {path}")
        output[name] = result
    validation = read("refine-logs/round3-validation-final.json")
    source_record = validation["files"].get("src/minivectordb.cpp",
                       validation["files"].get("src\\minivectordb.cpp"))
    if (validation["status"] != "passed" or len(validation["runs"]) != 19 or
            validation["binaries"]["normal"]["sha256"] != final_hash or
            source_record is None or source_record["sha256"] != source_hash):
        raise AssertionError("validation belongs to a different source/binary")
    output["validation"] = {"source": "refine-logs/round3-validation-final.json",
                            "commands_passed": len(validation["runs"]),
                            "long_random_output": validation["runs"][-1]["output"]}
    ordinary = {label: [] for label in ("before", "final")}
    for index in (1, 2, 3):
        pair = {}
        for label in ordinary:
            path = f"profile_output/round3-ordinary-{label}-{index}.json"
            report = read(path)
            if report["status"] != "passed" or not report["validation"]["all_columns_checked"]:
                raise AssertionError(f"ordinary query validation missing: {path}")
            if label == "final" and report["binary"]["sha256"] != final_hash:
                raise AssertionError(f"wrong final binary: {path}")
            ordinary[label].append(report["query_seconds"])
            pair[label] = report
        if pair["before"]["parameters"] != pair["final"]["parameters"]:
            raise AssertionError("ordinary paired parameters differ")
    output["ordinary_supplemental"] = {label: stats(values) for label, values in ordinary.items()}
    output["ordinary_supplemental"]["ratio_of_medians"] = (
        statistics.median(ordinary["before"]) / statistics.median(ordinary["final"]))
    output["rejected_experiments"] = {name: comparison(path) for name, path in (
        ("batch", "bench/results/round3-batch.json"),
        ("directory", "bench/results/round3-directory.json"))}
    rendered = json.dumps(output, indent=2, allow_nan=False) + "\n"
    path = ROOT / "bench/results/round3-summary.json"
    path.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
