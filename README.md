# MiniVectorDB

MiniVectorDB is a from-scratch, single-process vector database for the 2026
OceanBase competition add-on task. It implements the required SQL subset,
durable storage, transactions, exact filtered TopK search, crash recovery, and
the Linux container stdin/stdout interface without using a database engine or a
third-party vector index.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/minivectordb --data-dir=/tmp/mvd
```

Docker:

```bash
docker build -t minivectordb .
docker run --rm -i -v mvd-data:/data minivectordb
```

The container runs in the foreground and does not open a network port.

## Protocol

- Input is UTF-8 SQL over stdin. Statements end with `;` and may span lines.
- `--`, `#`, and `/* ... */` comments are accepted.
- stdout contains only query rows or status/error records.
- `INSERT`, `UPDATE`, and `DELETE` return `OK <affected_rows>`.
- DDL, transaction control, and `SET` return `OK`.
- Errors return `ERROR <CODE>: <message>` by default.
- `--numeric-errors` returns MySQL-style numeric identifiers.
- `--ready-marker=TOKEN` appends the token after every response.
- `--startup-marker=TOKEN` emits a startup marker after recovery completes.

An interactive client should use a ready marker when zero-row SELECT results
must be distinguished from an unfinished response.

## SQL

Supported:

- `CREATE TABLE`
- `BIGINT PRIMARY KEY`, `VARCHAR(n)`, and `VECTOR(n)`
- multi-row `INSERT`, `SELECT`, `UPDATE`, and `DELETE`
- `=`, `<`, `<=`, `>`, `>=`, and `AND`
- projection, `SELECT *`, `ORDER BY`, `LIMIT`, and `OFFSET`
- `L2_DISTANCE(column, vector)` with aliases
- `BEGIN`, `COMMIT`, and `ROLLBACK`
- `CREATE VECTOR INDEX ... ON ...`
- `SET vector_mode = exact|ann`

`ann` mode currently uses the exact scan. This deliberately guarantees
Recall@K = 1.0 when an index has been created or updated.

Primary-key updates, NULLs, joins, aggregates, subqueries, OR, nested
transactions, ALTER TABLE, and other unsupported syntax return an error.

## Storage design

- `catalog.bin` is an append-only catalog whose records are checksummed.
- `t<oid>.dat` stores append-only row versions.
- `mvd.wal` stores commit records.
- A transaction buffers its final row changes in memory.
- Commit writes and `fsync`s all row versions first, then writes and `fsync`s a
  commit record before returning success.
- Startup truncates incomplete torn tails, reads committed transaction IDs, and
  replays only committed row versions in physical order.
- Uncommitted records are never made visible and are skipped again after a
  restart.
- The hot row cache is capped. When its budget is exceeded, payloads are read
  from disk by offset so correctness does not depend on fitting the whole table
  in memory.

## Vector search

Filtering is evaluated before distance calculation. Exact TopK uses a
size-bounded max heap, then sorts by `(distance, primary key)`. The required
`L2_DISTANCE` output is Euclidean distance. Distances are accumulated with an
AVX2/FMA runtime-dispatched kernel when supported and a scalar fallback
otherwise.

## Tests

```bash
python -X utf8 -B tests/run_public.py ./build/minivectordb
python -X utf8 -B tests/test_extended.py ./build/minivectordb
python -X utf8 -B tests/test_semantics.py ./build/minivectordb
python -X utf8 -B tests/test_disk_scan.py ./build/minivectordb
python -X utf8 -B bench/bench.py ./build/minivectordb
```

The public suite covers the currently published 32 SQL assertions. The extended
suite covers transaction visibility, rollback, statement atomicity, clean
restart, forced-process recovery, filtered TopK, vector-index update behavior,
mid-COMMIT all-or-none recovery, SQL boundary/error behavior, and a forced
out-of-cache disk scan. The benchmark follows the published
10,000-record / 128-dimensional / 1,000-query / K=10 workload shape.

The checked-in development measurement is in
`bench/results/windows-dev.json`: 2.069445 seconds for the write phase,
0.387468 seconds for 1,000 queries, and 1.0 sampled Recall@10 for both filtered
and unfiltered queries. These are not official Linux cgroup measurements.

## Limitations

- A value is kept inline in the append-only record; no page compaction is
  performed yet.
- Deleted or superseded versions remain in table files until a future
  checkpoint implementation.
- Approximate indexing is intentionally replaced by exact search for the first
  correctness-complete submission.
- The official hidden cases, complete driver, and official baseline are not yet
  public, so formal evaluation cannot be reproduced locally.
