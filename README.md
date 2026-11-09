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

The historical, pre-optimization development measurement is in
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

## 2026-11-09 optimization and robustness update

See [the experiment report](refine-logs/EXPERIMENT_RESULTS.md) for all paired
rounds, hashes, retained historical results, and validation limits. The local
comparison baseline is commit 3659b39d6de0537fb81bd08a70b6f580c4e26f37, not
the unpublished official performance baseline.

The optimized implementation uses checked float32 parsing, bulk little-endian
serialization, an equivalent table-driven CRC32, untied stdin/stdout, a reused
file stream for disk scans, PK point lookup, and a budgeted ordinary-column
equality candidate index. The candidate index is invalidated on committed
changes and combined with the current transaction overlay. Row cache accounting
uses actual value capacities, is divided across tables, and releases row storage
when disabled. Set MVD_CACHE_LIMIT_MB to a positive integer to change the
default 192 MiB row-cache budget. This is not a process/cgroup memory limit:
PK metadata, transaction overlays, output rows and OS file caches also consume
memory. There is no automatic cgroup budget discovery.

Distance subtraction and accumulation use double precision. The exact TopK
scan prepares the constant probe and chooses the CPU kernel once per query.
When approximate distances are within a conservative rounding-error bound, a
from-scratch integer accumulator compares squared distances exactly for the
stored float32 values. This avoids false ties for mixed large/small components,
including scalar/SIMD accumulation differences. Distance text still uses the
documented floating-point tolerance. Bounded selection preserves all ORDER BY
keys, then the PK tie-break; it is not restricted to the first key.

Recovery no longer reuses a transaction ID left by an interrupted uncommitted
row record. Additional checks cover aliases, unprojected distance ordering,
extreme LIMIT/OFFSET, unsupported VECTOR ordering, duplicate DDL, NULL atomicity
and long predicate strings without weakening stored VARCHAR length checks.

The optional --column-header flag emits a COLUMNS record with tab-separated
real column names, even for zero-row SELECTs. The default protocol is unchanged.
The public adapter now checks names/order, values, affected-row counts and
successful status for empty expectation objects.

Additional validation commands (Python 3, NumPy needed only for evaluate.py):

```bash
python -X utf8 -B tests/test_hidden_edges.py ./build/minivectordb
python -X utf8 -B tests/test_differential.py ./build/minivectordb
python -X utf8 -B tests/test_exact_distance.py ./build/minivectordb
python -X utf8 -B tests/test_public_harness.py
python -X utf8 -B bench/evaluate.py --baseline /path/to/baseline/minivectordb --candidate ./build/minivectordb --runs 3 --report bench/results/local.json
cmake -S . -B build/scalar -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DMVD_FORCE_SCALAR
cmake --build build/scalar --parallel
```

MVD_FORCE_SCALAR bypasses the explicit AVX2/FMA distance dispatch; it is a
compiler definition, not an environment switch or a promise that dependencies
contain no AVX instructions. The normal build performs runtime CPU detection.

The Dockerfile pins GCC 14.3.0/bookworm and Debian bookworm-slim by digest and
builds from source, statically linking libstdc++/libgcc. Docker build and Linux
cgroup validation remain unexecuted on this host. The source does not invoke
Faiss/HNSWlib/VSAG; open-source design references and the retained local
query-preparation experiment are documented in the report.

## Empty-directory usage and restart

Run the same command again with the same data directory after EOF to recover:

```bash
./build/minivectordb --data-dir=/tmp/mvd-demo
```

Send SQL over stdin, then close stdin (EOF) to exit:

```sql
CREATE TABLE docs(id BIGINT PRIMARY KEY, tag VARCHAR(16), emb VECTOR(2));
INSERT INTO docs VALUES(1,'a','[3,4]'),(2,'b','[0,0]');
SELECT id,tag FROM docs ORDER BY id;
SELECT id,L2_DISTANCE(emb,'[0,0]') AS d FROM docs ORDER BY d LIMIT 2;
BEGIN;
UPDATE docs SET tag='kept' WHERE id=1;
COMMIT;
BEGIN;
DELETE FROM docs WHERE id=2;
ROLLBACK;
```

After restarting, SELECT id,tag FROM docs ORDER BY id returns both committed
rows; the rolled-back DELETE has no effect. Use a fresh directory for CREATE.
