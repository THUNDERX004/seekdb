# Catalog write failure containment

## Baseline and scope

Parent: cd69e47d7b9532a9a65be0e20ed2a45043b246ce, branch
2026-competition-round2. The upstream round2 branch was checked again:
9f6df2ba5acf871376e22b64a3d0724e4b575044 remains its tip and is an ancestor.
This round does not change mysqltest, the evaluator, Dockerfile, CMake or CI.

## Failure reproduced before the guard

The catalog append path previously lets write/sync/publication exceptions reach
the ordinary recoverable request handler. It can continue serving requests with
a torn catalog suffix or with durable metadata missing from the live instance.

To make this path reproducible, two hooks are compiled only with MVD_TEST_FAULTS:

- MVD_TEST_FAIL_CATALOG_APPEND writes and syncs half a catalog record, then
  throws bad_alloc.
- MVD_TEST_FAIL_CATALOG_PUBLICATION writes and syncs a complete catalog record,
  then throws bad_alloc before the caller publishes its in-memory metadata.

These simulate failures after real writes to test-owned temporary files. They
are not an actual failing disk/fsync test, platform log, cgroup OOM, or proof of
the screenshot's unknown root cause. The pre-guard executable consists of the
parent implementation plus these hooks; it is not an unmodified parent binary.
It is retained locally as build/round8-catalog-before.exe.

round8-catalog-before.txt records the complete-record counterexample: CREATE
TABLE returns ERROR INTERNAL_ERROR, SELECT reports UNKNOWN_TABLE, a subsequent
INSERT still succeeds, and the process exits zero. Reopening the same files
finds the supposedly failed table and the later inserted row.
round8-catalog-suite-before.txt records the independent regression rejecting
the old torn-record path: it exits zero and processes later UPDATE/INSERT/COMMIT
instead of stopping before a response to the failed DDL.

## Repair

The catalog record is fully encoded before opening the append writer. Those
pre-write allocation/open errors remain ordinary errors, preserving the prior
statement-isolation behavior. Once append starts, any exception becomes
RecoveryRequired. The CLI already propagates that exception out of the request
loop: there is no success, recoverable error, ready marker or later request
after the interrupted DDL. Restart replays a complete record or truncates a
torn one. The diagnostic now refers to durable writes rather than only COMMIT.
The default build has no new environment checks or fault injection code.

## Independent regression and full validation

tests/test_catalog_publication.py has independent Python oracles for exact
protocol transcripts, full rows, the v1 catalog bytes and CRC32. Its 12 cases
cover CREATE TABLE and CREATE VECTOR INDEX with both failure hooks, ordinary
normal/fault builds without hooks, and the normal build ignoring each hook.
Each case has a seed subprocess, writer subprocess, and two recovery processes:
48 owned child processes in total, all using isolated temporary directories.

Fatal cases require a nonzero exit and restart-required diagnostic, with stdout
ending before the target DDL. The already-queued later requests must not run.
Catalog bytes distinguish torn and complete writes; unchanged WAL and complete
rows after both restarts prove the pre-DDL uncommitted work and later COMMIT
were not published. Index metadata and table visibility are explicitly checked.
The candidate passes all 12 cases; round8-catalog-suite-after.txt retains output.

The rebuilt default/scalar/fault executables pass all 34 commands in
round8-validation.json and round8-validation.txt. This includes public 20-group /
95-SQL runs in both arithmetic modes, SQL/stream boundaries, differential and
exact-order comparisons, previous commit recovery tests, the new catalog test,
and all six standalone allocation executables. Catalog allocation enumeration
still covers 51 CREATE TABLE and 39 CREATE INDEX failures in each mode with
zero violations. Source, tests and executable fingerprints are unchanged during
the complete run. A local command count is not a formal platform group count.

Canonical source blob: 46d121237b29f026fab3c614b600d28670f73e84.

| Item | SHA256 |
| --- | --- |
| Worktree source | 048d49bb9be7ad0be82827c8fc7a2c01182f26647f91f9cdc24515b30fabffdf |
| Default executable | 5436d225b0cb633a9dda25adf0cfb6eaf70f43148a7aa1bc64638d8ac29bca29 |
| Scalar executable | 00de63812e8b8a2f9c25dd8699c8e6f2afa24328bea5ee73d7b8a60417f7ac7a |
| Fault executable | 0dc75fdef3813f025ca9f9aef8d0bfce9d9532144629ed6d45866365b0f3090f |

