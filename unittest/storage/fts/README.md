# Full-text hot-path regressions

These integration tests require Python 3, PyMySQL, and a dedicated running
seekdb instance. They create and drop only fts_hot_path_regression and
fts_collation_regression. The default SQL endpoint is 127.0.0.1:2891;
override --host, --port, or --user as needed. Read the password from
SEEKDB_TEST_PASSWORD; do not put it in a command line or fixture.

~~~bash
python3 unittest/storage/fts/test_hot_path.py --port 2891 --check-plans
python3 unittest/storage/fts/test_collation.py --port 2891
python3 unittest/storage/fts/test_match_batches.py --port 2891
~~~

The hot-path suite checks complete token text, frequencies, document lengths,
document IDs, relevance scores and result order. It covers Unicode boundaries,
six encodings, invalid UTF-8, smart/max-word IK, BENG, the whitespace parser,
concurrent first-use initialization, three index builds, mutations, rollback,
zero/nonzero and prepared score thresholds, boolean queries, nested statements,
functional lookups, and the long-token-list TaaT fallback. Score comparisons
allow relative/absolute error of 1e-12.

The batch suite independently derives the complete expected document-ID sets
from 2,051 input documents per table. It checks simple/composite primary keys,
overlapping posting lists, long token lists, OFFSET crossing batch boundaries,
updates, deletes, transactions and rollback.

The collation suite covers complete pairwise comparison matrices, full sorting,
top-N sorting, grouping, distinct values and index access under
utf8mb4_general_ci, utf8mb4_unicode_ci and utf8mb4_bin. Its SHA-256 fixtures
cover every result cell and ordering, not just hit counts.

Both fixtures were independently recorded on unmodified source
e3c04befd2c9b6559f0005c4bf72c62e244cf06c, with baseline executable SHA-256
e6ac29a9653941049d3b4982899b7ff7c4abae490a5ec827c3c21cf834e28429.
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
