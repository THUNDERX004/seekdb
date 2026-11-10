# Round 18: disk equality candidates and recovery allocation paths

Date: 2026-11-10.
Parent submission: `35f4c78d57d2de13d1b79ed5c4cc39b23d05ea7c`.

## Changes

- Ordinary-column equality candidates now work when the table is larger than
  the row cache. Non-cached candidates store the primary key, current physical
  offset and an optional cached-row pointer instead of requiring every payload
  in memory.
- The disk candidate list is built in physical file order and read through one
  reused stream. The current transaction overlay is still checked before
  reading a base row, and every candidate still passes the full WHERE predicate
  before distance calculation.
- Fully cached tables keep the prior direct hash traversal, avoiding the
  physical-order sort and extra lookup on the common cached path.
- Row and commit headers are decoded directly from fixed byte buffers. Recovery
  and disk scans reuse payload storage, and a recovered row is moved into the
  row cache instead of copied after deserialization.
- The disk equality index has a bounded metadata budget derived from the row
  cache limit. If the budget is exceeded, the query falls back to the existing
  exact visible-table scan.

The on-disk record format, CRC checks, WAL ordering, transaction publication,
official protocol and Docker entrypoint are unchanged.

## Correctness verification

All 51 commands in `round18-validation.json` passed:

- Normal, scalar-only and fault-injection builds.
- Official public cases in text and JSONL modes.
- Exact-distance, adversarial ordering, Unicode, protocol framing, restart,
  shutdown and transaction publication regressions.
- Differential, recovery, result-replay and allocation-failure suites.
- New low-cache tests for candidate reuse across different equality keys,
  matching inserts and replacements, deletes, rollback, commit invalidation and
  exact filtered TopK.

Final hashes:

```text
source  ace97da27fa9d1ac0cd810ba0cdb06baa2fa29b3c8bb58a38cc3b304eeaf33c3
normal  f57e1941fe20c1c3569442f341e8d95f617213a3246b413e11889a12346e7b70
scalar  3ba802db7e520d1d4891f84d49a347e76ebeffaa079b40c6bb4bab8d7ba008e8
fault   38bd6552830293861d405cce222fb45f1d82da5066c20f4fe62044bbf81fdf56
```

## Development performance

Three fresh run pairs were interleaved in opposite orders. The main workload
used 10,000 rows, 128 dimensions, 1,000 queries, K=10 and batches of 100. The
low-cache workload used the same dimensions and batch shape with 3,000 rows,
200 queries and `MVD_CACHE_LIMIT_MB=1`.

| Workload | Phase | Parent median | Candidate median | Paired median speedup |
| --- | --- | ---: | ---: | ---: |
| cached main | durable writes | 0.2259523 s | 0.2221763 s | 1.02x |
| cached main | all queries | 0.1621730 s | 0.1595842 s | 1.01x |
| 1 MiB cache | durable writes | 0.0750181 s | 0.0737806 s | 1.02x |
| 1 MiB cache | all queries | 0.8298640 s | 0.4991619 s | 1.67x |
| 1 MiB cache | filtered queries | 0.4123116 s | 0.1072463 s | 3.83x |

Both workloads retained Recall@10 = 1.0 in every measured run. Raw paired
samples are in `bench/results/round18-fast-paths.json`.

These are paired Windows development measurements. They are not official
Linux cgroup time, disk-I/O, memory or ranking results, and public local success
does not expose the withheld formal SQL groups.