## Same-binary timing control

Before rebuilding, the unchanged evaluator ran five serial A/A rounds on the
same parent executable in both roles, SHA256
dafe4ad8a78a03eec6c21dd950e78955722693f95fcde9403f17eaeb802d0031.
Normal workload: 10000 rows, 128 dimensions, 1000 queries, batch=100, K=10.
Each run passed full content/query/reload checks. Raw report:
bench/results/round8-write-aa.json.

Write medians are 0.2713466 and 0.2690926 seconds. Individual identical-program
write times reach 2.2080793 and 1.3548084 seconds; paired ratios range from
0.98809 to 1.62981, median 1.00838. This demonstrates intermittent write-timing
outliers even without a code difference. It does not prove every previous
negative A/B difference was noise or erase those retained negative samples.

## Parent/candidate timing

The unchanged evaluator compares the archived parent cd69e47d7 with this
candidate, serially in alternating order. All measured content, exact-query
and reload checks passed. The normal-cache workload uses 10000 rows, 128
dimensions, 1000 queries, batch=100 and K=10 (five seeds). The 1 MiB row-cache
workload uses 3000 rows and 40 queries (three seeds), other parameters unchanged.

Local request/response medians, seconds:

| Workload | Parent write | Candidate write | Parent query | Candidate query |
| --- | ---: | ---: | ---: | ---: |
| Normal row cache, 5 seeds | 0.3036456 | 0.2812103 | 0.1944310 | 0.1964546 |
| 1 MiB row cache, 3 seeds | 0.1715152 | 0.5249172 | 0.1714008 | 0.1716313 |

Median paired parent/candidate write ratios are 1.11229 and 0.54434; query
ratios are 1.06245 and 0.99866, respectively. Ratios below one indicate a
slower candidate. In particular the low-cache write median is about 3.06
times the parent's, a negative result retained in full, not omitted as noise.
The normal-cache parent has a write outlier of 8.5547248 seconds. These
observations do not establish a speedup or zero regression. Raw samples and
binary fingerprints are in bench/results/round8-main.json and round8-disk.json.
Windows process counters do not measure physical-device I/O or official
cgroup peak memory, score or rank.

The large low-cache write difference prompted a separate fixed-seed control
and follow-up (seeds 20261230 through 20261234, five serial rounds each).
The A/A uses the same archived parent executable in both roles, and the
follow-up A/B uses that parent and the unchanged candidate. All checks pass.

| Low-cache follow-up | Baseline write | Candidate write | Baseline query | Candidate query |
| --- | ---: | ---: | ---: | ---: |
| Same archived parent, A/A | 0.0843396 | 0.0858046 | 0.1688534 | 0.1680532 |
| Parent/candidate, A/B | 0.0871509 | 0.0872831 | 0.1684885 | 0.1695623 |

The paired write-ratio medians are 0.98407 (A/A) and 0.99849 (A/B); paired
query-ratio medians are 0.99713 and 1.00008. These additional samples do not
reproduce the earlier approximately threefold write difference, but do not
erase it or prove absence of regression. Both full reports are retained as
bench/results/round8-disk-aa.json and round8-disk-followup.json.

## Input/output contract boundary

mysqltest/README.md specifies one complete SQL or driver directive per
nonempty test-file line, and a JSON assertion array in each .result file.
Those JSON files are expected-result metadata, not a JSON stdout protocol.
The same README explicitly says the complete process driver is not included
and process communication follows the organizer's companion driver. The task
description similarly delegates framing, result format, error codes, readiness,
mode switches and shutdown to that driver, and rejects the earlier JSON
request/response example.

The local TSV/OK/ERROR encoding and optional headers/markers are therefore
implementation/local-adapter contracts, not confirmed formal compatibility.
Do not guess a replacement wire format or use local public-case success as
evidence that the unknown formal driver accepts it. The official upstream
round2 tip was rechecked on 2026-11-10 and remains 9f6df2ba5.

## Formal acceptance still outstanding

The complete official driver, withheld inputs and a formal result for the new
revision remain unavailable. No formal submission, Linux/container execution,
Docker/WSL installation, external execution service or cgroup measurement is
performed. The known unresolved driver framing boundary is not changed by
guessing a new default encoding. Formal all-cases-passed status remains
unproven.
