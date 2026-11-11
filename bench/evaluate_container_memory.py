#!/usr/bin/env python3
"""Local Linux cgroup-v2 comparison; requires Docker, NumPy and the cached image.

python -B bench/evaluate_container_memory.py --baseline /path/to/linux-old \
    --candidate /path/to/linux-new --report memory-comparison.json
Defaults: 3 alternating pairs, 10000 rows, dim 128, 1000 queries, batch 100, K 10.
The host runs the driver/oracle; only the database runs inside each container.
"""
from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch
import uuid

import evaluate as evaluation


def docker(*arguments):
    result = subprocess.run(["docker", *arguments], capture_output=True, text=True,
                            timeout=30, check=True)
    return result.stdout.strip()


def container_cgroup(container_id):
    pid = int(docker("inspect", "--format", "{{.State.Pid}}", container_id))
    entries = Path(f"/proc/{pid}/cgroup").read_text().splitlines()
    relative = next((line[3:] for line in entries if line.startswith("0::")), None)
    if not relative or container_id not in relative or ".." in Path(relative).parts:
        raise RuntimeError("container cgroup v2 is not visible on this Linux host")
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        before, after = line.split(" - ", 1)
        if after.split()[0] != "cgroup2":
            continue
        fields = before.split()
        decode = lambda value: re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), value)
        root, mount = Path(decode(fields[3])), Path(decode(fields[4]))
        if Path(relative).is_relative_to(root):
            path = mount / Path(relative).relative_to(root)
            if (path / "memory.current").is_file():
                quota, period = map(int, (path / "cpu.max").read_text().split())
                if (int((path / "memory.max").read_text()) != 256 * 1024**2 or
                        int((path / "memory.swap.max").read_text()) != 0 or quota != 2 * period):
                    raise RuntimeError("container cgroup limits differ from requested limits")
                return pid, path
    raise RuntimeError("cannot resolve the container's host cgroup-v2 mount")


def snapshot(path):
    def counters(name):
        return {key: int(value) for key, value in
                (line.split() for line in (path / name).read_text().splitlines())}
    return {"memory.peak": int((path / "memory.peak").read_text()),
            "memory.current": int((path / "memory.current").read_text()),
            "memory.stat": counters("memory.stat"), "memory.events": counters("memory.events"),
            "io.stat": {parts[0]: {key: int(value) for key, value in
                                   (item.split("=", 1) for item in parts[1:])}
                        for parts in (line.split() for line in (path / "io.stat").read_text().splitlines())}}


def memory_delta(before, after):
    return {**{key: after[key] - before[key] for key in ("memory.current", "memory.peak")},
            **{key: after["memory.stat"][key] - before["memory.stat"][key]
               for key in ("anon", "file")}}


@contextmanager
def container_client(binary, directory, image, timeout):
    name = "mvd-memory-" + uuid.uuid4().hex
    container_id = None
    try:
        container_id = docker(
            "create", "--pull=never", "--interactive", "--name", name,
            "--attach", "stdin", "--attach", "stdout", "--attach", "stderr",
            "--label", f"mvd.memory.owner={name}", "--network", "none",
            "--cpus", "2", "--memory", "256m", "--memory-swap", "256m",
            "--user", f"{os.getuid()}:{os.getgid()}", "--workdir", "/app",
            "--mount", f"type=bind,src={binary},dst=/app/minivectordb,readonly",
            "--mount", f"type=bind,src={directory},dst=/data",
            "--entrypoint", "/app/minivectordb", image, "--data-dir=/data")
        real_popen = subprocess.Popen
        intended = [str(binary), f"--data-dir={directory}"]

        def launch(command, *args, **kwargs):
            if isinstance(command, (list, tuple)) and list(command) == intended:
                command = ["docker", "start", "--attach", "--interactive", container_id]
            return real_popen(command, *args, **kwargs)

        # Only intercept the exact Client JSON launch, only during construction;
        # subprocess.run and any other launch are delegated to the real Popen.
        with patch.object(evaluation.subprocess, "Popen", launch):
            client = evaluation.Client(binary, directory, timeout, protocol="json")
        with client:
            pid, path = container_cgroup(container_id)
            yield client, {"container": name, "container_id": container_id,
                           "database_pid": pid, "cgroup": str(path)}, path
    finally:
        # Recover our ID by its random ownership label if create timed out.
        owned = [container_id] if container_id else docker(
            "ps", "--all", "--quiet", "--no-trunc", "--filter", f"label=mvd.memory.owner={name}").splitlines()
        for owned_id in owned:
            docker("rm", "--force", owned_id)


