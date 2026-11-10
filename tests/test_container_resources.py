#!/usr/bin/env python3
"""Check Docker writes, TopK, large unordered output and EOF restart under limits.

Build the image separately, then run this standard-library-only test:
  python -B tests/test_container_resources.py --image minivectordb:validation
Quick variant:
  python -B tests/test_container_resources.py --memory-mib 64 --dim 1024 --cache-mib 8

Defaults leave the image's cache environment unchanged. Current resource
failures are test failures, never expected failures or skipped successes.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid


START = b"__MVD_RESOURCES_STARTED__\n"
END = b"__MVD_RESOURCES_END__\n"
OWNER_LABEL = "org.minivectordb.resource-test"
CLI_TIMEOUT = 20
SESSION_TIMEOUT = 300
BATCH_SIZE = 100


class Failure(RuntimeError):
    pass


def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def vector(first, dim):
    return "[" + str(first) + ",0" * (dim - 1) + "]"


def write_load(stream, args):
    stream.write(("CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,"
                  f"v VECTOR({args.dim}));\n").encode("ascii"))
    for start in range(1, args.rows + 1, BATCH_SIZE):
        rows = ",".join(
            f"({row},{row % 7},'{vector(row, args.dim)}')"
            for row in range(start, min(start + BATCH_SIZE, args.rows + 1))
        )
        # Each INSERT is its own autocommitted statement; no giant transaction.
        stream.write(("INSERT INTO t VALUES" + rows + ";\n").encode("ascii"))


def load_output(rows):
    output = bytearray(b"OK\n" + END)
    for start in range(1, rows + 1, BATCH_SIZE):
        output.extend(f"OK {min(BATCH_SIZE, rows - start + 1)}\n".encode("ascii"))
        output.extend(END)
    return bytes(output)


def query_cases(args):
    probe = (args.rows + 1) // 2
    distance = "L2_DISTANCE(v,'" + vector(probe, args.dim) + "') AS d"
    cases = []
    for label, predicate in (("exact TopK", ""), ("filtered TopK", " WHERE tag=3")):
        ids = (row for row in range(1, args.rows + 1)
               if not predicate or row % 7 == 3)
        nearest = sorted(ids, key=lambda row: (abs(row - probe), row))[:10]
        expected = "".join(
            f"{row}\t{row % 7}\t{abs(row - probe):.6f}\n" for row in nearest
        ).encode("ascii")
        sql = f"SELECT id,tag,{distance} FROM t{predicate} ORDER BY d LIMIT 10;\n"
        cases.append((label, sql, expected + END))
    # Deliberately no LIMIT, no vector projection, and no distance expression.
    expected = "".join(f"{row}\t{row % 7}\n" for row in range(1, args.rows + 1))
    cases.append(("ordinary full ordered query", "SELECT id,tag FROM t ORDER BY id;",
                  expected.encode("ascii") + END))
    # Two copies make even the text output exceed rows * dim * 4 bytes, hence
    # the configured hard limit, despite zero components formatting as "0".
    # No ORDER BY: retaining/sorting projected vectors is a separate limitation.
    cases.append(("large unordered vector projection", "SELECT id,tag,v,v FROM t;",
                  lambda stream: vector_output_problem(stream, args)))
    return cases


def vector_output_problem(stream, args):
    """Check an unordered ID multiset and regenerate just one expected row."""
    seen = bytearray(args.rows + 1)
    # Generate the oracle independently of the SQL vector() input helper.
    suffix = b",0" * (args.dim - 1) + b"]"
    line_limit = 4 * args.dim + 3 * len(str(args.rows)) + 32
    for index in range(args.rows):
        position = stream.tell()
        actual = stream.readline(line_limit + 1)
        identifier, separator, _ = actual.partition(b"\t")
        try:
            row = int(identifier) if separator else 0
        except ValueError:
            row = 0
        if not 1 <= row <= args.rows:
            return (f"missing or invalid row ID at row {index + 1}, byte {position}: "
                    f"{actual[:100]!r}")
        if seen[row]:
            return f"duplicate row ID {row} at row {index + 1}, byte {position}"
        vector_text = b"[" + str(row).encode("ascii") + suffix
        expected = (f"{row}\t{row % 7}\t".encode("ascii") + vector_text
                    + b"\t" + vector_text + b"\n")
        if actual != expected:
            offset = next((i for i, pair in enumerate(zip(actual, expected))
                           if pair[0] != pair[1]), min(len(actual), len(expected)))
            return (f"row ID {row} mismatch at byte {position + offset}: "
                    f"expected {expected[offset:offset + 100]!r}, "
                    f"received {actual[offset:offset + 100]!r}")
        seen[row] = 1
    # Exactly rows unique IDs in [1, rows] prove that every ID occurred once.
    actual = stream.read(len(END))
    if actual != END:
        return f"expected ready marker after all vector rows; received {actual[:100]!r}"
    return None


def output_problem(stream, phases):
    stream.seek(0)
    for phase, expected in phases:
        if callable(expected):
            problem = expected(stream)
            if problem:
                return phase, problem
            continue
        actual = stream.read(len(expected))
        if actual != expected:
            offset = next((i for i, pair in enumerate(zip(actual, expected))
                           if pair[0] != pair[1]), min(len(actual), len(expected)))
            return phase, (f"output mismatch at byte {offset}: "
                           f"expected {expected[offset:offset + 100]!r}, "
                           f"received {actual[offset:offset + 100]!r}")
    if stream.read(1):
        return "EOF shutdown", "unexpected output after the final response"
    return "EOF shutdown", None


def tail(stream):
    size = stream.seek(0, 2)
    stream.seek(max(0, size - 4096))
    return stream.read().decode("utf-8", errors="replace")


class OwnedContainers:
    def __init__(self, docker):
        self.docker = docker
        self.owner = uuid.uuid4().hex
        self.names = []

    def cli(self, *args):
        return subprocess.run([self.docker, *args], capture_output=True,
                              timeout=CLI_TIMEOUT, check=False)

    def checked(self, *args):
        result = self.cli(*args)
        if result.returncode:
            raise Failure(f"docker {' '.join(args)}: "
                          + result.stderr[-4096:].decode("utf-8", errors="replace"))
        return result

    def inspect(self, name):
        result = self.cli("container", "inspect", name)
        if result.returncode:
            error = result.stderr.decode("utf-8", errors="replace")
            if "No such" in error and name in error:
                return None
            raise Failure(f"inspect {name}: {error[-4096:]}")
        record = json.loads(result.stdout)[0]
        if (record.get("Config", {}).get("Labels") or {}).get(OWNER_LABEL) != self.owner:
            raise Failure(f"refusing to operate on container with wrong owner: {name}")
        return record

    def run(self, args, directory, label, writer, phases):
        name = f"mvd-resources-{self.owner}-{len(self.names) + 1}"
        self.names.append(name)
        command = [
            self.docker, "run", "--pull=never", "--interactive", "--name", name,
            "--label", f"{OWNER_LABEL}={self.owner}", "--network=none", "--cpus=2",
            f"--memory={args.memory_mib}m", f"--memory-swap={args.memory_mib}m",
            "--mount", f"type=bind,src={directory},dst=/data",
        ]
        if args.cache_mib is not None:
            command += ["--env", f"MVD_CACHE_LIMIT_MB={args.cache_mib}"]
        command += [args.image, "--data-dir=/data", "--vector-mode=exact",
                    "--startup-marker=" + START.decode("ascii").rstrip("\n"),
                    "--ready-marker=" + END.decode("ascii").rstrip("\n")]
        print(f"RUN {label}: {name} (timeout {SESSION_TIMEOUT}s)", flush=True)
        # SQL and captured output live in temporary files, not giant host buffers.
        with tempfile.TemporaryFile() as sql, tempfile.TemporaryFile() as output, \
                tempfile.TemporaryFile() as errors:
            writer(sql)
            sql.seek(0)
            process = subprocess.Popen(command, stdin=sql, stdout=output, stderr=errors)
            timed_out = False
            stop_errors = []
            try:
                process.wait(timeout=SESSION_TIMEOUT)
            except subprocess.TimeoutExpired:
                timed_out = True
            finally:
                if timed_out or process.poll() is None:
                    try:
                        record = self.inspect(name)
                        if record is not None and record["State"]["Running"]:
                            # Killing only the attached docker client leaves the DB alive.
                            self.checked("container", "kill", name)
                    except Exception as error:
                        stop_errors.append(str(error))
                    finally:
                        if process.poll() is None:
                            process.kill()
                        process.wait(timeout=5)
            phase, problem = output_problem(output, phases)
            record = self.inspect(name)
            state = None if record is None else record["State"]
            diagnostic = ("not created" if state is None else
                          f"OOMKilled={state.get('OOMKilled')} "
                          f"ExitCode={state.get('ExitCode')} Running={state.get('Running')}")
            print(f"STATE {name}: {diagnostic}", flush=True)
            if (timed_out or stop_errors or problem or process.returncode != 0
                    or state is None or state.get("Running")
                    or state.get("OOMKilled") or state.get("ExitCode") != 0):
                raise Failure(
                    f"{label} / {phase}: {problem or 'abnormal termination'}; "
                    f"timeout={timed_out}, client_exit={process.returncode}, {diagnostic}; "
                    f"stop_errors={stop_errors}; stderr={tail(errors)!r}"
                )
            for completed, _ in phases:
                print(f"PASS {label} / {completed}", flush=True)

    def cleanup(self):
        errors = []
        for name in reversed(self.names):
            try:
                # Recheck the unique ownership label before every destructive action.
                if self.inspect(name) is not None:
                    self.checked("container", "rm", "--force", name)
            except Exception as error:
                errors.append(f"{name}: {error}")
        return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="minivectordb:validation")
    parser.add_argument("--memory-mib", type=positive, default=256)
    parser.add_argument("--rows", type=positive, default=20000)
    parser.add_argument("--dim", type=positive, default=4096)
    parser.add_argument("--cache-mib", type=positive, default=None,
                        help="set MVD_CACHE_LIMIT_MB only when explicitly supplied")
    args = parser.parse_args()
    if args.rows > 2 ** 24:
        parser.error("row IDs must be exactly representable as float32 (rows <= 2**24)")
    payload = args.rows * args.dim * 4
    if payload <= args.memory_mib * 2 ** 20:
        parser.error("vector payload (rows * dim * 4) must exceed the memory limit")
    docker = shutil.which("docker")
    if docker is None:
        print("ERROR: Docker executable not found; container test was not run.", file=sys.stderr)
        return 2
    containers = OwnedContainers(docker)
    directory = Path(tempfile.mkdtemp(prefix="mvd-container-resources-")).resolve()
    status = 0
    try:
        cache = "image default (no env override)" if args.cache_mib is None else f"{args.cache_mib} MiB"
        print(f"WORKLOAD rows={args.rows} dim={args.dim} vector_payload={payload} bytes "
              f"memory={args.memory_mib} MiB cache={cache}", flush=True)
        containers.run(args, directory, "write and EOF", lambda stream: write_load(stream, args),
                       [("startup", START), ("autocommitted batch writes", load_output(args.rows))])
        cases = query_cases(args)

        def write_queries(stream):
            for _, sql, _ in cases:
                stream.write(sql.encode("ascii"))

        for label in ("after load EOF", "after query EOF"):
            containers.run(args, directory, label, write_queries,
                           [("recovery", START)] + [(name, expected) for name, _, expected in cases])
    except (Failure, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        status = 1
    finally:
        cleanup_errors = containers.cleanup()
        if cleanup_errors:
            print("FAIL cleanup: " + "; ".join(cleanup_errors), file=sys.stderr)
            print(f"Retained owned data directory: {directory}", file=sys.stderr)
            status = 1
        else:
            try:
                shutil.rmtree(directory)
            except OSError as error:
                print(f"FAIL cleanup of owned directory {directory}: {error}", file=sys.stderr)
                status = 1
    if status == 0:
        print("PASS container resource limits, TopK, ordered/unordered projection and EOF restart")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
