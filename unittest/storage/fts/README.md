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
