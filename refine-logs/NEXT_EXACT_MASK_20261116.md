# Exact TopK compound-filter iteration — 2026-11-16

Baseline: `2fb9c2e3cd1a23dc4e688ba05025ae2d84c3df3e`; its `src/` and
`tests/` trees equal remote round2 `b735c2c9a5db741da9b7bdf8042eb97c8f401822`.

## Change

Fully cached exact TopK queries with a non-primary equality and additional
scalar predicates use the existing equality directory. Matching rows are
gathered in groups of four for the existing float screening/double and exact
tie comparison. Primary-key conditions retain the point/range selection path;
uncommitted overlays and disk rows retain their existing paths.

One optional bitmap per table remembers predicate membership, not scores or
query results. Its key owns and compares all condition columns, operators and
typed values. Different vectors, limits and offsets still compute exact
distances afresh. The bitmap is cleared with equality directories before row
mutation, cache rebalance or equality-column changes. Allocations, key copies
and complete mask construction precede publication. Cache metadata is charged
to the existing table budget; the complete key plus bitmap has a 256 KiB cap.
There is no bitmap for fewer than 256 candidates, over 32 conditions or a key
larger than 4 KiB. Budget/allocation failure falls back to row filtering.

Compound-filter scans filter before vector screening and keep the raw-vector
path, rather than preparing codes for rows that may not match. There is no ANN
change and no approximate result acceptance.

## Measurements

Raw evidence: `bench/results/exact-conjunction-mask-20261116.json`.
Linux WSL2, GCC 15.2, `-std=c++20 -O3 -DNDEBUG -pthread`; 10,000 rows,
128 dimensions, K=10, 16 different probes, 512 timed queries per group,
three alternating baseline/candidate pairs. Processes run serially in fresh
data directories. Every response, including all timed queries, is checked
against independent integer squared distances and primary-key ordering.
Binary32-exact multiples of 1/256 avoid using the database's distance kernel
as an oracle. There were 19,044 checked responses across both binaries.

| Query group | Median baseline/candidate time ratio |
| --- | ---: |
| No filter | 1.010 |
| Single equality | 0.972 |
| Repeated compound filter, 1% matching | 1.361 |
| Repeated compound filter, 10% matching | 1.521 |
| Repeated compound filter, 50% matching | 1.270 |
| Compound filter within a selective equality bucket | 1.120 |

The gains apply to repeated filters while changing probes. First use pays
bitmap construction; observed first-query times for the 10% and 50% groups
were roughly 0.54–0.78 ms and 0.62–0.70 ms for the candidate, versus
0.35–0.37 ms and 0.41–0.47 ms for baseline. Both controls fluctuated by run,
including a slower final candidate run; single equality did not demonstrate
an improvement. These are pipe round-trip wall times including client IPC,
not official E, cgroup measurements, a hidden-workload prediction or a rank.

The initial experiment without bitmap reuse improved Windows timings but
only about 0–3% on Linux. That alone was not accepted as a substantive gain.
The existing 65,536-row parallel threshold remains unchanged.

Reproduce (use a separately built baseline from the commit above):

```sh
g++ -std=c++20 -O3 -DNDEBUG -pthread src/minivectordb.cpp -o candidate
python3 -B bench/evaluate_conjunction.py \
  --baseline /path/to/baseline --candidate ./candidate \
  --queries 512 --report /path/to/conjunction.json
```

## Correctness verification

Linux `tests/validate_all.py`: **57/57 commands passed**, including normal
and forced-scalar suites, published JSONL cases, exact ordering, disk and
cache paths, transactions, restart, injected write/publication failures and
three 4,000-step random runs. Source/tests/binaries were unchanged during the
run. Complete local evidence is in
`build/round-next-20261116/final-linux-validation.json` and `.txt`.

Additional Linux checks: large TopK regression (including a compound filter
over a 65,536-row equality bucket) and the extended allocation test passed.
The allocation test injects failures in the mask key, owned string and bit
storage, checks complete recovery, allocation-free reuse, cache-budget
fallback and invalidation. The cached TopK suite checks mask-word tails,
different probes and pagination, changed operators/values/order, extreme
float32 ties, committed updates/deletes/inserts, overlay reads, rollback and
restart. Windows GCC 16.2 static build passed the cached TopK and JSON suites.

No competition-platform evaluation was submitted, and no conversation file
was edited as part of this iteration.
