# Full-text performance optimization

The measured benefit is a 4.45% category-equal reduction on the local Windows
control, with 16.55%, 16.96% and 19.16% reductions for the Chinese, English and
mixed COUNT queries. Build and tokenization changes are small relative to
run-to-run noise; the LIMIT query regressed 5.06% in the two-run mean.
The 50% overall reduction target has not been demonstrated.

## Bottlenecks and changes

- IK classified a character through several predicates that each decoded the
  same bytes. It now decodes once, preserving predicate precedence and UTF-16
  surrogate handling, and passes the already fetched character to processors.
- Every IK parser previously constructed immutable dictionary readers and
  cache-handle containers. The dictionary hub now lazily owns three readers
  and their pins. A cold-path mutex and acquire/release publication protect
  initialization; failed initialization is discarded and can be retried.
  Shutdown releases readers and pins before the backing dictionary/global
  cache is destroyed, after runtime users have stopped.
- BENG previously allocated a tokenizer/normalizer pipeline and copied each
  normalized token again. Embedded pipeline components now normalize directly
  into the caller's arena, keeping tokens alive after parser destruction.
  Whitespace classification combines the same ctype flags; ASCII character
  length bypasses the decoder only for encodings with minimum byte length one.
- Natural-language MATCH used only as a positive WHERE predicate performed
  BM25 estimation, read token frequencies and document lengths, and computed
  scores that the caller never consumed. Conservative expression-use analysis
  now allows score-free driving scans for a SELECT with one WHERE condition,
  no nested statements or column boosts, and no LIMIT/OFFSET. It recognizes
  BOOL(MATCH), MATCH > constrained zero, and zero < MATCH. Projection, ordering,
  boolean mode, nonzero thresholds, additional predicates, and functional
  lookups keep the scored path. Zero thresholds add plan-cache constraints.
- Score-free scans read posting IDs without BM25 aggregates or score-only
  columns. DaaT streams a deduplicated union for ordinary token counts and
  skips per-posting score collection/filter-scope setup. The >256-token TaaT
  fallback still deduplicates IDs, including overlap across posting lists.
  ID-only batch consumers initialize their skip vector; scalar projection and
  token-range rescan paths handle the absence of score expressions.
- Auxiliary build sampling observed collation comparison and sorter symbols
  among active server samples (8.176 s wall, 7.344 s process CPU). This was
  coarse sampling, not a reliable percentage attribution. Identical UTF-8
  general-ci sort keys now return equality without decoding; ASCII decoding
  avoids a function call while retaining the same Unicode weight mapping,
  multibyte validation, and end-space behavior. No sort-key collation changes.

The auxiliary parser microbenchmark used RAND-dependent input inside
BENCHMARK(10000, TOKENIZE(...)) to avoid constant folding. Earlier measurements
showed IK 0.03390 -> 0.03142 ms and BENG 0.00987 -> 0.00932 ms per invocation.
These are diagnostic measurements, not contest scoring metrics.

## Correctness validation

See [test instructions](../../../unittest/storage/fts/README.md).

The original executable was independently verified against source revision
e3c04befd2c9b6559f0005c4bf72c62e244cf06c. Fixtures were recorded only on that
executable and were rechecked on it after formatting the fixture.

The final candidate passed:

- 408 complete token/frequency/document-length cases, 144 ranked MATCH
  results, 920 predicate results and 144 concurrent first-use parses.
- 280 independently derived complete-ID checks on 2,051 documents per table,
  with simple/composite primary keys, retrieval batch boundaries, duplicate
  and >256-token queries, LIMIT/OFFSET, updates, deletes, and rollback.
- 27 shared-collation cases covering 52,963 complete result rows: pairwise
  comparisons, full/top-N sorting, grouping, distinct and index access.
- Two normal open/close cycles through the Windows public C driver, checking
  shared dictionary recreation and a persistent full-text index. One initial
  run returned SQL 1205 while acquiring an index-build snapshot; its log was
  retained. An unchanged isolated rerun passed in 3.900 s.
- Compilation and linking of all affected native unity units, plus a clean
  git diff --check.

