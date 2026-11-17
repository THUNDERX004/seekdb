#!/usr/bin/env python3
"""INSERT column binding, error order, atomic batches and restart regression."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
from jsonl_client import JsonlClient, same_json


@contextmanager
def session(binary, directory):
    client = JsonlClient([binary], directory, timeout=30)
    try:
        yield client
        client.shutdown()
    finally:
        client.close()


def success(client, sql, affected=None):
    result = client.execute(sql)
    assert result.get("ok") is True, result
    if affected is not None:
        assert same_json(result.get("affected_rows"), affected), result
    return result


def failure(client, sql, code, message):
    result = client.execute(sql)
    assert result.get("ok") is False, result
    assert result.get("error", {}).get("code") == code, result
    assert result.get("error", {}).get("message") == message, result


def rows(client, expected, table="t"):
    result = success(client, f"SELECT id,s,n FROM {table} ORDER BY id")
    assert same_json(result.get("columns"), ["id", "s", "n"]), result
    assert same_json(result.get("rows"), expected), result


def main(binary):
    binary = str(Path(binary).resolve(strict=True))
    initial = [[1, "one", 10], [2, "two", 20], [3, "tri", 30]]
    prior = [[1, "edit", 10], [2, "two", 20], [3, "tri", 30], [8, "eight", 80]]
    # The fixture's actual VARCHAR limit is five; 'longer' is invalid.
    committed = [[1, "edit", 10], [2, "two", 20], [3, "tri", 30],
                 [4, "four", 40], [8, "eight", 80]]
    with tempfile.TemporaryDirectory(prefix="mvd-insert-binding-") as temporary:
        directory = Path(temporary)
        with session(binary, directory) as client:
            success(client, "CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(5),n BIGINT)")
            success(client, "INSERT INTO t(N,ID,S) VALUES(10,1,'one'),(20,2,'two'),(30,3,'tri')", 3)
            rows(client, initial)
            # Invalid earlier value wins; at the duplicate slot, duplicate wins.
            failure(client, "INSERT INTO t(id,ID,n) VALUES(bad,2,20)",
                    "TYPE_MISMATCH", "BIGINT requires an integer literal")
            failure(client, "INSERT INTO t(id,ID,n) VALUES(4,NULL,40)",
                    "TYPE_MISMATCH", "duplicate column in INSERT")
            failure(client, "INSERT INTO t(s,S,n) VALUES('longer',NULL,40)",
                    "VALUE_TOO_LONG", "VARCHAR value exceeds declared length")
            failure(client, "INSERT INTO t(id,s) VALUES(4,'four')",
                    "TYPE_MISMATCH", "INSERT must provide every column")
            failure(client, "INSERT INTO t VALUES(4,'four',40),(5,'five',50),(6,'longer',60)",
                    "VALUE_TOO_LONG", "VARCHAR value exceeds declared length")
            rows(client, initial)

            success(client, "BEGIN")
            success(client, "UPDATE t SET s='edit' WHERE id=1", 1)
            success(client, "INSERT INTO t VALUES(8,'eight',80)", 1)
            failure(client, "INSERT INTO t(N,ID,S) VALUES(40,4,'four'),(50,5,'five'),(60,6,'longer')",
                    "VALUE_TOO_LONG", "VARCHAR value exceeds declared length")
            rows(client, prior)
            # Reusing the failed batch's first key must still succeed.
            success(client, "INSERT INTO t(S,N,ID) VALUES('four',40,4)", 1)
            success(client, "COMMIT")
            rows(client, committed)

            success(client, "BEGIN")
            success(client, "UPDATE t SET n=99 WHERE id=2", 1)
            in_transaction = [[1, "edit", 10], [2, "two", 99], [3, "tri", 30],
                              [4, "four", 40], [8, "eight", 80]]
            failure(client, "INSERT INTO t VALUES(5,'five',50),(6,'six')",
                    "TYPE_MISMATCH", "column count does not match value count in INSERT")
            failure(client, "INSERT INTO t(id,s,n) VALUES(5,'five',50),(6,'six',60,NULL)",
                    "TYPE_MISMATCH", "too many values for INSERT")
            failure(client, "INSERT INTO t VALUES(5,'five',50),(4,'again',44)",
                    "DUPLICATE_PRIMARY_KEY", "primary key already exists")
            rows(client, in_transaction)
            success(client, "ROLLBACK")
            rows(client, committed)

            success(client, "CREATE TABLE u(n BIGINT,id BIGINT PRIMARY KEY,s VARCHAR(5))")
            success(client, "INSERT INTO u(S,ID,N) VALUES('nine',9,90),('ten',10,100)", 2)
            success(client, "INSERT INTO u VALUES(110,11,'elvn')", 1)
            rows(client, [[9, "nine", 90], [10, "ten", 100], [11, "elvn", 110]], "u")
        with session(binary, directory) as client:
            rows(client, committed)
            rows(client, [[9, "nine", 90], [10, "ten", 100], [11, "elvn", 110]], "u")
    print("PASS INSERT binding, error precedence, late batch failure, transactions and restart")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    main(parser.parse_args().binary)
