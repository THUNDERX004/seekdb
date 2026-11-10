# Bounded memory for large SELECT responses

Parent: 53a231c8296a9e9e21c1c1f3dcfdb6f1e1ceccb7.

## Reproduced gap and change

The row cache was bounded, but ExecResult still retained every formatted result
row in memory. A disk-backed SELECT of 7500 VARCHAR(8192) rows produces
61483893 bytes. With an 8 MiB row cache, the exact parent reaches a Windows
peak working set of 71471104 bytes; the final candidate reaches 18558976 bytes.
Every ID, payload and row count is verified independently. These are process
measurements, not Linux cgroup memory or official performance scores.

SELECT response rows now use a 1 MiB accounting budget. Larger responses are
staged under the database data directory and emitted only after execution and
the staging flush have succeeded. Small responses remain in memory. Row counts
remain independent of the in-memory buffer, preserving LIMIT after spilling.
Columns, raw tab/newline bytes, error responses and requested markers retain
their existing protocol. Sorting buffers are separate; this does not implement
external sorting or bound a single extremely large row/transaction.

Temporary files use exclusive creation and binary I/O. Linux unlinks the open
file immediately; Windows uses delete-on-close. Ownership is RAII and the
response remains noexcept movable. Query files stay on the measured data
filesystem, including their I/O and page-cache costs. Unlike small TopK,
large responses incur additional staging I/O.

Replay errors after output starts propagate to the process boundary: stderr
reports the failure and the process exits nonzero. No ordinary ERROR record,
completion marker or next statement is appended to partial result bytes.

## Validation

All 47 Windows validation commands passed on stable final source/tests/binaries:
normal/scalar public 20-group / 95-SQL checks and regressions, long differential
sequences, transaction/catalog/replay failure checks and six standalone
allocation-failure executables. Raw records: round13-validation.json and .txt.

tests/test_large_results.py verifies five groups on each execution mode:
disk-backed raw bytes and LIMIT/OFFSET; ordinary sorting and distance TopK;
empty results/header/marker combinations; transaction overlays and rollback
across EOF/restart; repeated projections of a long Unicode value across commit
and restart. Values deliberately contain tabs, CR/LF and marker-looking text.

tests/test_result_spool_io.py injects isolated replay failures before the first
block and after 65536 bytes. It checks exact output prefixes, nonzero exit,
stderr, temporary-file cleanup and complete production retries with the test
hook ignored. The hook exists only in MVD_TEST_FAULTS builds. Linux creation,
RLIMIT_FSIZE and live descriptor-leak negative tests were not run locally.

The container resource suite now also selects two full vector projections
without ORDER BY. Its output exceeds the hard memory limit; a streaming oracle
checks every vector and an exact unique-ID multiset without buffering the full
expected output on the host. Linux CI will exercise the existing 256 MiB/default
cache and 64 MiB/8 MiB cache configurations, each with 20000 rows and two
restart cycles. Results for this new commit are pending its CI run.

## Performance evidence

Final candidate SHA256:
18ff30fbc3a877469c979335cce107857ccfe0d5462655a18450ae554f23df2e.
Parent executable SHA256:
da87738c6f484ae80abccbbf9ce4afffdc4e0d4fb69c0d28cedc6a7baed44e72.
Final source SHA256:
37c0ef11ac45e1baddf8a33ef103ec3828947c89b4ce67984e01b852e7980093.

All final benchmark executions are serial, with alternating parent/candidate
order and fresh databases. Every query and all stored fields after restart are
checked. Negative samples are retained. Seconds and paired parent/candidate
ratios (below one is slower):

| Workload | Parent write | Candidate write | Parent query | Candidate query | Paired write ratio | Paired query ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Standard, all 12 final rounds | 0.2597249 | 0.26413965 | 0.18727135 | 0.1872553 | 0.9811701 | 1.0246221 |
| 1 MiB row cache, 3 final rounds | 0.0855985 | 0.0836126 | 0.1700101 | 0.1697887 | 1.0176997 | 0.9938742 |

The first five final rounds were noisy, with a 0.944858 paired write ratio and
a 0.755643 minimum query ratio. Seven additional rounds were combined with all
five original rounds, not substituted for them. The combined paired write ratio
still indicates about a 1.9% slowdown. No official speedup or score is claimed.
The standard workload is 10000 rows / 128 dimensions / 1000 queries / K=10;
the low-cache workload uses 3000 rows and 40 queries. All use batch size 100.
Seeds start at 20261710, with 20261810 for the seven-round follow-up.

Raw final records in bench/results: round13-final-result-memory.json,
round13-final-main.json, round13-final-main-followup.json and
round13-final-disk.json. Earlier round13-result-memory/main/disk.json files
measure the intermediate executable before the replay fail-stop correction;
their fingerprints distinguish them from the final executable.

## Formal acceptance boundary

The official competition and round2 branches were rechecked at
5bce9e06d1281347ad61344e4b01fab9158c58a2 and
9f6df2ba5acf871376e22b64a3d0724e4b575044. Both are ancestors of the submission;
official mysqltest fixtures remain unchanged. Public platform entry,
submission-options and question-introduction APIs were also checked: they
do not publish the missing driver/framing configuration or a failure log.

No formal submission was triggered. The previous failure has no associated
case ID/log/locked SHA available here. Development checks cannot establish
all 29 formal cases or explain that platform failure. The complete companion
driver and/or a new authoritative platform result remains necessary.
