# Full-text hot-path regressions

> Competition validation (2026-11-04): both the unmodified official
> `upstream/2026-competition` at `5bce9e06d1281347ad61344e4b01fab9158c58a2`
> and the migrated candidate passed all seven SQL integration suites.
> The four fixtures were independently recorded again on the official original,
> then used unchanged for candidate comparisons. Windows x64 CMake builds use
> the same local configuration with physical standby disabled; formal Linux CI
> and competition performance scoring are not established by these checks.
> See [migration evidence](../../../docs/developer-guide/assets/fts-performance/competition-migration.json).

These integration tests require Python 3, PyMySQL, and a dedicated running
seekdb instance. The suites create/drop only fts_hot_path_regression,
fts_collation_regression, fts_match_batches and fts_map_concurrency_regression. The default SQL endpoint is 127.0.0.1:2891;
override --host, --port, or --user as needed. Read the password from
SEEKDB_TEST_PASSWORD; do not put it in a command line or fixture.

~~~bash
python3 unittest/storage/fts/test_tokenize_config.py --port 2891
python3 unittest/storage/fts/test_hot_path.py --port 2891 --check-plans
python3 unittest/storage/fts/test_collation.py --port 2891
python3 unittest/storage/fts/test_match_batches.py --port 2891
python3 unittest/storage/fts/test_map_concurrency.py --port 2891
python3 unittest/storage/fts/test_stopword_concurrency.py --port 2891
python3 unittest/storage/fts/test_dictionary_prefixes.py --port 2891
~~~

The hot-path suite checks complete token text, frequencies, document lengths,
document IDs, relevance scores and result order. It covers Unicode boundaries,
six input-encoding conversions and their original error behavior, invalid
UTF-8, smart/max-word IK, BENG, the whitespace parser,
concurrent first-use initialization, three index builds, mutations, rollback,
zero/nonzero and prepared score thresholds, boolean queries, nested statements,
functional lookups, and the long-token-list TaaT fallback. Score comparisons
allow relative/absolute error of 1e-12.

The configuration suite checks 768 complete outputs and error codes across
five built-in parsers, valid/invalid properties, output modes, dynamic row
parameters and prepared executions. It retains the latest spelling of
collation-equivalent words and verifies repeated-word frequencies. Its fixture
was recorded on the same unmodified executable and rechecked after restarting
that original binary. JSON token arrays are compared exactly in this suite.

The expanded batch suite checks 480 complete results and independently derives the complete expected document-ID sets
from 2,051 input documents per table. It checks simple/composite primary keys,
overlapping posting lists, long token lists, OFFSET crossing batch boundaries,
updates, deletes, transactions and rollback.

The concurrency suite checks 1,440 complete ID/ranking/token results across
eight independent sessions. Expected IDs come from 127 input documents;
complete ranking results are compared with sequential executions. Different
query terms exercise the optimizer and both scored/score-free retrieval paths.

The stopword suite derives all token frequencies and document lengths from
literal ASCII inputs independently. It checks 510 complete BENG/whitespace
outputs across general-ci, Unicode-ci and binary input collations and eight
concurrent sessions, including every built-in stopword, mixed case, misses,
short tokens, repeated words, empty text and stopword-only text.

The IK-prefix suite checks 162 exact smart/max-word outputs against an original
fixture, then repeats them in four concurrent sessions (810 complete checks).
It covers overlapping prefixes, disjoint initial-character ranges, unknown
characters, punctuation/reset boundaries and long repeated input. The fixture
was recorded and rechecked on the independently verified original binary.

The collation suite covers complete pairwise comparison matrices, full sorting,
top-N sorting, grouping, distinct values and index access under
utf8mb4_general_ci, utf8mb4_unicode_ci and utf8mb4_bin. Its SHA-256 fixtures
cover every result cell and ordering, not just hit counts.