def measure(binary, workload, image, timeout, result):
    result["status"] = "running"
    with tempfile.TemporaryDirectory(prefix="mvd-memory-") as directory:
        directory = Path(directory).resolve()
        with container_client(binary, directory, image, timeout) as (client, identity, path):
            result.update(identity)
            if client.execute(workload.create_sql) != [b"OK"]:
                raise RuntimeError("CREATE acknowledgement differs")
            for statement, count in workload.inserts:
                output = client.execute(statement)
                if output != [f"OK {count}".encode()]:
                    raise RuntimeError("INSERT acknowledgement differs")
            result["after_load"] = snapshot(path)
            outputs = []
            for query in workload.queries:
                output = client.execute(query.sql)
                outputs.append(output)
            # No persistence reads, extra SQL, or validation precedes this sample.
            result["after_queries"] = snapshot(path)
            result["query_growth_bytes"] = memory_delta(result["after_load"], result["after_queries"])
        result["queries"] = evaluation.validate_queries(workload, outputs)
        with container_client(binary, directory, image, timeout) as (client, identity, _):
            result["restart_container"] = identity["container"]
            result["restart"] = evaluation.validate_persistence(client, workload)
    result["status"] = "passed"


def main(argv=None):
    args = evaluation.parse_args(argv)
    dockerfile = Path(__file__).resolve().parents[1] / "Dockerfile"
    if args.report is None or args.report in (Path(__file__).resolve(), dockerfile):
        raise SystemExit("--report is required and must not overwrite source files")
    report = {"schema": "mvd-local-container-memory-v1", "status": "running",
              "started_at_utc": evaluation.datetime.now(evaluation.timezone.utc).isoformat(),
              "host": evaluation.platform.platform(),
              "parameters": {key: getattr(args, key) for key in
                             ("runs", "rows", "dim", "queries", "batch", "k", "seed", "timeout")},
              "limits": {"cpus": 2, "memory_bytes": 256 * 1024**2, "swap_bytes": 0, "network": "none"},
              "protocol": "json", "data_root": tempfile.gettempdir(), "runs": [],
              "limitations": ["Local host cgroup v2 only; not official E, score or rank.",
                              "Host driver/oracle and Docker CLI are outside the measured container.",
                              "memory.peak is cumulative since container creation, including startup/load.",
                              "Sequential snapshots are not atomic; caches are not flushed or cold.",
                              "anon/file are accounting categories, not proof of algorithm-specific bytes; shared pages and filesystem affect charges.",
                              "io.stat is cgroup-attributed block I/O, not a physical-media or official-I/O claim.",
                              "Restart and correctness reads are excluded from memory snapshots; no performance timings are collected."]}
    code = 0
    try:
        if sys.platform != "linux" or evaluation.np is None:
            raise RuntimeError("requires Linux, NumPy, and a local Docker daemon")
        endpoint = None if os.environ.get("DOCKER_CONTEXT") else os.environ.get("DOCKER_HOST")
        endpoint = endpoint or docker("context", "inspect", "--format", "{{.Endpoints.docker.Host}}")
        if not endpoint.startswith("unix://"):
            raise RuntimeError("requires a local Unix-socket Docker daemon")
        images = re.findall(r"^FROM (debian:[^\s@]+@sha256:[0-9a-f]{64})\s*$",
                            dockerfile.read_text(), flags=re.MULTILINE)
        if len(images) != 1:
            raise RuntimeError("expected one digest-pinned Debian runtime image in Dockerfile")
        image = report["image"] = images[0]
        report["image_id"] = docker("image", "inspect", "--format", "{{.Id}}", image)
        report["binaries"] = {label: evaluation.fingerprint(getattr(args, label))
                              for label in ("baseline", "candidate")}
        for index in range(args.runs):
            seed = args.seed + index
            workload = evaluation.prepare_workload(args, seed)
            evaluation.prepare_oracle(workload, args.k)
            order = ["baseline", "candidate"] if index % 2 == 0 else ["candidate", "baseline"]
            run = {"pair": index + 1, "seed": seed, "order": order,
                   "sql_sha256": workload.sql_sha256, "results": {}}
            report["runs"].append(run)
            for label in order:
                print(f"Pair {index + 1}/{args.runs}: {label}", file=sys.stderr, flush=True)
                result = run["results"][label] = {}
                try:
                    measure(getattr(args, label), workload, image, args.timeout, result)
                except (Exception, KeyboardInterrupt):
                    result["status"] = "failed"
                    raise
            run["candidate_minus_baseline_bytes"] = {
                phase: memory_delta(run["results"]["baseline"][phase], run["results"]["candidate"][phase])
                for phase in ("after_load", "after_queries")}
        if any(evaluation.fingerprint(getattr(args, label))["sha256"] != report["binaries"][label]["sha256"]
               for label in ("baseline", "candidate")):
            raise RuntimeError("binary changed during comparison")
        report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
        if isinstance(error, subprocess.CalledProcessError):
            report["docker_stderr"] = (error.stderr or "")[-2000:]
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    report["finished_at_utc"] = evaluation.datetime.now(evaluation.timezone.utc).isoformat()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, separators=(",", ":"), allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(args.report),
                      "error": report.get("error")}, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
