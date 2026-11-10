#!/usr/bin/env python3
"""Run the published JSONL cases locally with Windows-compatible byte pipes.

Usage: python -X utf8 -B tests/test_official_json_public.py EXECUTABLE
Repeat with the scalar executable. --output optionally records one JSON report.
The official case driver and assertions are imported unchanged; this launcher
only replaces their selectors-based local transport. These are published local
checks, not hidden tests, container resource checks, or Linux validation.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]


def load_official():
    directory = ROOT / "protocol_check"
    spec = importlib.util.spec_from_file_location(
        "_minivectordb_official_json_public_run", directory / "run.py",
    )
    if spec is None or spec.loader is None:
        raise ImportError("cannot load official protocol_check/run.py")
    module = importlib.util.module_from_spec(spec)
    # The official driver uses sibling imports. Avoid borrowing another test's
    # same-named modules, and restore the importing process's module namespace.
    siblings = {name: sys.modules.pop(name, None)
                for name in ("_transport", "_assertions")}
    sys.path.insert(0, str(directory))
    try:
        spec.loader.exec_module(module)
        error = sys.modules["_transport"].ProtocolError
    finally:
        sys.path.pop(0)
        for name, previous in siblings.items():
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous
    return module, error


OFFICIAL, ProtocolError = load_official()
strict_json = OFFICIAL.strict_json


class Session:
    def __init__(self, executable, config, *, tmp_root=None):
        self.executable, self.config = executable, config
        self.deadline = time.monotonic() + config["case_timeout"]
        self.sequence = self.sql_count = 0
        self.proc = self.reader = self.writer = None
        self.ready = False
        self.buffer = b""
        self.stderr_tail = ""
        self.temp = tempfile.TemporaryDirectory(prefix="mvd-official-json-", dir=tmp_root)
        try:
            self.directory = Path(self.temp.name) / "data"
            self.directory.mkdir()
            self.log = (Path(self.temp.name) / "stderr.log").open("w+b", buffering=0)
        except BaseException:
            self.temp.cleanup()
            raise

    def expires(self, timeout):
        return min(self.deadline, time.monotonic() + timeout)

    def left(self, deadline):
        remaining = min(deadline, self.deadline) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("case or operation deadline exceeded")
        return remaining

    @staticmethod
    def drain_stdout(stream, events, stopped):
        def publish(value):
            while not stopped.is_set():
                try:
                    events.put(value, timeout=0.05)
                    return
                except queue.Full:
                    pass

        try:
            while not stopped.is_set():
                chunk = stream.read(65536)
                if not chunk:
                    break
                publish(chunk)
        except Exception as error:
            publish(error)
        finally:
            publish(None)

    def start(self):
        if self.proc is not None:
            raise ProtocolError("database is already started")
        deadline = self.expires(self.config["startup_timeout"])
        self.left(deadline)
        self.buffer = b""
        self.ready = False
        self.events = queue.Queue(maxsize=4)
        self.stopped = threading.Event()
        self.proc = subprocess.Popen(
            [str(self.executable), f"--data-dir={self.directory}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            bufsize=0, cwd=self.directory,
        )
        try:
            self.reader = threading.Thread(
                target=self.drain_stdout,
                args=(self.proc.stdout, self.events, self.stopped), daemon=True,
            )
            self.reader.start()
            # No stdin write is possible until this exact pre-input handshake.
            message = self.read(deadline)
            if message != {"type": "ready"}:
                raise ProtocolError(f"expected ready, got {message!r}")
            self.ready = True
        except BaseException:
            self.stop()
            raise

    def read(self, deadline):
        while b"\n" not in self.buffer:
            try:
                chunk = self.events.get(timeout=self.left(deadline))
            except queue.Empty as error:
                raise TimeoutError("JSON response timeout") from error
            if chunk is None:
                raise ProtocolError("database exited before a newline-terminated response")
            if isinstance(chunk, Exception):
                raise ProtocolError(f"stdout reader failed: {chunk}") from chunk
            self.buffer += chunk
            if len(self.buffer) > self.config["max_response_bytes"]:
                raise ProtocolError("response exceeds byte limit")
        self.left(deadline)
        line, self.buffer = self.buffer.split(b"\n", 1)
        try:
            value = strict_json(line.decode("utf-8"))
        except (ValueError, UnicodeError) as error:
            raise ProtocolError(
                f"invalid JSON response: {error}; stdout line (first 240 bytes)={line[:240]!r}"
            ) from error
        if not isinstance(value, dict):
            raise ProtocolError("response must be an object")
        return value

    def quiet(self, deadline):
        if self.buffer:
            raise ProtocolError("unsolicited or duplicate response")
        try:
            # Allow the reader to publish bytes already arriving on the pipe,
            # including a partial unsolicited line, before sending a request.
            self.events.get(timeout=min(0.01, self.left(deadline)))
        except queue.Empty:
            return
        raise ProtocolError("unsolicited or duplicate response, or premature stdout EOF")

    def send(self, request, deadline):
        if not self.ready or self.proc is None:
            raise ProtocolError("request before ready")
        self.quiet(deadline)
        payload = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        completed = queue.Queue()

        def write(stream):
            try:
                pending = memoryview(payload)
                while pending:
                    count = stream.write(pending)
                    if not count:
                        raise BrokenPipeError("request write made no progress")
                    pending = pending[count:]
                stream.flush()
                completed.put(None)
            except Exception as error:
                completed.put(error)

        self.writer = threading.Thread(target=write, args=(self.proc.stdin,), daemon=True)
        self.writer.start()
        try:
            error = completed.get(timeout=self.left(deadline))
        except queue.Empty as error:
            raise TimeoutError("request write timeout") from error
        self.writer.join(timeout=self.left(deadline))
        if self.writer.is_alive():
            raise TimeoutError("request writer did not terminate")
        self.writer = None
        if error is not None:
            raise ProtocolError(f"request write failed: {error}") from error

    def request(self, sql=None, mode="exact", op="execute", wait=True, ident=None):
        deadline = self.expires(self.config["request_timeout"])
        self.sequence += 1
        request = {"id": self.sequence if ident is None else ident, "op": op}
        if sql is not None:
            request.update(sql=sql, search_mode=mode)
        self.send(request, deadline)
        if not wait:
            return None
        response = self.read(deadline)
        if (type(response.get("id")) is not type(request["id"])
                or response["id"] != request["id"]):
            raise ProtocolError("response id differs from request")
        if type(response.get("ok")) is not bool:
            raise ProtocolError("missing boolean ok")
        if sql is not None and op == "execute":
            self.sql_count += 1
        return response

    def shutdown(self):
        deadline = self.expires(self.config["request_timeout"])
        response = self.request(op="shutdown")
        if response["ok"] is not True:
            raise ProtocolError("shutdown failed")
        self.ready = False
        self.proc.stdin.close()
        self.proc.wait(timeout=self.left(deadline))
        self.reader.join(timeout=self.left(deadline))
        if self.reader.is_alive():
            raise TimeoutError("stdout reader did not terminate after shutdown")
        eof = False
        extra = bool(self.buffer)
        while True:
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                break
            if event is None:
                eof = True
            else:
                extra = True
        if extra or not eof or self.proc.returncode:
            raise ProtocolError("extra stdout or nonzero exit after shutdown")
        self.stop()

    def stop(self):
        self.ready = False
        if self.proc is None:
            return
        self.stopped.set()
        # Cleanup has its own short bound, even when the case deadline expired.
        deadline = time.monotonic() + min(10, self.config["request_timeout"])
        errors = []
        try:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=max(0, deadline - time.monotonic()))
        except Exception as error:
            errors.append(str(error))
        finally:
            for worker in (self.writer, self.reader):
                if worker is not None and worker.ident is not None:
                    worker.join(timeout=max(0, deadline - time.monotonic()))
                    if worker.is_alive():
                        errors.append("owned pipe thread did not terminate")
            for stream in (self.proc.stdin, self.proc.stdout):
                if stream is not None:
                    try:
                        stream.close()
                    except Exception as error:
                        errors.append(str(error))
            if self.proc.poll() is not None:
                self.proc = self.reader = self.writer = None
        if errors:
            raise RuntimeError("; ".join(errors))

    def close(self):
        try:
            self.stop()
        finally:
            try:
                self.log.seek(max(0, self.log.seek(0, 2) - 4096))
                self.stderr_tail = self.log.read().decode("utf-8", errors="replace")
            finally:
                try:
                    self.log.close()
                finally:
                    self.temp.cleanup()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--tmp-root", type=Path, help="parent for owned temporary case directories")
    parser.add_argument("--output", type=Path, help="optional local JSON report file")
    args = parser.parse_args(argv)
    executable = args.executable.resolve(strict=True)
    package = ROOT / "mysqltest"
    config = strict_json((ROOT / "protocol_check/config.json").read_text(encoding="utf-8"))
    manifest = strict_json((package / "manifest.json").read_text(encoding="utf-8"))
    cases = manifest["cases"]
    expected_sql = sum(case["statements"] for case in cases)
    binary_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    print(f"binary={executable}\nbinary_sha256={binary_hash}", flush=True)
    results, aborted = [], None
    for case in cases:
        before = time.monotonic()
        row = dict(id=case["id"], status="PASS", message="", sql_responses=0,
                   expected_sql=case["statements"])
        session = None
        try:
            if aborted:
                row.update(status="SKIPPED", message=aborted)
            else:
                session = Session(executable, config, tmp_root=args.tmp_root)
                session.start()
                OFFICIAL.check_case(session, case, package, config)
                if session.sql_count != case["statements"]:
                    raise ValueError("manifest statement count differs from checked SQL count")
                session.shutdown()
        except KeyboardInterrupt:
            row.update(status="ERROR", message="interrupted by operator")
            aborted = "not run after interruption"
        except (TimeoutError, subprocess.TimeoutExpired) as error:
            row.update(status="TIMEOUT", message=str(error))
            aborted = "not run after first timeout"
        except Exception as error:
            row.update(status="FAIL", message=str(error))
        finally:
            if session is not None:
                try:
                    session.close()
                except Exception as error:
                    row.update(status="ERROR", message=row["message"] + "; cleanup failed: " + str(error))
                    aborted = "not run after cleanup failure"
                row["sql_responses"] = session.sql_count
                if row["status"] != "PASS":
                    row["stderr_tail"] = session.stderr_tail
        row["duration_ms"] = round((time.monotonic() - before) * 1000)
        results.append(row)
        print(f"{row['id']}: {row['status']} "
              f"({row['sql_responses']}/{row['expected_sql']} SQL) {row['message']}", flush=True)
        if row.get("stderr_tail"):
            print(row["stderr_tail"], file=sys.stderr, flush=True)
    passed_groups = sum(row["status"] == "PASS" for row in results)
    completed_sql = sum(row["sql_responses"] for row in results)
    passed = bool(cases) and passed_groups == len(cases) and completed_sql == expected_sql
    report = dict(
        scope="local-published-json-public", passed=passed,
        executable=str(executable), binary_sha256=binary_hash,
        manifest_version=manifest["version"], groups=len(cases), passed_groups=passed_groups,
        expected_sql=expected_sql, sql_responses=completed_sql, cases=results,
        note="Published SQL/assertion checks with local thread transport only; "
             "no hidden-case, container-resource, performance, or Linux validation claim.",
    )
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2)
                               + "\n", encoding="utf-8")
    print(f"{'PASS' if passed else 'FAIL'}: {passed_groups}/{len(cases)} published groups; "
          f"{completed_sql}/{expected_sql} SQL responses (local JSONL)", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
