# MiniVectorDB

MiniVectorDB is a from-scratch, single-process vector database for the 2026
OceanBase competition add-on task. It implements the required SQL subset,
durable storage, transactions, exact filtered TopK search, crash recovery, and
the Linux container stdin/stdout interface.

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
- A complete statement responds immediately at its semicolon; an extra newline
  or EOF is not required. Empty/comment-only input produces no response.
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

The public suite covers 20 published groups / 95 SQL statements from official
round2 commit 9f6df2ba5acf871376e22b64a3d0724e4b575044, including restart, mode
and expected-error directives. Nine formal groups have withheld inputs. The extended
suite covers transaction visibility, rollback, statement atomicity, clean
restart, forced-process recovery, filtered TopK, vector-index update behavior,
mid-COMMIT all-or-none recovery, SQL boundary/error behavior, and a forced
out-of-cache disk scan. The benchmark follows the published
10,000-record / 128-dimensional / 1,000-query / K=10 workload shape.

The historical, pre-optimization development measurement is in
`bench/results/windows-dev.json`: 2.069445 seconds for the write phase,
0.387468 seconds for 1,000 queries, and 1.0 sampled Recall@10 for both filtered
and unfiltered queries. These are not official Linux cgroup measurements.

## Statement-failure tests

Additional statement-failure regressions can be run without changing the
production executable or enabling test fault hooks in it:

~~~bash
c++ -std=c++20 -O1 tests/test_statement_allocation.cpp -o build/test_statement_allocation
c++ -std=c++20 -O1 -DMVD_FORCE_SCALAR tests/test_statement_allocation.cpp -o build/test_statement_allocation_scalar
python -X utf8 -B tests/test_statement_allocation.py build/test_statement_allocation
python -X utf8 -B tests/test_round6_sql_contract.py build/minivectordb
python -X utf8 -B tests/test_round6_stream_contract.py build/minivectordb
~~~

The allocation test replaces global allocation only in its separate test
executable. It enumerates one allocation failure at a time and checks failed
statement isolation, successful execution, COMMIT/ROLLBACK, and restart. This
is not a cgroup memory-limit test or evidence of official hidden-case acceptance.
The full local validator accepts optional --allocation-normal and
--allocation-scalar paths to include both allocation-test executables.

## Limitations

- A value is kept inline in the append-only record; no page compaction is
  performed yet.
- Deleted or superseded versions remain in table files until a future
  checkpoint implementation.
- Approximate indexing is intentionally replaced by exact search for the first
  correctness-complete submission.
- The official hidden cases, complete driver, and official baseline are not yet
  public, so formal evaluation cannot be reproduced locally.

## Statement-validation corrections (2026-11-09)

See [the correction report](refine-logs/ROUND6_STATEMENT_FIXES_20261109.md)
for invalid transaction controls, VARCHAR length validation, and allocation-failure
statement isolation. The expanded local suite passed; official acceptance still
requires a result for the exact submitted revision.

## Historical 2026-11-09 optimization (commit 666989f7d)

See [the historical experiment report](refine-logs/EXPERIMENT_RESULTS_20261109_182245.md)
for that version; the latest report includes the incremental third round. The local
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
cgroup validation remain unexecuted on this host. The local query-preparation
experiment is documented in the report.

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

## Third-round optimization and validation (historical)

The public mysqltest package is merged unchanged from official round2 commit
9f6df2ba5acf871376e22b64a3d0724e4b575044. Compared locally against the previous
implementation commit 666989f7d, three paired Windows runs reduced median main
query time by 4.18% and write time by 2.76%. Out-of-row-cache query time improved
2.0738x with MVD_CACHE_LIMIT_MB=1; that is not an OS cold-cache or physical-I/O
claim. Main-workload memory peaks slightly increased and out-of-cache write
median slightly regressed. These are not official E, rankings or Linux results.

