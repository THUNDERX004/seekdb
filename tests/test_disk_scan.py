#!/usr/bin/env python3
"""Exercise the non-cached, row-by-row disk scan path."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


class Client:
    def __init__(self, executable: Path, data_dir: Path, environment):
        self.process = subprocess.Popen(
            [
                str(executable),
                "--stdio=text",
                f"--data-dir={data_dir}",
                "--ready-marker=__END__",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=environment,
        )

    def execute(self, sql: str) -> list[str]:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(sql.rstrip().rstrip(";") + ";\n")
        self.process.stdin.flush()
        output = []
        while True:
            line = self.process.stdout.readline()
            if line == "":
                stderr = "" if self.process.stderr is None else self.process.stderr.read()
                raise AssertionError(f"database exited: {self.process.poll()} {stderr}")
            line = line.rstrip("\r\n")
            if line == "__END__":
                if output and output[-1].startswith("ERROR "):
                    raise AssertionError(output[-1])
                return output
            output.append(line)

    def close(self):
        if self.process.stdin is not None:
            self.process.stdin.close()
        self.process.wait(timeout=20)
        if self.process.returncode != 0:
            stderr = "" if self.process.stderr is None else self.process.stderr.read()
            raise AssertionError(stderr)


def make_vector(value: int) -> str:
    return "[" + str(value) + (",0" * 127) + "]"


def run(executable: Path):
    environment = os.environ.copy()
    environment["MVD_CACHE_LIMIT_MB"] = "1"
    data_dir = Path(tempfile.mkdtemp(prefix="mvd-disk-"))
    client = Client(executable, data_dir, environment)
    try:
        assert client.execute(
            "CREATE TABLE big (id BIGINT PRIMARY KEY, cat VARCHAR(8), "
            "emb VECTOR(128))"
        ) == ["OK"]
        row_count = 3000
        for start in range(1, row_count + 1, 100):
            rows = ",".join(
                f"({index},'{'even' if index % 2 == 0 else 'odd'}',"
                f"'{make_vector(index)}')"
                for index in range(start, min(row_count + 1, start + 100))
            )
            expected = f"OK {min(100, row_count - start + 1)}"
            assert client.execute(f"INSERT INTO big VALUES {rows}") == [expected]
        client.close()
    finally:
        try:
            client.close()
        except Exception:
            pass

    client = Client(executable, data_dir, environment)
    try:
        result = client.execute(
            "SELECT id, L2_DISTANCE(emb,'" + make_vector(0) + "') AS d "
            "FROM big ORDER BY "
            "L2_DISTANCE(emb,'" + make_vector(0) + "') LIMIT 10"
        )
        # This deliberately uses the expression order form to test parsing too.
        ids = [int(value.split("\t", 1)[0]) for value in result]
        assert ids == list(range(1, 11)), ids
        result = client.execute(
            "SELECT id, L2_DISTANCE(emb,'" + make_vector(0) + "') AS d "
            "FROM big WHERE cat = 'even' ORDER BY d LIMIT 10"
        )
        ids = [int(value.split("\t", 1)[0]) for value in result]
        assert ids == list(range(2, 21, 2)), ids
        # Hash traversal previously issued random seeks whenever a query had no
        # ORDER BY. Disk scans now follow current row offsets even though SQL
        # result ordering remains unspecified.
        result = client.execute("SELECT id FROM big")
        ids = sorted(int(value) for value in result)
        assert ids == list(range(1, row_count + 1)), ids[:20]
    finally:
        client.close()
        if os.name != "nt":
            shutil.rmtree(data_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    run(args.executable.resolve())
    print("PASS out-of-cache ordered, filtered TopK and unordered full scan")


if __name__ == "__main__":
    main()
