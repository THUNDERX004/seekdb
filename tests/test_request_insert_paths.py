#!/usr/bin/env python3
"""Focused JSON SQL ownership and atomic batch INSERT checks for an existing binary."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import tempfile

from jsonl_client import JsonlClient, same_json


@contextmanager
def session(binary, directory):
    client = JsonlClient([binary], directory, timeout=30)
    try:
        yield client
        client.shutdown()
    finally:
        client.close()


def success(client, sql, *, affected=None, **options):
    response = client.execute(sql, **options)
    assert response.get("ok") is True, response
    if affected is not None:
        assert same_json(response.get("affected_rows"), affected), response
    return response


def failure(response, code):
    assert response.get("ok") is False, response
    assert response.get("error", {}).get("code") == code, response


def rows(client, table, expected, *, where=""):
    response = success(client, f"SELECT id,s,v FROM {table} {where} ORDER BY id")
    wanted = [[key, text, vector] for key, (text, vector) in sorted(expected.items())]
    assert same_json(response.get("columns"), ["id", "s", "v"]), response
    actual = response.get("rows")
    assert same_json(actual, wanted), f"{table}: rows differ; expected keys {sorted(expected)}"


def literal(text):
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def values(records):
    return ",".join(
        f"({key},{literal(text)},'{json.dumps(vector)}')"
        for key, text, vector in records
    )


def check_requests(client):
    success(client, "CREATE TABLE requests(id BIGINT PRIMARY KEY,s VARCHAR(65535),v VECTOR(2))")
    expected = {}
    # Grow, reuse and overwrite getline's buffer, including with short SELECTs.
    for key, length in enumerate((4097, 65535, 65535, 1025), 1):
        text, vector = chr(64 + key) * length, [key, key + 1]
        success(client, "INSERT INTO requests VALUES " + values([(key, text, vector)]), affected=1)
        expected[key] = (text, vector)
        rows(client, "requests", {key: expected[key]}, where=f"WHERE id={key}")

    cases = (
        (5, "prefix-" * 600 + "quote'\" slash\\ newline\n tab\t tail", False),
        (6, "原始中文与补充字符🌊", False),
        (7, "Unicode 转义中文🌊", True),
    )
    for key, text, ensure_ascii in cases:
        vector = [key, key + 1]
        success(client, "INSERT INTO requests VALUES " + values([(key, text, vector)]),
                affected=1, ensure_ascii=ensure_ascii)
        expected[key] = (text, vector)
    rows(client, "requests", expected)

    # A complete mutating SQL field must wait for validation of the whole frame.
    for key, suffix in ((80, b"} trailing"), (81, b',"tail":[1,]}')):
        ident = 9007199254740993 + key
        sql = "INSERT INTO requests VALUES " + values([(key, "must-not-appear", [8, 9])])
        frame = json.dumps(dict(id=ident, op="execute", sql=sql)).encode("utf-8")
        client.send_bytes(frame[:-1] + suffix + b"\n")
        response = client.read_message()
        assert same_json(response.get("id"), ident), response
        failure(response, "INVALID_REQUEST")
        rows(client, "requests", expected)
    return expected


def check_inserts(client):
    success(client, "CREATE TABLE inserts(id BIGINT PRIMARY KEY,s VARCHAR(64),v VECTOR(2))")
    lowest, highest = -(1 << 63), (1 << 63) - 1
    initial = [(highest, "maximum", [9, 8]), (0, "zero", [0, 1]),
               (lowest, "minimum", [1, 2])]
    success(client, "INSERT INTO inserts VALUES " + values(initial), affected=3)
    expected = {key: (text, vector) for key, text, vector in initial}
    rows(client, "inserts", expected)

    def reject(records):
        failure(client.execute("INSERT INTO inserts VALUES " + values(records)),
                "DUPLICATE_PRIMARY_KEY")
        rows(client, "inserts", expected)

    # New rows before a late duplicate must never become visible or durable.
    reject([(7, "first", [1, 2]), (-7, "middle", [3, 4]), (7, "duplicate", [5, 6])])
    reject([(8, "new", [1, 2]), (highest, "committed duplicate", [3, 4])])

    success(client, "BEGIN")
    success(client, f"UPDATE inserts SET s='prior-change' WHERE id={lowest}", affected=1)
    expected[lowest] = ("prior-change", [1, 2])
    success(client, "INSERT INTO inserts VALUES " + values([(5, "transaction row", [5, 6])]),
            affected=1)
    expected[5] = ("transaction row", [5, 6])
    reject([(9, "new", [1, 2]), (lowest, "committed duplicate", [3, 4])])
    reject([(10, "new", [1, 2]), (5, "overlay duplicate", [3, 4])])
    reject([(11, "first", [1, 2]), (-11, "middle", [3, 4]), (11, "duplicate", [5, 6])])

    success(client, "DELETE FROM inserts WHERE id=0", affected=1)
    replacements = [(12, "new positive", [1, 2]), (0, "reborn", [3, 4]),
                    (-12, "new negative", [5, 6])]
    success(client, "INSERT INTO inserts VALUES " + values(replacements), affected=3)
    expected.update({key: (text, vector) for key, text, vector in replacements})
    rows(client, "inserts", expected)
    success(client, "COMMIT")
    rows(client, "inserts", expected)

    success(client, "BEGIN")
    success(client, f"DELETE FROM inserts WHERE id={lowest}", affected=1)
    rolled_back = [(lowest, "rolled-back rebirth", [7, 8]), (13, "rolled-back row", [8, 9])]
    success(client, "INSERT INTO inserts VALUES " + values(rolled_back), affected=2)
    transient = dict(expected)
    transient.update({key: (text, vector) for key, text, vector in rolled_back})
    rows(client, "inserts", transient)
    success(client, "ROLLBACK")
    rows(client, "inserts", expected)
    return expected


def main(binary):
    binary = str(Path(binary).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="mvd-request-insert-") as temporary:
        directory = Path(temporary)
        with session(binary, directory) as client:
            request_rows = check_requests(client)
            insert_rows = check_inserts(client)
        with session(binary, directory) as client:
            rows(client, "requests", request_rows)
            rows(client, "inserts", insert_rows)
    print("PASS JSON SQL ownership, whole-frame validation, atomic INSERT and restart")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    raise SystemExit(main(parser.parse_args().binary))
