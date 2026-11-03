# Full-text performance optimization

The latest local Windows native-client control measures a 10.18% category-equal
time reduction: build 4.48%, tokenization 8.21% and query 17.85%. The Chinese,
English and mixed COUNT queries improve 25.08%, 22.81% and 25.42%; the LIMIT
query regresses 1.93%. Both binaries ran the same complete fixed-parameter
benchmark twice in alternating order. Earlier Python/native-client controls
are retained below as separate experiments. The 50% overall reduction target
and a formal VLDB CI result have not been demonstrated.

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


## Additional native-client control

A subsequent original-binary control (baseline-after) still measured the LIMIT
query at 2.5435 ms with the Python client. A longer direct-connection probe
measured about 1.667 ms for that same query, motivating a separate client
overhead check rather than attributing all differences to the kernel.

The standalone Windows native batch client is retained with the test
instructions for reproducibility. It sends every statement and consumes every
result through the MySQL protocol; it contains no benchmark data, result
shortcuts or timing code. It is restricted to the dedicated passwordless
127.0.0.1:2891 test endpoint. Chinese input, quoting/comments, NULL, TOKENIZE
results and counts were compared with the existing client; result contents
matched (line endings differed). The official benchmark files stayed
byte-identical and all fixed parameters, SQL, repetitions and timer logic
were retained.

Two more complete original runs and two more complete candidate runs used
that same native client. These are a separate comparison; their times are
not averaged with the Python-client reports.

| Metric | Original native mean | Candidate native mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.58800 | 7.45900 | 1.70% |
| build_ik_content_sec | 6.04750 | 6.25200 | -3.38% |
| build_beng_en_sec | 3.61000 | 3.38650 | 6.19% |
| tokenize_ik_avg_ms | 0.13175 | 0.12785 | 2.96% |
| tokenize_beng_avg_ms | 0.08795 | 0.09250 | -5.17% |
| query_cn_avg_ms | 1.46700 | 1.15210 | 21.47% |
| query_beng_avg_ms | 2.06545 | 1.55965 | 24.49% |
| query_mixed_avg_ms | 1.64315 | 1.24600 | 24.17% |
| query_limit_avg_ms | 1.97755 | 1.90485 | 3.68% |

Native-client category reductions are build 1.50%, tokenize -1.11%
and query 18.45%; the category-equal mean is 6.28%. Negative metrics,
including the second candidate IK-content build and BENG tokenization, were
retained. The LIMIT result is sensitive to client/sampling conditions: this
native pair does not show the Python pair's regression. A robust LIMIT
improvement is not claimed. These data reinforce the ordinary-query benefit
but still do not demonstrate 50% overall reduction. Both original and
candidate again cap the fixed CI reference score locally; formal CI remains
unverified.

An additional per-document IK charset metadata cache passed the full result
regressions but changed the adjacent parser microbenchmark by only about 1%,
inside its sample spread. That experiment was rejected; the committed kernel
and candidate executable were restored exactly to the checkpoint hash.
Raw samples and the rejection decision are retained with the results.

## Further word aggregation and TOKENIZE configuration optimization

The word aggregation hot path called get_refactored followed by an overwriting
set_refactored for each word. This hashed and looked up the same word twice;
the overwrite also allocated a replacement hash node for repeated words.
It now uses the existing set_or_update operation. An in-place callback updates
both the frequency and key, retaining the established initial-count convention
and the latest spelling for collation-equivalent words.

TOKENIZE previously rebuilt/validated its parser properties, serialized that
JSON tree, and immediately parsed the serialization again when initializing
the parse helper. It now materializes the same properties directly from the
validated tree and passes the parsed parser identity/configuration to the
helper. Stored index configurations still use the existing string entry point.
That path and the typed entry point share property decoding and initialization;
the difference between an absent stored string and a nonempty JSON object
remains explicit. All argument, parser and property validation still runs.

A new regression records 768 complete configuration, grouping and error cases
on the independently verified original executable. It covers all five built-in
parsers, default/explicit output, custom and invalid settings, dynamic row
parameters, prepared execution, case/accent-equivalent keys and repeated words.
Exact outputs, including retained word spelling, are compared with the fixture.
It exposed that preserving the first spelling was incompatible: the original
overwriting hash insertion retained the latest spelling. The callback was
corrected before final performance runs. The new fixture was also rechecked
after restarting the original binary. The final candidate passed this suite,
the existing hot-path and batch suites, all 27 collation cases/52,963 result
rows, and two normal driver open/close cycles (3.877 seconds).

The complete benchmark ran in order candidate, original, candidate, original
with the same native client and the same CPU/memory/data-directory configuration
described above. Every run used SKIP_LOAD=0 and all fixed official parameters.
The initial attempt stopped at raw data load because the Windows Python
generator's default stdout encoding was not UTF-8; it produced no complete
report. PYTHONUTF8=1 and PYTHONIOENCODING=utf-8 were then set for all four
complete runs. Official scripts, SQL and data generation were unchanged and
their Git blob hashes were rechecked. No complete run or slow sample was
excluded, and no compilation or profiling ran alongside the measurements.

Build values are seconds; all other values are milliseconds. Each column is
the arithmetic mean of two complete runs.

