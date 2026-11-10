"""Portable, bounded byte-pipe client for the official JSONL contract.

No selectors, PTYs, shell, build dependencies, protocol logs, or text markers.
Readers drain both pipes so Windows anonymous pipes and noisy stderr work too.
"""
from __future__ import annotations

from collections import deque
import json
import math
from pathlib import Path
import queue
import subprocess
import threading


class ProtocolError(AssertionError):
    pass


def strict_json(text):
    def constant(value):
        raise ValueError(f"non-standard JSON number: {value}")

    def floating(value):
        result = float(value)
        if not math.isfinite(result):
            raise ValueError(f"non-finite JSON number: {value}")
        return result

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    return json.loads(text, parse_constant=constant, parse_float=floating,
                      object_pairs_hook=pairs)


def same_json(actual, expected):
    """Compare recursively without accepting bools as ints, including in IDs."""
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, list):
        return len(actual) == len(expected) and all(
            same_json(a, b) for a, b in zip(actual, expected)
        )
    if isinstance(expected, dict):
        return actual.keys() == expected.keys() and all(
            same_json(actual[key], value) for key, value in expected.items()
        )
    return actual == expected


_AUTO_ID = object()


class JsonlClient:
    def __init__(self, argv, data_dir: Path, timeout=10.0):
        self.timeout = timeout
        self.sequence = 0
        self.last_line_size = 0
        self._lines = queue.Queue()
        self._stderr = deque(maxlen=16)
        self._writer = None
        self.process = subprocess.Popen(
            [*map(str, argv), f"--data-dir={data_dir}"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=65536,
        )
        self._readers = [
            threading.Thread(target=self._read_stdout, daemon=True),
            threading.Thread(target=self._read_stderr, daemon=True),
        ]
        for reader in self._readers:
            reader.start()
        try:
            # stdin has received no bytes: this also checks pre-input flushing.
            ready = self.read_message()
            if not same_json(ready, {"type": "ready"}):
                raise self.failure(f"expected exactly {{'type': 'ready'}}, got {ready!r}")
        except BaseException:
            self.close()
            raise

    def failure(self, message):
        tail = b"".join(self._stderr).decode("utf-8", errors="replace")[-2000:]
        return ProtocolError(f"{message}; stderr tail: {tail!r}")

    def _read_stdout(self):
        try:
            while True:
                # The official checker uses this same 16 MiB response bound.
                line = self.process.stdout.readline(16 * 1024 * 1024 + 1)
                if not line:
                    break
                self._lines.put(line)
        except Exception as error:
            self._lines.put(error)
        finally:
            self._lines.put(None)

    def _read_stderr(self):
        try:
            while True:
                chunk = self.process.stderr.read1(4096)
                if not chunk:
                    return
                self._stderr.append(chunk)
        except Exception as error:
            self._stderr.append(str(error).encode("utf-8", errors="replace"))

    def read_message(self):
        try:
            line = self._lines.get(timeout=self.timeout)
        except queue.Empty as error:
            raise self.failure("JSON response timeout (stdin remains open)") from error
        if line is None:
            raise self.failure("stdout reached EOF before the response")
        if isinstance(line, Exception):
            raise self.failure(f"stdout reader failed: {line}")
        self.last_line_size = len(line)
        if not line.endswith(b"\n"):
            raise self.failure(f"response lacks a flushed newline: {line[:240]!r}")
        try:
            message = strict_json(line.decode("utf-8"))
        except (ValueError, UnicodeError) as error:
            raise self.failure(f"invalid JSON response: {line[:240]!r}: {error}") from error
        if not isinstance(message, dict):
            raise self.failure(f"response is not a JSON object: {line[:240]!r}")
        return message

    def assert_quiet(self, timeout=0):
        try:
            extra = self._lines.get(timeout=timeout)
        except queue.Empty:
            return
        if isinstance(extra, bytes):
            extra = extra[:240]
        raise self.failure(f"unsolicited/duplicate stdout or premature EOF: {extra!r}")

    def send_bytes(self, payload):
        self.assert_quiet()
        completed = queue.Queue()

        def write():
            try:
                pending = memoryview(payload)
                while pending:
                    count = self.process.stdin.write(pending)
                    if not count:
                        raise BrokenPipeError("request write made no progress")
                    pending = pending[count:]
                self.process.stdin.flush()
                completed.put(None)
            except Exception as error:
                completed.put(error)

        # Even a request larger than a Windows pipe buffer has a bounded write.
        self._writer = threading.Thread(target=write, daemon=True)
        self._writer.start()
        try:
            error = completed.get(timeout=self.timeout)
        except queue.Empty as error:
            raise self.failure("JSON request write timeout") from error
        self._writer.join(timeout=self.timeout)
        if error is not None:
            raise self.failure(f"request write failed: {error}") from error

    def request(self, request, *, ensure_ascii=False):
        payload = (json.dumps(request, ensure_ascii=ensure_ascii, allow_nan=False)
                   + "\n").encode("utf-8")
        self.send_bytes(payload)
        response = self.read_message()
        if "id" not in response or not same_json(response["id"], request["id"]):
            raise self.failure(f"response id differs from request: {response.get('id')!r}")
        if type(response.get("ok")) is not bool:
            raise self.failure("response must contain boolean ok")
        return response

    def execute(self, sql, *, ident=_AUTO_ID, mode="exact", ensure_ascii=False):
        self.sequence += 1
        if ident is _AUTO_ID:
            ident = self.sequence
        return self.request(dict(id=ident, op="execute", sql=sql, search_mode=mode),
                            ensure_ascii=ensure_ascii)

    def shutdown(self, *, ident=_AUTO_ID):
        self.sequence += 1
        if ident is _AUTO_ID:
            ident = self.sequence
        response = self.request(dict(id=ident, op="shutdown"))
        if response["ok"] is not True:
            raise self.failure(f"shutdown failed: {response!r}")
        # Match the judge: close stdin after the response and wait for clean EOF.
        self.end_of_input()
        return response

    def end_of_input(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired as error:
            raise self.failure("database did not exit after stdin closed") from error
        for reader in self._readers:
            reader.join(timeout=self.timeout)
            if reader.is_alive():
                raise self.failure("pipe reader did not terminate")
        if self.process.returncode != 0:
            raise self.failure(f"nonzero exit: {self.process.returncode}")
        # Reader joins ensure even delayed output, partial lines and blank lines
        # after shutdown are observed; do not rely on a timing-only quiet check.
        extra = self._lines.get_nowait()
        if extra is not None or not self._lines.empty():
            if isinstance(extra, bytes):
                extra = extra[:240]
            raise self.failure(f"extra stdout after the final response: {extra!r}")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        threads = [*self._readers]
        if self._writer is not None:
            threads.append(self._writer)
        for thread in threads:
            thread.join(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if not stream.closed:
                stream.close()
        if any(thread.is_alive() for thread in threads):
            raise self.failure("owned pipe thread failed to terminate during cleanup")
