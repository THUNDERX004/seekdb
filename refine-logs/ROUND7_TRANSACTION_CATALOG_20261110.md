# Transaction and catalog allocation-failure fixes

## Scope

Parent revision: 3baf8eccceac06de16aa4b3ff03bb423aa0864a4, on the
2026-competition-round2 branch. The upstream round2 branch was checked again
and still points to 9f6df2ba5acf871376e22b64a3d0724e4b575044, an ancestor
of the candidate. This round does not change the evaluator, public mysqltest
fixtures, CMakeLists.txt, Dockerfile or CI configuration relative to its parent.

The platform screenshot reports an unsuccessful MySQL admission check without
a failing case, diagnostic log or locked revision. The defects below were
independently reproduced locally; neither is established as that platform
failure's root cause.

## Retryable COMMIT loses prior transaction work

An independent global-new failure test creates committed rows, starts a
transaction, updates one row and inserts another. On the parent, allocation
failure budget 5 during COMMIT produces a recoverable bad_alloc while clearing
the pending transaction. The exact failure is retained in
round7-commit-allocation-before.txt; the old test executable is kept locally as
build/test_commit_allocation-before-round7.exe.

The catch path now preserves the transaction overlay if no write has started.
The caller can retry COMMIT or issue ROLLBACK. Autocommit still clears its
failed statement through finishMutation(). Exceptions after writing has begun
retain the existing recovery-required behavior: the instance must stop serving
requests and reload its atomic WAL state.

The final standalone test enumerates both one-table and two-table transactions,
with separate retry-COMMIT and retry-ROLLBACK scenarios. For each default and
scalar build:

- One table: 10 retryable and 22 recovery-required allocation failures per
  resolution scenario, followed by a successful COMMIT.
- Two tables: 12 retryable and 37 recovery-required failures per scenario,
  followed by a successful COMMIT.
- Before-write errors preserve visible pending work; retry or rollback and
  durable reload are checked. Recovery-required instances receive no further
  requests. Reload must yield the complete before or complete after state,
  never a cross-table mixture.

## Failed CREATE TABLE appears after reload

The old CREATE TABLE path syncs the catalog before allocating the Table and
table-map entry. Injecting allocation failures after that sync returns an error
and UNKNOWN_TABLE in the live instance, but reopening the same files reveals
the supposedly failed table.

The preserved old executable reproduces 13 such violations at budgets 38-50
in the short temporary path used for round7-catalog-allocation-before.txt.
Allocation ordinals depend on the temporary path and build settings; they are
not an ABI or a production memory limit.

The candidate prepares the table object and map node before appending the
catalog record. Requests are serialized, so no other request can observe this
staged entry. Preparation/append exceptions erase that entry; successful catalog
append is followed only by the OID increment and nonallocating cache rebalance.
The cache-clearing helpers explicitly declare their nonthrowing contract.

The final default and scalar tests each enumerate 51 CREATE TABLE allocation
failures with zero visibility/catalog violations, followed by a successful
durable creation. CREATE VECTOR INDEX covers 39 failures with zero violations;
the existing rows are unchanged, and successful index metadata survives reload.
No createIndex implementation change was necessary.

## Complete local regression

round7-validation.json and round7-validation.txt record all 33 commands passing:

- Default and scalar: public 20 groups / 95 SQL statements, semantic and SQL
  edge cases, disk scanning, randomized differential comparisons, exact vector
  ordering, candidate selection, stdio and fragmented-input contracts.
- Adapter regression, existing process-level commit-publication failure tests,
  and the longer random differential sequence.
- Default/scalar standalone statement, COMMIT and catalog allocation tests.

These are 33 local commands, not 33 formal platform groups. The standalone
allocation tests close and reopen Database objects in the same process;
separate existing suites cover actual subprocess restarts. Global allocation
replacement exists only in the standalone test executables. This is not an
OS-wide memory cap, cgroup OOM test or proof of all possible allocation failures.

The validator checks source, test and executable fingerprints before and after
the complete run. The canonical source blob is
32b08952b2c926a4594fe49b7f668fb0fae5f8a3.

| Item | SHA256 |
| --- | --- |
| Worktree src/minivectordb.cpp | c054601638166978a02c854a6bcc7ecc65f211379691817e9faf335d692d2ba3 |
| Default executable | dafe4ad8a78a03eec6c21dd950e78955722693f95fcde9403f17eaeb802d0031 |
| Scalar executable | 96313df6446915966799ebb8e181f53c1137a10adc1f6c316ec6eaf434cf6cff |
| Fault executable | a20465bfb63ddc8eed2201e89066b3c1311b580327220cc3812c5496b241408e |
| Archived parent executable | 4172ce17f4b646e41973e198532bcec18627106173d362c7d44709f7d7b08d65 |

## Performance checks

The unchanged evaluator compares the archived parent with the candidate,
serially and in alternating order. Every measured run passes the evaluator's
content, exact-query and reload checks. Normal-cache runs use 10000 rows,
128 dimensions, 1000 queries, batch size 100, K=10 and a 192 MiB row-cache
budget. The low-cache runs use 3000 rows, 40 queries and a 1 MiB row-cache
budget, with the other parameters unchanged.

Local request/response medians, in seconds:

| Samples | Parent write | Candidate write | Parent query | Candidate query |
| --- | ---: | ---: | ---: | ---: |
| Initial normal cache, 5 seeds | 0.8393045 | 1.1224463 | 0.1917258 | 0.1880020 |
| Additional normal cache, 7 seeds | 0.2688158 | 0.2827755 | 0.1918499 | 0.1912739 |
| Low row cache, 3 seeds | 0.0852245 | 0.0861724 | 0.1696000 | 0.1693206 |

The first five write samples vary widely, so seven more fixed seeds were
measured without discarding or replacing the initial samples. Median paired
parent/candidate write ratios are 1.05245, 0.98166 and 0.97803, respectively;
ratios below 1 indicate a slower candidate. The later normal-cache write
median is approximately 5.2% slower and the low-cache median approximately
1.1% slower. Query results are approximately unchanged. These results do not
establish a performance improvement or zero regression; correctness of the
exception paths is the purpose of this change.

Raw samples, parameters, binary fingerprints and diagnostic observations are
in bench/results/round7-main.json, round7-main-followup.json and round7-disk.json.
Windows process diagnostics and the row-cache budget are not physical-device
I/O, cgroup peak memory, official scores or ranking measurements.

## Acceptance boundary

The complete official driver and withheld inputs remain unavailable here.
This run does not install or enable Docker/WSL, contact a new execution service,
or submit a formal competition evaluation. No Linux/container/cgroup or formal
all-cases-passed claim is made. The exact pushed candidate still requires a
platform evaluation; local passing results cannot replace that acceptance.
