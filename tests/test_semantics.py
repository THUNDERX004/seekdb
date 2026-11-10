#!/usr/bin/env python3
"""Boundary and error-semantics tests for the required SQL subset."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


class Session:
    def __init__(self, executable: Path, data_dir: Path):
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
                return output
            output.append(line)

    def close(self):
        if self.process.stdin is not None:
            self.process.stdin.close()
        self.process.wait(timeout=10)
        if self.process.returncode != 0:
            raise AssertionError(f"database exited {self.process.returncode}")


def check(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: {actual!r} != {expected!r}")


def is_error(output: list[str]) -> bool:
    return len(output) == 1 and output[0].startswith("ERROR ")


def run(executable: Path):
    data_dir = Path(tempfile.mkdtemp(prefix="mvd-semantics-"))
    session = Session(executable, data_dir)
    try:
        check(
            session.execute(
                "/* multiline */ CREATE TABLE edge ("
                "id BIGINT PRIMARY KEY, "
                "name VARCHAR(1), "
                "v VECTOR(2))"
            ),
            ["OK"],
            "multiline create",
        )
        check(
            session.execute(
                "INSERT INTO edge VALUES "
                "(-9223372036854775808,'甲','[0,0]'),"
                "(2,'乙','[0,0]')"
            ),
            ["OK 2"],
            "bigint minimum",
        )
        check(
            session.execute(
                "SELECT id,name FROM edge ORDER BY id"
            ),
            ["-9223372036854775808\t甲", "2\t乙"],
            "unicode and bigint minimum projection",
        )
        check(
            session.execute(
                "SELECT id, L2_DISTANCE(v,'[0,0]') AS d "
                "FROM edge ORDER BY d"
            ),
            [
                "-9223372036854775808\t0.000000",
                "2\t0.000000",
            ],
            "distance tie uses primary-key order",
        )
        assert is_error(
            session.execute(
                "INSERT INTO edge VALUES (3,'丙','[NaN,0]')"
            )
        )
        assert is_error(
            session.execute(
                "INSERT INTO edge VALUES (3,'甲乙','[1,0]')"
            )
        )
        assert is_error(
            session.execute("SELECT missing FROM edge")
        )
        assert is_error(session.execute("SELECT id FROM edge WHERE id = 1 OR id = 2"))
        assert is_error(
            session.execute("UPDATE edge SET id = 3 WHERE id = 2")
        )

        check(session.execute("BEGIN"), ["OK"], "begin")
        check(
            session.execute(
                "INSERT INTO edge VALUES (5,'丙','[1,0]')"
            ),
            ["OK 1"],
            "valid insert in transaction",
        )
        assert is_error(
            session.execute(
                "INSERT INTO edge VALUES (6,'丁','[1,0]'),(6,'戊','[2,0]')"
            )
        )
        check(
            session.execute("SELECT id FROM edge WHERE id >= 5 ORDER BY id"),
            ["5"],
            "failed statement keeps prior transaction work only",
        )
        check(session.execute("COMMIT"), ["OK"], "commit")
        check(
            session.execute("SELECT id FROM edge WHERE id >= 5 ORDER BY id"),
            ["5"],
            "commit after failed statement",
        )
    finally:
        session.close()

    session = Session(executable, data_dir)
    try:
        check(
            session.execute("SELECT id FROM edge ORDER BY id"),
            ["-9223372036854775808", "2", "5"],
            "clean restart",
        )
    finally:
        session.close()
        if os.name != "nt":
            shutil.rmtree(data_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    run(args.executable.resolve())
    print("PASS SQL boundary, atomicity, and error semantics")


if __name__ == "__main__":
    main()
