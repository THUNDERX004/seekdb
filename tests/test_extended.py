#!/usr/bin/env python3
"""Extended MiniVectorDB transaction, vector, and recovery tests."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


class Session:
    def __init__(self, executable: Path, data_dir: Path):
        self.process = subprocess.Popen(
            [
                str(executable),
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
        lines: list[str] = []
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = self.process.stdout.readline()
            if line == "":
                stderr = ""
                if self.process.stderr is not None:
                    stderr = self.process.stderr.read()
                raise AssertionError(
                    f"database exited during {sql!r}: {self.process.poll()}, {stderr}"
                )
            line = line.rstrip("\r\n")
            if line == "__END__":
                return lines
            lines.append(line)
        raise AssertionError(f"timeout waiting for {sql!r}")

    def close(self):
        if self.process.poll() is not None:
            return
        assert self.process.stdin is not None
        self.process.stdin.close()
        self.process.wait(timeout=10)
        if self.process.returncode != 0:
            stderr = "" if self.process.stderr is None else self.process.stderr.read()
            raise AssertionError(f"database exited {self.process.returncode}: {stderr}")

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)

    def write_without_waiting(self, sql: str):
        assert self.process.stdin is not None
        self.process.stdin.write(sql.rstrip().rstrip(";") + ";\n")
        self.process.stdin.flush()


def expect(actual, wanted, label: str):
    if actual != wanted:
        raise AssertionError(f"{label}\nactual={actual!r}\nexpected={wanted!r}")


def run(executable: Path):
    data_dir = Path(tempfile.mkdtemp(prefix="mvd-extended-"))
    try:
        session = Session(executable, data_dir)
        try:
            expect(
                session.execute(
                    "CREATE TABLE docs (id BIGINT PRIMARY KEY, "
                    "cat VARCHAR(16), body VARCHAR(32), emb VECTOR(4))"
                ),
                ["OK"],
                "create",
            )
            expect(
                session.execute(
                    "INSERT INTO docs VALUES "
                    "(1,'a','one','[0,0,0,0]'),"
                    "(2,'b','two','[10,0,0,0]'),"
                    "(3,'a','three','[2,0,0,0]'),"
                    "(4,'a','four','[1,0,0,0]')"
                ),
                ["OK 4"],
                "insert vector rows",
            )
            expect(
                session.execute(
                    "SELECT id FROM docs WHERE cat = 'a' "
                    "ORDER BY id DESC"
                ),
                ["4", "3", "1"],
                "ordinary filter and descending order",
            )
            expect(
                session.execute(
                    "SELECT id, L2_DISTANCE(emb, '[0,0,0,0]') AS d "
                    "FROM docs WHERE cat = 'a' ORDER BY d LIMIT 2"
                ),
                ["1\t0.000000", "4\t1.000000"],
                "filter-before-topk",
            )
            expect(
                session.execute(
                    "CREATE VECTOR INDEX ix ON docs(emb)"
                ),
                ["OK"],
                "create vector index",
            )
            expect(
                session.execute(
                    "UPDATE docs SET emb = '[0,0,0,0]' WHERE id = 2"
                ),
                ["OK 1"],
                "update indexed vector",
            )
            expect(
                session.execute(
                    "SELECT id, L2_DISTANCE(emb, '[0,0,0,0]') AS d "
                    "FROM docs ORDER BY d LIMIT 3"
                ),
                ["1\t0.000000", "2\t0.000000", "4\t1.000000"],
                "distance tie by primary key",
            )
            expect(
                session.execute(
                    "SELECT id, L2_DISTANCE(emb, '[0,0,0,0]') AS d "
                    "FROM docs ORDER BY L2_DISTANCE(emb, '[0,0,0,0]') LIMIT 2"
                ),
                ["1\t0.000000", "2\t0.000000"],
                "order by distance expression",
            )
            expect(session.execute("BEGIN"), ["OK"], "begin")
            expect(
                session.execute(
                    "INSERT INTO docs VALUES (5,'z','five','[5,0,0,0]')"
                ),
                ["OK 1"],
                "transaction insert",
            )
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["1", "2", "3", "4", "5"],
                "read own writes",
            )
            expect(session.execute("ROLLBACK"), ["OK"], "rollback")
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["1", "2", "3", "4"],
                "rollback visibility",
            )
            expect(session.execute("BEGIN"), ["OK"], "begin again")
            expect(
                session.execute(
                    "INSERT INTO docs VALUES (6,'z','six','[6,0,0,0]')"
                ),
                ["OK 1"],
                "transaction insert again",
            )
            expect(session.execute("COMMIT"), ["OK"], "commit")
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["1", "2", "3", "4", "6"],
                "commit visibility",
            )
            error = session.execute(
                "INSERT INTO docs VALUES "
                "(7,'x','seven','[7,0,0,0]'),"
                "(7,'y','duplicate','[8,0,0,0]')"
            )
            assert len(error) == 1 and error[0].startswith("ERROR "), error
            expect(
                session.execute("SELECT id FROM docs WHERE id = 7"),
                [],
                "failed statement leaves no partial rows",
            )
        finally:
            session.close()

        session = Session(executable, data_dir)
        try:
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["1", "2", "3", "4", "6"],
                "persistence after clean restart",
            )
            expect(
                session.execute(
                    "INSERT INTO docs VALUES (8,'z','eight','[8,0,0,0]')"
                ),
                ["OK 1"],
                "persist committed row",
            )
            expect(session.execute("BEGIN"), ["OK"], "begin crash test")
            expect(
                session.execute(
                    "INSERT INTO docs VALUES (9,'z','nine','[9,0,0,0]')"
                ),
                ["OK 1"],
                "uncommitted insert",
            )
            session.kill()
        finally:
            session.kill()

        session = Session(executable, data_dir)
        try:
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["1", "2", "3", "4", "6", "8"],
                "crash recovery excludes uncommitted row",
            )
            expect(session.execute("BEGIN"), ["OK"], "begin before crash")
            expect(
                session.execute("DELETE FROM docs WHERE id = 1"),
                ["OK 1"],
                "delete in transaction",
            )
            expect(session.execute("COMMIT"), ["OK"], "commit delete")
            session.kill()
        finally:
            session.kill()

        session = Session(executable, data_dir)
        try:
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["2", "3", "4", "6", "8"],
                "recover committed delete",
            )
            expect(session.execute("BEGIN"), ["OK"], "begin mid-commit crash")
            rows = ",".join(
                f"({10000 + index},'z','bulk{index}','[1,2,3,4]')"
                for index in range(400)
            )
            expect(
                session.execute(f"INSERT INTO docs VALUES {rows}"),
                ["OK 400"],
                "large pending transaction",
            )
            session.write_without_waiting("COMMIT")
            time.sleep(0.0005)
            session.kill()
        finally:
            session.kill()

        session = Session(executable, data_dir)
        try:
            recovered = session.execute(
                "SELECT id FROM docs WHERE id >= 10000 ORDER BY id"
            )
            wanted = [str(10000 + index) for index in range(400)]
            assert recovered in ([], wanted), (
                "mid-commit crash produced a partial transaction: "
                f"{len(recovered)} rows"
            )
            expect(
                session.execute("SELECT id FROM docs ORDER BY id"),
                ["2", "3", "4", "6", "8"] + recovered,
                "database remains usable after mid-commit recovery",
            )
        finally:
            session.close()
    finally:
        if os.name != "nt":
            shutil.rmtree(data_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    run(args.executable.resolve())
    print("PASS extended transaction, vector, persistence, and recovery tests")


if __name__ == "__main__":
    main()
