#!/usr/bin/env python3
"""Verify spool writeback failures, fail-stop replay and hook isolation."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


MARKER = b"__MVD_SPOOL_IO_END__\n"
HEADER = b"COLUMNS\tid\ts\n"
HOOK = "MVD_TEST_FAIL_RESULT_READ"
SYNC_HOOK = "MVD_TEST_FAIL_RESULT_SYNC"
ROWS = 256
WIDTH = 8192


def invoke(executable, directory, sql, hook=None, *, json_protocol=False, hook_name=HOOK):
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("MVD_TEST_")}
    environment["MVD_CACHE_LIMIT_MB"] = "8"
    if hook is not None:
        environment[hook_name] = str(hook)
    command = [str(executable), f"--data-dir={directory}"]
    if json_protocol:
        sql = b"".join((json.dumps({"id":index,"op":"execute","sql":statement,
                                  "search_mode":"exact"}) + "\n").encode()
                       for index, statement in enumerate(sql, 1))
    else:
        command += ["--stdio=text", "--column-header",
                    "--ready-marker=" + MARKER.decode().rstrip("\n")]
    return subprocess.run(
        command,
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
        json_query = ["SELECT id,s FROM t ORDER BY id;", "SELECT id FROM t WHERE id=1;"]
        ready = b'{"type":"ready"}\n'
        header = b'{"id":1,"ok":true,"columns":["id","s"],"rows":['
        body = b",".join(f'[{key},"{text}"]'.encode()
                         for key in range(1, ROWS + 1))
        complete = (ready + header + body + b"]}\n" +
                    b'{"id":2,"ok":true,"columns":["id"],"rows":[[1]]}\n')
        for block in (0, 1):
            failed = invoke(fault, directory, json_query, block, json_protocol=True)
            assert failed.returncode != 0, "JSON replay failure did not stop process"
            assert failed.stdout == ready + header + body[:block * 65536], repr(failed.stdout[:200])
            assert b"injected query result temporary read failure" in failed.stderr
            assert not list(Path(directory).glob(".mvd-result-*")), "JSON spool leaked"
            recovered = invoke(normal, directory, json_query, block, json_protocol=True)
            assert recovered.returncode == 0, recovered.stderr
            assert recovered.stdout == complete, "JSON production retry or hook isolation differs"
            assert not list(Path(directory).glob(".mvd-result-*")), "JSON recovery spool leaked"
            print(f"PASS JSON replay failure after {block * 65536} bytes, production retry and isolation")
        # Two copies cross the 4 MiB writeback boundary. Failure 0 occurs while
        # building the result; failure 1 occurs flushing its tail in prepare().
        sync_query = ["SELECT id,s,s FROM t ORDER BY id;", "SELECT id FROM t WHERE id=1;"]
        sync_body = b",".join(f'[{key},"{text}","{text}"]'.encode()
                              for key in range(1, ROWS + 1))
        assert len(sync_body) > 4 * 1024 * 1024
        sync_complete = (ready + b'{"id":1,"ok":true,"columns":["id","s","s"],"rows":['
                         + sync_body + b"]}\n" +
                         b'{"id":2,"ok":true,"columns":["id"],"rows":[[1]]}\n')
        for block in (0, 1):
            failed = invoke(fault, directory, sync_query, block, json_protocol=True,
                            hook_name=SYNC_HOOK)
            assert failed.returncode == 0, failed.stderr
            messages = [json.loads(line) for line in failed.stdout.splitlines()]
            assert len(messages) == 3 and messages[0] == {"type": "ready"}, messages[:1]
            assert messages[1]["id"] == 1 and messages[1]["ok"] is False, messages[1]
            assert messages[1]["error"]["code"] == "IO_ERROR", messages[1]
            assert "injected query result temporary sync failure" in messages[1]["error"]["message"]
            assert "rows" not in messages[1], "writeback failure leaked partial success"
            assert messages[2] == {"id": 2, "ok": True, "columns": ["id"], "rows": [[1]]}
            assert not list(Path(directory).glob(".mvd-result-*")), "sync failure spool leaked"
            recovered = invoke(normal, directory, sync_query, block, json_protocol=True,
                               hook_name=SYNC_HOOK)
            assert recovered.returncode == 0, recovered.stderr
            assert recovered.stdout == sync_complete, "writeback retry/hook isolation differs"
            assert not list(Path(directory).glob(".mvd-result-*")), "sync retry spool leaked"
            print(f"PASS writeback failure {block} returns one error before output, keeps session usable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