All four fixtures were independently recorded on the unmodified official
competition commit 5bce9e06d1281347ad61344e4b01fab9158c58a2, with executable SHA-256
bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762.
The committed fixtures have identical complete JSON contents to the new
original recordings; unchanged results retain their compact file formatting.
The historical master fixture comparison and committed file hashes are retained in
[competition-fixture-comparison.json](../../../docs/developer-guide/assets/fts-performance/competition-fixture-comparison.json).
Use --record PATH only when deliberately recording an independently verified
baseline. A candidate must be checked against the baseline fixtures.

The Windows lifecycle test starts a separate temporary instance through the
public C driver. It performs two normal open/close cycles, checks tokenization
and a persistent full-text index on both, and removes its own temporary data
on success.

~~~powershell
$env:SEEKDB_TEST_DRIVER = 'C:\Program Files\seekdb\seekdb_driver.dll'
$env:SEEKDB_TEST_BINARY = 'D:\Projects\OceanBase\seekdb\build\fts-runtime-candidate\seekdb.exe'
python unittest/storage/fts/test_dictionary_lifecycle.py -v
~~~

These tests are invoked explicitly; they are not registered as hermetic Bazel
tests because they require a running database or the installed Windows driver.

## Native local benchmark client (Windows)

The optional native_batch_client.cpp is an auxiliary batch SQL client for the
unchanged benchmark's MYSQL command override. It connects only to the
passwordless dedicated endpoint 127.0.0.1:2891. It supports the protocol and
flags used by this benchmark; use an ordinary MySQL client for general use.
Every statement is sent to the database and every result is consumed.

From a Visual Studio developer shell in the repository root:

~~~powershell
New-Item -ItemType Directory -Force build/fts-native-client | Out-Null
cl.exe /nologo /std:c++17 /O2 /MD /EHsc /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS unittest/storage/fts/native_batch_client.cpp /Fobuild/fts-native-client/client.obj /Febuild/fts-native-client/mysql-native.exe /link ws2_32.lib
~~~

Set MYSQL and MYSQL_VERBOSE to that executable plus the usual -N -s -A
--default-character-set=utf8mb4 flags. Use exactly the same client binary for
the original and candidate. The native comparison is retained separately from
the PyMySQL-client comparison; it does not establish a formal CI score.

## Competition BMP dictionary lookup coverage

```bash
python3 unittest/storage/fts/test_dictionary_bmp.py --port 2891
```

The 2,488 SHA-256 digests cover every complete ordered JSON result in both IK
modes. Inputs include every BMP scalar outside the surrogate range, fixed-seed
mixes, repeated characters, character-context combinations and supplementary
planes. Expected outputs were independently recorded on the official
competition original, not on a candidate. The optimized dictionary tables are
derived from original dictionary entries; they contain no workload literals.
The expanded MATCH batch suite includes one/two/three-term unions, exhausted
streams, overlapping and contiguous runs, plus longer-query fallbacks.
See the round-3 evidence in the performance report for final binaries and logs.


`test_ranked_batches.py` checks 432 complete ordered result sets (142,692
result rows) against an independent recording from the official competition
original. It covers dense, sparse and disjoint posting streams, natural and
boolean queries, ranked LIMIT/offset, ordinary and composite primary keys,
updates, deletes and transaction rollback. IDs are exact and score comparisons
use a relative/absolute tolerance of 1e-12. The compressed fixture contains all
rows and scores, not just hit counts.

```sh
python3 unittest/storage/fts/test_ranked_batches.py --port 2891
```

Only on the verified, unmodified `upstream/2026-competition` binary, generate a
new fixture at a new path with `--record /path/to/ranked_batches_expected.json.gz`.
The test refuses to overwrite an existing fixture. Use a dedicated instance;
only the `fts_ranked_batches` test database is created and dropped.

`test_ranked_key_types.py` adds 192 complete ordered result sets (68,852 rows)
from the official original, covering UInt64/Int64 boundaries and binary/composite
keys with NUL and trailing spaces. Only scores use 1e-12 tolerance; all key bytes
and row order are exact. Run on the dedicated instance:

```sh
python3 unittest/storage/fts/test_ranked_key_types.py --port 2891
```

Its default fixture is `ranked_key_types_expected.json.gz`. `--record NEW_PATH`
refuses to overwrite and is only for an independently verified official original.
See the round-7 evidence manifest for both fixture hashes and original binaries.