Changes defer projection until final selection, safely borrow synchronous
cache/overlay rows, move bounded heap candidates, scan ordered disk queries in
fixed offset-sorted metadata blocks, and reduce commit-time row duplication.
Post-write commit exceptions are fail-stop so recovery, not partial in-memory
publication, decides atomic visibility. Empty changes do not grow the WAL.

All 19 final validation commands passed: default and scalar published cases and
regressions, independent exact-order/selection tests, 24 public-adapter mocks,
and six commit-failure scenarios. Default differential coverage totals 33,104
SQL statements with zero mismatches; scalar ran the regular 6,070 statements,
not the additional 27,034-statement long sequence. Hidden official cases and
Docker/Linux/cgroup validation remain outstanding.

Run the additional suites or the whole local validation contract:

```bash
python -X utf8 -B tests/test_selection_paths.py ./build/minivectordb
cmake -S . -B build/fault -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DMVD_TEST_FAULTS
cmake --build build/fault --parallel
python -X utf8 -B tests/test_commit_publication.py ./build/minivectordb ./build/fault/minivectordb
python -X utf8 -B tests/validate_all.py --normal ./build/minivectordb --scalar ./build/scalar/minivectordb --fault ./build/fault/minivectordb --prefix refine-logs/local-validation-new
```

MVD_TEST_FAULTS and MVD_PROFILE are separate test/diagnostic-only compiler
definitions; neither is enabled in the default or scalar delivery build.
Fault environment variables do nothing in production. Profile clocks/counters
are not exclusive CPU stack samples and are excluded from main timing.
Four-row batch distances and a cached row-pointer directory were rejected
after paired measurements; neither is in the final implementation.

See [the current report](refine-logs/EXPERIMENT_RESULTS.md),
[raw validation transcript](refine-logs/round3-validation-final.txt), and
[profiling report](profile_output/PROFILE_REPORT.md). ARIS cross-model audit
remains REVIEW_UNAVAILABLE because its reviewer MCP is not registered. Local
engineering checks do not substitute for that verdict or formal evaluation.

## Fourth-round admission fixes and validation

The previous executable could block on a complete request with no byte after
its final semicolon. The input scanner now looks ahead only where needed.
Comment-only input no longer generates unsolicited errors or response markers.
Bare identifiers are no longer silently converted to VARCHAR text in INSERT,
UPDATE or predicates, preventing invalid statements from writing/deleting rows.

The expanded local validation passed all 23 commands on rebuilt default,
scalar and fault-injection binaries. This includes both public 20-group/95-SQL
runs, nine real-pipe framing tests, ten independent SQL edge tests, the previous
correctness/recovery suites and the long default differential sequence. Local
results do not prove the withheld formal cases or identify the platform's actual
failure without its necessary logs and locked commit SHA.

Three local paired main-workload runs reduced median write time by 8.47% and
query time by 1.29% against commit 3c1adfbf3. Out-of-row-cache query time regressed
2.61% in the first three runs and 0.25% in seven additional fixed-seed runs;
all ten combined regressed 1.01%. These negative samples are retained. All
queries and full-field restart checks passed, with exact-mode Recall@10=1.0
for both query groups. These are Windows development timings, not official
cgroup metrics, E or rankings.

Raw line/tab output remains unchanged. Arbitrary VARCHAR values containing
line/tab delimiters or a chosen marker need a reversible encoding/framing
contract with the official driver. Its complete protocol is not public here;
this submission does not claim that unresolved boundary is fixed. Linux Docker
build/run and cgroup checks also remain unexecuted on this host. No official
submission attempt was triggered during this round.

See refine-logs/EXPERIMENT_RESULTS_20261109_213756.md for all raw timing tables,
version hashes, known gaps and failure-to-fix evidence. New regression commands:

    python -X utf8 -B tests/test_stdio_contract.py ./build/minivectordb
    python -X utf8 -B tests/test_formal_sql_edges.py ./build/minivectordb

The existing tests/validate_all.py command now includes both new suites.
