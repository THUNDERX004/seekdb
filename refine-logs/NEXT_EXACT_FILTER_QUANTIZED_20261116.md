# Dense-filter quantized exact screening — 2026-11-16

Baseline: round2 commit `ab231135474d9024ba529c31c7272520827d45b4`.

When an exact TopK query has residual predicates and a complete cached
predicate bitmap, dense masks now intersect the existing quantized distance
survivors before exact scoring. Quantized scanning is enabled only when at
least 25% of the candidate rows match. Sparse masks keep the prior matching-row
vector path. Exact distance calculation, tie-breaking, and output are unchanged.

## Linux measurements

The independent JSON-protocol evaluator used 10,000 rows, 128 dimensions,
16 distinct probes, 512 timed queries per group, and three alternating paired
runs. Every result was checked against integer squared distances and primary
key order. Ratios are median baseline time divided by candidate time.

| Query group | Speedup |
| --- | ---: |
| Unfiltered | 1.001x |
| Single equality | 0.980x |
| Compound filter, 1% match | 1.003x |
| Compound filter, 10% match | 1.009x |
| Compound filter, 50% match | 1.186x |
| 50% match inside equality bucket | 1.076x |

This is a targeted gain for dense repeated filters; other groups remain near
baseline. The committed result summary is
`bench/results/exact-conjunction-qmask-20261116.json`.

With a 50% filter as the first vector query, Linux first-query latency was
2.471 ms baseline and 3.075 ms candidate because the candidate prepares the
code cache. Across the following 256 checked queries, average latency was
0.248 ms and 0.220 ms respectively, an estimated break-even near 22 queries.

## Validation

- Linux `tests/validate_all.py`: 57/57 commands passed with normal, scalar,
  and fault-injection builds.
- Linux large parallel TopK boundary tests: 2/2 passed.
- Windows cached TopK, quantized screening, and parallel TopK tests: 14/14
  passed.
- No `conversation` file changed. No official platform evaluation was run.
