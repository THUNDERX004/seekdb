#!/usr/bin/env python3
"""Verify fail-stop output replay and production isolation of its test hook."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


MARKER = b"__MVD_SPOOL_IO_END__\n"
HEADER = b"COLUMNS\tid\ts\n"
HOOK = "MVD_TEST_FAIL_RESULT_READ"
ROWS = 256
WIDTH = 8192


def invoke(executable, directory, sql, hook=None):
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("MVD_TEST_")}
    environment["MVD_CACHE_LIMIT_MB"] = "8"
    if hook is not None:
        environment[HOOK] = str(hook)
    return subprocess.run(
        [str(executable), f"--data-dir={directory}", "--column-header",
         "--ready-marker=" + MARKER.decode().rstrip("\n")],
        input=sql, capture_output=True, env=environment, timeout=30, check=False,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("normal", type=Path)
    parser.add_argument("fault", type=Path)
    args = parser.parse_args()
    normal = args.normal.resolve(strict=True)
    fault = args.fault.resolve(strict=True)
    text = "x" * WIDTH
    query = b"SELECT id,s FROM t ORDER BY id;SELECT id FROM t WHERE id=1;"
    rows = b"".join(f"{key}\t{text}\n".encode() for key in range(1, ROWS + 1))
    first_result = HEADER + rows + MARKER
    all_results = first_result + b"COLUMNS\tid\n1\n" + MARKER
    with tempfile.TemporaryDirectory(prefix="mvd-result-spool-io-") as directory:
        sql = ("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8192));" +
               "INSERT INTO t VALUES" +
               ",".join(f"({key},'{text}')" for key in range(1, ROWS + 1)) + ";")
        loaded = invoke(normal, directory, sql.encode())
        assert loaded.returncode == 0, loaded.stderr
        assert loaded.stdout == b"OK\n" + MARKER + b"OK 256\n" + MARKER
        for block in (0, 1):
            failed = invoke(fault, directory, query, block)
            assert failed.returncode != 0, "injected read failure did not stop the process"
            assert failed.stdout == HEADER + rows[:block * 65536], repr(failed.stdout[:200])
            assert b"ERROR IO_ERROR:" in failed.stderr, failed.stderr
            assert b"injected query result temporary read failure" in failed.stderr
            assert not list(Path(directory).glob(".mvd-result-*")), "spool leaked"
            print(f"PASS replay failure after {block * 65536} bytes closes partial response")
            # A production process must ignore the test hook and preserve every
            # committed byte after the fault process's nonzero exit.
            recovered = invoke(normal, directory, query, block)
            assert recovered.returncode == 0, recovered.stderr
            assert recovered.stdout == all_results, "production hook isolation/recovery differs"
            assert not list(Path(directory).glob(".mvd-result-*")), "recovery spool leaked"
            print("PASS production isolation, full retry and committed-data recovery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