| Metric | Original native mean | v19 candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.71200 | 7.47150 | 3.12% |
| build_ik_content_sec | 6.04250 | 5.92550 | 1.94% |
| build_beng_en_sec | 3.47850 | 3.35850 | 3.45% |
| tokenize_ik_avg_ms | 0.12805 | 0.12220 | 4.57% |
| tokenize_beng_avg_ms | 0.09115 | 0.08425 | 7.57% |
| query_cn_avg_ms | 1.44500 | 1.09700 | 24.08% |
| query_beng_avg_ms | 2.02215 | 1.52375 | 24.65% |
| query_mixed_avg_ms | 1.64125 | 1.21790 | 25.79% |
| query_limit_avg_ms | 1.85320 | 1.91865 | -3.53% |

The three category reductions are 2.83%, 6.07% and 17.75%; the category-equal
mean is 8.8841%. The unchanged scoring function gives 17.77/100 when supplied
the local original means as an analytical control. This derived comparison
does not replace the checked-in CI baseline. All four reports independently
cap the fixed CI reference score locally, including both originals; that
100/100 reference cannot be attributed to this patch. LIMIT regression and
run-to-run variation remain visible in the calculation. Hit counts remain
8001, 11000, 7332 and 20; the separate complete-result regressions establish
correctness.

Adjacent auxiliary parser diagnostics measured the previous checkpoint at
0.031966/0.009313 ms for IK/BENG and the final candidate at 0.029756/0.008069 ms.
These diagnostics support reduced parser overhead, but are not scoring data.
Intermediate diagnostic results, including a slower intermediate sample, are
also retained. The final native binary hash is
dee9a1fddd9e7fd26ba5ca6914261330bde05a8ad8b0219c884828b7aed22e5d.
Raw reports, sample logs, category calculations, diagnostics and provenance
are the v19 files in [the results directory](../assets/fts-performance/).

## Per-evaluation word maps without bucket locks

An auxiliary BENG instruction-pointer sample on the v19 checkpoint observed
word-map latch lock/unlock functions, alongside stopword checks, charset/hash
work and JSON output. There were only 73 samples, with 33 outside the server
image; this is directional evidence, not an exact CPU percentage. Source
inspection found every mutable word-map instance on the stack of one
synchronous document or query evaluation: TOKENIZE, domain-index row generation,
optimizer query parsing and both legacy/current query iterators.

ObFTWordMap now selects the existing NoPthreadDefendMode. The three query
callers that previously spelled out the locked template use the same alias.
No mutable word map is shared across evaluations; reusable parser helpers do
not own these maps. Shared dictionaries and the stopword set retain their
existing synchronization. All seven affected native compilation units were
rebuilt, including the optimizer and both query iteration implementations.

The adjacent parser diagnostic changed IK 0.029716 -> 0.027223 ms and BENG
0.007968 -> 0.006439 ms, approximately 8.39% and 19.19% reductions from v19.
An additional concurrent regression derives expected IDs independently from
127 input documents, records sequential full rankings, and verifies 1,440
complete ID/ranking/token results across eight independent sessions. This
checks that concurrent query parsing and document parsing remain isolated.
The final candidate passed it, all 768 configuration cases, the existing
408-token/144-ranking/920-predicate/144-concurrent-parse suite, all 280 batch
cases, and all 27 collation cases/52,963 result rows. Two normal driver
open/close cycles also passed in 4.114 seconds.

Two more complete original runs and two complete v20 runs used the same
unmodified native client, official fixed parameters and resource settings.
The order was candidate, original, candidate, original. A local wrapper initially
selected Windows PowerShell 5 and treated normal benchmark stderr progress as
an error before measurement. After switching to PowerShell 7, the first
candidate run completed; a stale Windows TCP listener snapshot then delayed
the next process switch. The port was verified free before resuming at the
next original run. No completed run was repeated, overwritten or excluded.
All samples, including slow English-query samples in both versions, remain
in the arithmetic means below. No compiler or profiler ran during timing.

Build values are seconds; tokenization and query values are milliseconds.

| Metric | Original native mean | v20 candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.68950 | 7.52200 | 2.18% |
| build_ik_content_sec | 6.03900 | 5.82500 | 3.54% |
| build_beng_en_sec | 3.59750 | 3.32000 | 7.71% |
| tokenize_ik_avg_ms | 0.12710 | 0.11645 | 8.38% |
| tokenize_beng_avg_ms | 0.08820 | 0.08110 | 8.05% |
| query_cn_avg_ms | 1.43010 | 1.07140 | 25.08% |
| query_beng_avg_ms | 2.08385 | 1.60845 | 22.81% |
| query_mixed_avg_ms | 1.63120 | 1.21660 | 25.42% |
| query_limit_avg_ms | 1.85730 | 1.89315 | -1.93% |

The category improvements are build 4.4785%, tokenize 8.2146% and query
17.8456%; their equal-weight mean is 10.1796%. Applying the unchanged formula
to this separate local control gives 20.36/100. Against the fixed CI reference,
all four local reports again cap at 100/100, including the originals. Formal
CI and a 50% overall reduction remain unproven. The native-client series are
kept separate from earlier experiments; the coarse profiles and micro timings
are not included in scoring.

The v20 files in [the results directory](../assets/fts-performance/) retain
all raw reports, sample logs, score calculations, diagnostics, the five-call-site
ownership audit and exact binary/source hashes. The final native binary hash is
3e1b144c5f8b243936c408270fd5b6f65994896bd0b89bd8b896af2724ce69a6.

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