The larger regression exposed an OFFSET/top-N runtime-filter projection
difference on an intermediate score-free candidate; the original passed the
same test. LIMIT/OFFSET statements now deliberately retain the established
scored path. The new regression and an explicit plan assertion prevent that
unsafe eligibility from returning.

## Measurement protocol and results

Measurements were collected on 2026-11-04, using a dedicated native Windows
instance on SQL port 2891/RPC port 2892, cpu_count=4, memory_budget=2G and
log_disk_size=2G. The installed service and its data were not used.

The same cached Bazel native compiler options and dependencies were used for
both binaries. Candidate native units were recompiled and linked with the
unchanged Rust/native dependencies. A fresh standard Bazel build was blocked
by cargo-bazel's Windows symlink privilege requirement; no clean Linux VLDB CI
build is claimed.

The current source checkout did not contain the contest benchmark or its
VLDB workflow. The five benchmark files were restored locally, unmodified,
from upstream/2026-competition at 5bce9e06d, and their Git blob hashes were
verified. They are not part of this optimization patch.

The unmodified official shell script, generator, setup SQL, timing and scoring
logic ran with ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3,
WARMUP=30 and SKIP_LOAD=0. Every full run reimported data and built all indexes
into a new report file. No build or profiler ran concurrently with measured
runs. A PyMySQL batch client supplied the local MySQL-compatible command
interface because mysql.exe was unavailable; this is a local comparison,
not an exact reproduction of the CI client/platform.

Raw reports are retained for both original runs (baseline-final,
baseline-paired) and both final candidate runs (candidate-v15 and its repeat).
Intermediate experiments and the initial noisy baseline were not substituted
for these controls. Report Git HEAD/dirty fields describe the checkout at
measurement time; actual executed binaries are identified by
[provenance](../assets/fts-performance/provenance.json).

Build metrics below are seconds; tokenization and query metrics are
milliseconds. Reductions use the arithmetic mean of the two complete runs
for each version. Negative reductions are retained.

| Metric | Original (mean of 2) | Candidate (mean of 2) | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.71450 | 7.64000 | 0.97% |
| build_ik_content_sec | 6.10150 | 6.12300 | -0.35% |
| build_beng_en_sec | 3.56000 | 3.50100 | 1.66% |
| tokenize_ik_avg_ms | 0.22915 | 0.22385 | 2.31% |
| tokenize_beng_avg_ms | 0.18430 | 0.18605 | -0.95% |
| query_cn_avg_ms | 2.19240 | 1.82955 | 16.55% |
| query_beng_avg_ms | 2.74000 | 2.27535 | 16.96% |
| query_mixed_avg_ms | 2.37995 | 1.92400 | 19.16% |
| query_limit_avg_ms | 2.56005 | 2.68960 | -5.06% |

Category reductions are build 0.76%, tokenize 0.68% and query 11.90%.
Their equal-weight mean is 4.45%; using the contest formula against the local
control gives 8.89/100. This comparison quantifies the patch, not a formal
contest result. The four auxiliary hit counts remained 8001, 11000, 7332 and 20;
the complete-result regressions above provide the correctness evidence.

Running the unchanged scorer against the fixed CI baseline gives 100/100
for both final local reports. The unmodified original also reaches the cap
locally. Thus that reference score must not be attributed to this patch:
platform and source-version differences dominate that cross-environment
comparison. Formal performance/scoring still requires the supplied VLDB CI
environment. The nine metrics were not averaged directly.

The raw reports, category comparison and benchmark hashes are under
[the results directory](../assets/fts-performance/).

## Reproduction

Use a dedicated instance. With an ordinary MySQL-compatible client and the
unchanged competition benchmark available, run from the repository root:

~~~bash
export MYSQL='mysql -h127.0.0.1 -P2891 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2891 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
~~~

Use the same hardware, compiler flags, server resource settings and client
for an unmodified control and a candidate. Keep each report file fresh.
The benchmark deletes/recreates fts_large_bench. Tests and build/profile
activity should finish before timing begins.
