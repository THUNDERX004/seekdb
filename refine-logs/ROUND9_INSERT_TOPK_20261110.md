# INSERT arity correction and explicit primary-key TopK ordering

## Version and scope

Parent: 6d6fbb85ff9e5dfade0e499356f3bbbefa9b30d5 on
2026-competition-round2. That exact commit was independently archived,
rebuilt in default/scalar/fault modes with six allocation-test executables,
and passed its full 34-command local validator. The retained archive report
is round8-archive-validation.json/.txt. Its canonical source blob matches
46d121237b29f026fab3c614b600d28670f73e84. The archive is not dependent on
untracked files from the working checkout.

This change does not modify the official mysqltest files, existing evaluator,
Dockerfile, CMake or CI. It changes normal SQL validation and equivalent query
plan selection, not sample-specific answers or error/result expectations.

## Reproduced INSERT defect

The parser checked that every schema column received a value but omitted the
comparison between each row's value count and its explicit column-list size.
An unused trailing duplicate column could therefore be ignored:

~~~sql
CREATE TABLE t(id BIGINT PRIMARY KEY);
BEGIN;
INSERT INTO t VALUES(1);
INSERT INTO t(id,id) VALUES(2);
COMMIT;
SELECT id FROM t ORDER BY id;
~~~

The archived parent acknowledges the malformed INSERT with OK 1, returns IDs
1 and 2 and retains both after a real subprocess restart. The same defect
occurs with multiple columns and a reordered list ending in a duplicate.
round9-insert-before.txt records the actual independent regression failure:
five test methods, eight failed assertions across the two malformed-list
fixtures (rejection, immediate state, post-COMMIT state and post-restart state).
Other validation/control fixtures in that same run pass.

The parser now compares value_index with the schema-column count or explicit
column-list count immediately after each row's closing parenthesis. A mismatch
raises TYPE_MISMATCH before any parsed INSERT can execute. Existing duplicate,
missing-column and too-many-value checks remain in place. All five regression
methods pass on the candidate; raw output is round9-insert-after.txt.

The tests also check a malformed later row in a multirow statement, preservation
of earlier successful work in the explicit transaction, full-count duplicates,
too many values, and valid reordered lists through COMMIT and normal process
restart. Expected rows and errors are independent of the implementation output.

## Equivalent TopK path

The specialized distance heap already orders exact distance ties by ascending
primary key. A query explicitly requesting ORDER BY d,id ASC previously missed
that path and allocated/computed generic selected-row distance metadata for
every candidate. It now uses the same specialized heap when there is exactly
one distance item, finite LIMIT, ascending distance, and either no secondary
key or the actual ascending primary-key column as its only secondary key.

Descending primary keys, other secondary columns, multiple distance items and
unlimited queries keep the general path. Exact-distance comparisons, bounded
heap semantics, filtering, projection, transaction visibility and pagination
are unchanged. Aliases are considered only after existing ORDER BY resolution.

The expanded independent integer-distance suite checks explicit ascending and
descending PK keys, hidden/selected distance expressions, exact ties, subnormal
components, wide exponents, float64 distance collisions and SIMD tails. It
passes on the unmodified parent before optimization and on the candidate,
including the separately built scalar executable. Each mode now checks 680
exact-order SELECTs within 761 SQL statements. The selection suite now checks
616 exact queries / 682 statements across 64 MiB and 1 MiB row-cache budgets,
including transaction overlays, rollback, commit and process restarts.

## Serial local measurements

bench/evaluate.py is unchanged. The supplemental evaluate_tiebreak.py reuses its
client, data generation, independent oracle, timers and full-column persistence
checks, changing only each query's ordering to d,id ASC. It recomputes the SQL
digest from the changed statements. This is a labeled additional workload,
not a replacement for the standard or official benchmark.

Each report alternates parent/candidate execution order and uses a fresh
database per run. All outputs and reloads passed. Standard cache: 10000 rows,
128 dimensions, 1000 queries, batch=100, K=10, five seeds. Low cache: 1 MiB,
3000 rows, 40 queries, three seeds; other parameters unchanged. Seeds are
integers starting at 20261310, not date labels.

Local request/response medians in seconds:

| Workload | Parent write | Candidate write | Parent query | Candidate query | Paired query ratio median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Standard single-key, 5 rounds | 0.2686174 | 0.2668098 | 0.1811067 | 0.1810214 | 0.98987 |
| Supplemental explicit PK, 5 rounds | 0.2671011 | 0.2645784 | 0.3764080 | 0.1794820 | 2.06874 |
| Low-cache single-key, 3 rounds | 0.0877865 | 0.0868740 | 0.1687261 | 0.1686331 | 1.00055 |
| Low-cache explicit PK, 3 rounds | 0.0859396 | 0.0858205 | 0.1706935 | 0.1674295 | 1.02050 |

Ratios are parent/candidate; below one is a slower candidate. The standard
single-key paired median is about 1% slower despite nearly identical medians
of absolute query totals; these are different summary statistics. No zero-
regression or standard-workload speedup claim is made. The explicit-PK
supplemental workload improves in all five rounds (ratios 1.88716 to 2.13715).
That roughly 2.07x paired improvement must not be generalized to the single-key
workload or an unknown official score. Low-cache improvement is modest.

Full samples and fingerprints: bench/results/round9-main.json,
round9-tiebreak.json, round9-disk.json and round9-disk-tiebreak.json. Timers
include pipe request/response overhead and exclude generation, oracle, startup,
DDL, shutdown and reload verification. Windows process counters are not physical
device I/O or official cgroup memory. No official score/rank is calculated.

## Fingerprints and acceptance boundary

round9-validation.json/.txt records all 36 local commands passing, including
public 20-group / 95-SQL checks in both modes, SQL/stream and new INSERT
regressions, exact-distance and selection suites, differential sequences,
COMMIT/catalog failure containment and all six allocation-test executables.
The validator confirms source, test and binary fingerprints are unchanged
through the run. The 36 command count is not a formal platform group count.

Canonical source blob: 74b245fc7212cc881ff2616745afe691450aa8dd.

| Item | SHA256 |
| --- | --- |
| Worktree source | 61e2a256f028a28ae9ba41414558700570216c7b7dc5f28da461661a2439cc9a |
| Default executable | 66d5aee2e7a1e6b185f38d6c8798d2352c1b6fc9747bc8d8c6a3a32613ed7b3a |
| Scalar executable | eed35edffa72f2213f87e000e2c4d6674714ee66c54651dc0c2c21991d3ff135 |
| Fault executable | 8a52b965e1fc156555b1fda5390f315a55633c813193165e4cfe057ea9b2b100 |
| Archived parent default | 60593a746887d1ddf920d6620386f9e1e2e02fb8b91b7c974e8d37ab0acbe6ef |

Official driver framing, frozen resource configuration, withheld inputs and a
formal platform result for the candidate are still missing. The unchanged
local TSV/OK/ERROR interface is not asserted to match the full official driver.
No formal submission, Linux/container execution, Docker/WSL installation or
external execution service was used. Local passing tests cannot establish
formal all-cases acceptance.
