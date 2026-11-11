#!/usr/bin/env python3
"""Check Docker transactions, bulk updates, TopK and EOF restart under limits.

Build the image separately, then run this standard-library-only test:
  python -B tests/test_container_resources.py --image minivectordb:validation
Quick variant:
  python -B tests/test_container_resources.py --memory-mib 64 --dim 1024 --cache-mib 8

Defaults leave the image's cache environment unchanged. Current resource
failures are test failures, never expected failures or skipped successes.
The image uses its default JSONL protocol; large result rows are decoded one
at a time from the captured output file.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid


READY = {"type": "ready"}
OWNER_LABEL = "org.minivectordb.resource-test"
CLI_TIMEOUT = 20
SESSION_TIMEOUT = 300
BATCH_SIZE = 100
HEADER_LIMIT = 65536
READ_SIZE = 8192


class Failure(RuntimeError):
    pass


def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def vector(first, dim):
    return "[" + str(first) + ",0" * (dim - 1) + "]"


def write_execute(stream, ident, sql):
    request = {"id": ident, "op": "execute", "sql": sql, "search_mode": "exact"}
    stream.write((json.dumps(request, separators=(",", ":"), allow_nan=False)
                  + "\n").encode("ascii"))


def write_pending_delete(stream, ident):
    write_execute(stream, ident, "BEGIN;")
    write_execute(stream, ident + 1, "DELETE FROM t WHERE id=1;")
    # Deliberately no shutdown or COMMIT: the next session must recover this row.


def write_load(stream, args):
    write_execute(stream, 1, "CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,"
                  f"v VECTOR({args.dim}));")
    ident = 2
    for start in range(1, args.rows + 1, BATCH_SIZE):
        rows = ",".join(
            f"({row},{row % 7},'{vector(row, args.dim)}')"
            for row in range(start, min(start + BATCH_SIZE, args.rows + 1))
        )
        # Each INSERT is its own autocommitted statement; no giant transaction.
        write_execute(stream, ident, "INSERT INTO t VALUES" + rows + ";")
        ident += 1
    write_pending_delete(stream, ident)



def transaction_tail(args):
    statements = [
        ("UPDATE u SET tag=99;", {"affected_rows": args.rows}),
        ("SELECT id,tag FROM u ORDER BY id LIMIT 3;",
         {"columns": ["id", "tag"], "rows": [[i, 99] for i in range(1, min(3, args.rows) + 1)]}),
        ("COMMIT;", {}),
        ("BEGIN;", {}),
        ("UPDATE u SET tag=98;", {"affected_rows": args.rows}),
        ("DELETE FROM u WHERE id=1;", {"affected_rows": 1}),
        ("ROLLBACK;", {}),
        ("SELECT id,tag FROM u WHERE id=1;", {"columns": ["id", "tag"], "rows": [[1, 99]]}),
        ("BEGIN;", {}),
        ("UPDATE u SET v='" + vector(0, args.dim) + "' WHERE id=1;", {"affected_rows": 1}),
    ]
    first = 3 + (args.rows + BATCH_SIZE - 1) // BATCH_SIZE
    return [(ident, sql, {"id": ident, "ok": True, **expected})
            for ident, (sql, expected) in enumerate(statements, first)]


def write_transaction_load(stream, args):
    write_execute(stream, 1, "CREATE TABLE u(id BIGINT PRIMARY KEY,tag BIGINT,"
                  f"v VECTOR({args.dim}));")
    write_execute(stream, 2, "BEGIN;")
    for ident, start in enumerate(range(1, args.rows + 1, BATCH_SIZE), 3):
        values = ",".join(
            f"({row},0,'{vector(row, args.dim)}')"
            for row in range(start, min(start + BATCH_SIZE, args.rows + 1))
        )
        write_execute(stream, ident, "INSERT INTO u VALUES" + values + ";")
    for ident, sql, _ in transaction_tail(args):
        write_execute(stream, ident, sql)


def transaction_load_responses(args):
    result = [{"id": 1, "ok": True}, {"id": 2, "ok": True}]
    for ident, start in enumerate(range(1, args.rows + 1, BATCH_SIZE), 3):
        result.append({"id": ident, "ok": True,
                       "affected_rows": min(BATCH_SIZE, args.rows - start + 1)})
    result.extend(expected for _, _, expected in transaction_tail(args))
    return result


def transaction_recovery_cases(args):
    distance = "L2_DISTANCE(v,'" + vector(0, args.dim) + "') AS d"
    keys = list(range(1, min(3, args.rows) + 1))
    return [
        ("committed long transaction and EOF rollback",
         "SELECT id,tag," + distance + " FROM u WHERE id<=3 ORDER BY d LIMIT 3;",
         {"id": 1, "ok": True, "columns": ["id", "tag", "d"],
          "rows": [[i, 99, float(i)] for i in keys]}),
        ("autocommitted bulk update", "UPDATE u SET tag=100;",
         {"id": 2, "ok": True, "affected_rows": args.rows}),
        ("bulk update visible rows", "SELECT id,tag FROM u ORDER BY id LIMIT 3;",
         {"id": 3, "ok": True, "columns": ["id", "tag"],
          "rows": [[i, 100] for i in keys]}),
    ]


def pending_responses(ident):
    return [{"id": ident, "ok": True},
            {"id": ident + 1, "ok": True, "affected_rows": 1}]


def load_responses(rows):
    responses = [{"id": 1, "ok": True}]
    for ident, start in enumerate(range(1, rows + 1, BATCH_SIZE), 2):
        responses.append({"id": ident, "ok": True,
                          "affected_rows": min(BATCH_SIZE, rows - start + 1)})
    return responses


def typed_equal(actual, expected):
    if type(expected) is float:
        return type(actual) in (int, float) and actual == expected
    if type(expected) is list:
        return (type(actual) is list and len(actual) == len(expected)
                and all(typed_equal(a, b) for a, b in zip(actual, expected)))
    return type(actual) is type(expected) and actual == expected


def reject_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"non-finite JSON number: {value}")
    return result


def unique_pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


class JsonlReader:
    """Decode bounded metadata or a single row, never the full result line.

    This workload's names and numeric rows are ASCII. A physical newline may
    occur only after the response object, even when an array spans many reads.
    """

    def __init__(self, stream):
        self.stream = stream
        self.buffer = ""
        self.position = 0
        self.decoder = json.JSONDecoder(parse_constant=reject_constant,
                                        parse_float=finite_float,
                                        object_pairs_hook=unique_pairs)

    def fill(self):
        chunk = self.stream.read(READ_SIZE)
        try:
            self.buffer += chunk.decode("ascii")
        except UnicodeError as error:
            raise Failure(f"non-ASCII output at byte {self.position}") from error
        return bool(chunk)

    def consume(self, count):
        self.buffer = self.buffer[count:]
        self.position += count

    def space(self):
        while self.buffer or self.fill():
            count = len(self.buffer) - len(self.buffer.lstrip(" \t\r"))
            self.consume(count)
            if self.buffer:
                return

    def take(self, token):
        self.space()
        if self.buffer.startswith(token):
            self.consume(len(token))
            return True
        return False

    def require(self, token):
        if not self.take(token):
            raise Failure(f"expected {token!r} at byte {self.position}, "
                          f"received {self.buffer[:80]!r}")

    def value(self, limit=HEADER_LIMIT):
        self.space()
        while True:
            newline = self.buffer.find("\n")
            text = self.buffer if newline < 0 else self.buffer[:newline]
            try:
                value, end = self.decoder.raw_decode(text)
            except json.JSONDecodeError as error:
                if newline >= 0 or len(text) >= limit or not self.fill():
                    raise Failure(f"invalid or oversized JSON value at byte "
                                  f"{self.position}: {text[:80]!r}") from error
                continue
            except ValueError as error:
                raise Failure(f"invalid JSON at byte {self.position}: {error}") from error
            if end > limit:
                raise Failure(f"JSON value exceeds {limit} bytes at byte {self.position}")
            # raw_decode can accept a number before its final digits/exponent
            # have arrived. Require a delimiter before accepting the value.
            if end == len(text) and newline < 0:
                if self.fill():
                    continue
                raise Failure(f"unterminated JSON line at byte {self.position}")
            if end < len(text) and text[end] not in " \t\r,:]}":
                if newline < 0 and len(text) < limit and self.fill():
                    continue
                raise Failure(f"invalid JSON delimiter at byte {self.position + end}")
            self.consume(end)
            return value

    def response(self, expected):
        begin, streamed, seen = self.position, 0, set()
        self.require("{")
        if not self.take("}"):
            while True:
                key = self.value()
                if type(key) is not str or key in seen:
                    raise Failure(f"invalid or duplicate response key at byte {self.position}: {key!r}")
                seen.add(key)
                self.require(":")
                wanted = expected.get(key)
                if key == "rows" and callable(wanted):
                    before = self.position
                    wanted(self)
                    streamed += self.position - before
                else:
                    actual = self.value()
                    if key in expected and not typed_equal(actual, wanted):
                        raise Failure(f"{key}: expected {wanted!r}, received {actual!r}")
                    if key == "affected_rows" and type(actual) is not int:
                        raise Failure("affected_rows must be a JSON integer")
                    if key == "columns" and (type(actual) is not list
                                              or any(type(name) is not str for name in actual)):
                        raise Failure("columns must be an array of strings")
                if self.position - begin - streamed > HEADER_LIMIT:
                    raise Failure("response metadata exceeds byte limit")
                if self.take("}"):
                    break
                self.require(",")
        missing = expected.keys() - seen
        if missing:
            raise Failure(f"missing response fields: {sorted(missing)}")
        if expected == READY and seen != READY.keys():
            raise Failure("ready must contain exactly the type field")
        self.require("\n")

    def eof(self):
        if self.buffer or self.fill():
            raise Failure(f"unexpected output after the final response at byte {self.position}")


def ordered_rows(reader, expected_rows):
    reader.require("[")
    for index, expected in enumerate(expected_rows):
        if index:
            reader.require(",")
        actual = reader.value()
        if not typed_equal(actual, expected):
            raise Failure(f"row {index + 1}: expected {expected!r}, received {actual!r}")
    reader.require("]")


def query_cases(args):
    probe = (args.rows + 1) // 2
    distance = "L2_DISTANCE(v,'" + vector(probe, args.dim) + "') AS d"
    cases = []
    for label, predicate in (("exact TopK", ""), ("filtered TopK", " WHERE tag=3")):
        ids = (row for row in range(1, args.rows + 1)
               if not predicate or row % 7 == 3)
        nearest = sorted(ids, key=lambda row: (abs(row - probe), row))[:10]
        expected = [[row, row % 7, float(abs(row - probe))] for row in nearest]
        sql = f"SELECT id,tag,{distance} FROM t{predicate} ORDER BY d LIMIT 10;"
        cases.append((label, sql, {"id": len(cases) + 1, "ok": True,
                                  "columns": ["id", "tag", "d"],
                                  "rows": lambda reader, rows=expected: ordered_rows(reader, rows)}))
    # Deliberately no LIMIT, no vector projection, and no distance expression.
    cases.append(("ordinary full ordered query", "SELECT id,tag FROM t ORDER BY id;",
                  {"id": 3, "ok": True, "columns": ["id", "tag"],
                   "rows": lambda reader: ordered_rows(
                       reader, ([row, row % 7] for row in range(1, args.rows + 1)))}))
    # Two copies make even compact JSON exceed rows * dim * 4 bytes, hence
    # the configured hard limit, despite zero components formatting as "0".
    # No ORDER BY: retaining/sorting projected vectors is a separate limitation.
    cases.append(("large unordered vector projection", "SELECT id,tag,v,v FROM t;",
                  {"id": 4, "ok": True, "columns": ["id", "tag", "v", "v"],
                   "rows": lambda reader: vector_rows(reader, args)}))
    cases.append(("large ordered full projection", "SELECT * FROM t ORDER BY id;",
                  {"id": 5, "ok": True, "columns": ["id", "tag", "v"],
                   "rows": lambda reader: ordered_vector_rows(reader, args)}))
    return cases


def vector_rows(reader, args):
    """Check both vectors in every row and the complete unordered ID multiset."""
    seen = bytearray(args.rows + 1)
    # Allow ordinary decimal/exponent spellings while bounding one decoded row.
    row_limit = 128 * args.dim + 256
    reader.require("[")
    for index in range(args.rows):
        if index:
            reader.require(",")
        position = reader.position
        actual = reader.value(row_limit)
        if type(actual) is not list or len(actual) != 4:
            raise Failure(f"expected four cells at row {index + 1}, byte {position}")
        row, tag, *vectors = actual
        if type(row) is not int or not 1 <= row <= args.rows:
            raise Failure(f"invalid row ID at row {index + 1}, byte {position}: {row!r}")
        if seen[row]:
            raise Failure(f"duplicate row ID {row} at row {index + 1}, byte {position}")
        if not typed_equal(tag, row % 7):
            raise Failure(f"wrong tag for row ID {row}: {tag!r}")
        # Independent of vector(), and no full expected result is retained.
        for column, projected in enumerate(vectors, 3):
            if type(projected) is not list or len(projected) != args.dim:
                raise Failure(f"wrong vector dimension for row ID {row}, column {column}")
            for component, value in enumerate(projected):
                wanted = row if component == 0 else 0
                if type(value) not in (int, float) or value != wanted:
                    raise Failure(f"row ID {row}, column {column}, component {component}: "
                                  f"expected {wanted}, received {value!r}")
        seen[row] = 1
    # Exactly rows unique IDs in [1, rows] prove that every ID occurred once.
    reader.require("]")


def ordered_vector_rows(reader, args):
    """Verify a full vector projection ordered by ID without retaining it."""
    row_limit = 128 * args.dim + 256
    reader.require("[")
    for row in range(1, args.rows + 1):
        if row != 1:
            reader.require(",")
        position = reader.position
        actual = reader.value(row_limit)
        if type(actual) is not list or len(actual) != 3:
            raise Failure(f"expected three cells at row {row}, byte {position}")
        primary, tag, vector = actual
        if type(primary) is not int or primary != row:
            raise Failure(f"ordered row {row} has ID {primary!r}")
        if type(tag) is not int or tag != row % 7:
            raise Failure(f"wrong tag for ordered row {row}: {tag!r}")
        if type(vector) is not list or len(vector) != args.dim:
            raise Failure(f"wrong vector dimension for ordered row {row}")
        for component, value in enumerate(vector):
            wanted = row if component == 0 else 0
            if type(value) not in (int, float) or value != wanted:
                raise Failure(
                    f"ordered row {row}, component {component}: "
                    f"expected {wanted}, received {value!r}"
                )
    reader.require("]")


def output_problem(stream, phases):
    stream.seek(0)
    reader = JsonlReader(stream)
    for phase, expected in phases:
        try:
            for response in expected if type(expected) is list else [expected]:
                reader.response(response)
        except Failure as error:
            return phase, str(error)
    try:
        reader.eof()
    except Failure as error:
        return "EOF shutdown", str(error)
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
        command += [args.image, "--data-dir=/data"]
        print(f"RUN {label}: {name} (timeout {SESSION_TIMEOUT}s)", flush=True)
        # JSONL requests and captured output stay in files, not giant host buffers.
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
        loaded = load_responses(args.rows)
        containers.run(args, directory, "write and EOF", lambda stream: write_load(stream, args),
                       [("startup", READY), ("autocommitted batch writes", loaded),
                        ("uncommitted delete before EOF", pending_responses(len(loaded) + 1))])
        containers.run(args, directory, "bounded long transaction and bulk rollback",
                       lambda stream: write_transaction_load(stream, args),
                       [("startup", READY),
                        ("spilled INSERT, bulk UPDATE, COMMIT, ROLLBACK and EOF",
                         transaction_load_responses(args))])
        transaction_cases = transaction_recovery_cases(args)

        def write_transaction_recovery(stream):
            for ident, (_, sql, _) in enumerate(transaction_cases, 1):
                write_execute(stream, ident, sql)

        containers.run(args, directory, "transaction recovery and autocommitted bulk update",
                       write_transaction_recovery,
                       [("recovery", READY)] + [(name, expected) for name, _, expected in transaction_cases])

        def verify_bulk_commit(stream):
            write_execute(stream, 1, "SELECT id,tag FROM u WHERE id=1;")

        containers.run(args, directory, "bulk commit restart", verify_bulk_commit,
                       [("recovery", READY), ("committed scalar survives restart",
                         {"id": 1, "ok": True, "columns": ["id", "tag"], "rows": [[1, 100]]})])
        cases = query_cases(args)

        def write_queries(stream):
            for ident, (_, sql, _) in enumerate(cases, 1):
                write_execute(stream, ident, sql)
            write_pending_delete(stream, len(cases) + 1)

        for label in ("after load EOF", "after query EOF"):
            containers.run(args, directory, label, write_queries,
                           [("recovery", READY)] + [(name, expected) for name, _, expected in cases]
                           + [("uncommitted delete before EOF", pending_responses(len(cases) + 1))])
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
        print("PASS resource limits, long transactions, bulk updates, TopK, vector projection and EOF restart")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