## UTF8 validation/decode and Unicode case regression

```sh
python3 unittest/storage/fts/test_utf8_prepare_leaf.py --run --compiler clang++
python3 unittest/storage/fts/test_utf8_prepare_leaf.py --run --compiler g++ --sanitize
python3 unittest/storage/fts/test_casecopy.py --port 2891
```

The standard-library-only native entry point extracts real validator, decoder,
classification and context method bodies verbatim. It reads current candidate
sources and immutable parent Git blobs, verifies pinned raw/LF hashes, and fails
on other source drift. Generation alone is the default; `--run` compiles/runs.
Use `--compiler cl.exe` in a Visual Studio developer shell. `--source-root` and
`--output-dir` override the checkout and build/fts-utf8-prepare-leaf paths.
The manifest and driver.inc must remain beside the script. Review source changes
before deliberately updating pins. The 2,429,473 native cases include every
Unicode scalar, surrogate fallback, one/two-byte combinations, multibyte edges,
truncations, deterministic random strings and cached init/error state. Framework
and charset descriptor initialization are stubbed; non-UTF8 cases check dispatch
only, so complete SQL regressions remain required. Native checks must run outside
performance measurement intervals.

The independently recorded official-original casecopy_expected.json.gz checks
16,359 SQL groups: ordered TOKENIZE JSON, frequencies/document lengths, both IK
modes, BENG, all default-table case mappings, width boundaries, NUL, malformed
UTF8 and strict/non-strict sessions. It preserves 924 original SQL errors; the
trimmed original's unsupported non-UTF8 charset errors are not evidence of those
charsets' conversion contents. Recording is explicit, original-only and refuses
to overwrite a fixture. The lowercase pre-copy experiment was rejected; the
regression is retained for Unicode parser changes. See the round-9/10 evidence
manifests for independent fixture and executable provenance.

## Value-free query materialization regression (round 11)

The independently recorded fixture is `query_materialization_expected.json.gz`.
It contains 4,416 complete-result cases from the official competition original
`5bce9e06d1281347ad61344e4b01fab9158c58a2`, binary SHA256
`bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`.
Fixture SHA256 is
`8bb2ccc372e011a1fc290221d601857ca5093d4cdd5fc1449eae20b73b8a2c93`.
Never record expected results on a candidate or overwrite this fixture.

On an already prepared dedicated instance, run the comparison separately from
performance measurements:

```sh
python3 unittest/storage/fts/test_query_materialization.py --port 2891
```

The script creates and drops its isolated `fts_query_materialization_r11`
database. It compares complete rows, IDs, expressions, counts, warnings and
original SQL errors. Its eight tables cover signed/unsigned, binary and
composite keys with both parser configurations. Requested modes are default,
batch1, batch256 and batch513; a requested setting does not itself prove the
effective optimizer plan. Candidate-1 passed all 4,416 cases and all established
suites. The package contains the final driver as well as the earlier proposal
copy; the proposal driver predates bool-metadata and Decimal-client fixes.

The first two official-original recordings failed in client serialization and
Decimal conversion/cleanup. Their complete logs remain alongside successful
recording-3; packaging does not rerun any recording. LOG_DEBUG diagnostic-1
lacked its compiled literal and cannot prove eligibility. Final LOG_TRACE
diagnostic-2 plus the runtime excerpt observe three value-free COUNT paths with
`materialize_results:false` and two ID/order/LIMIT controls with `true`.
The runtime diagnostic is outside scored measurements and does not prove all
optimizer or distributed paths.

See [round-11 evidence](../../../docs/developer-guide/assets/fts-performance/competition-round11/manifest.json).
The final patch is `round11-trace-source.patch` on parent
`ecbf498d120dabaa01b60b89c49aa18a6b9926a1`; the earlier LOG_DEBUG patch is
historical. Three adjacent pairs show only 0.25310035648% overall reduction,
with tokenization 3.2676% slower. This is a small checkpoint, not stable full
marks or a new formal platform score. All negative metrics are retained.
