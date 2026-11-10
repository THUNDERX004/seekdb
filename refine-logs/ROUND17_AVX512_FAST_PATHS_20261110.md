# Round 17: cached-scan, SIMD, schema, and output fast paths

Date: 2026-11-10.
Parent submission: `e45ce3c25b52acaa25df496c9bff32592a56644d`.

## Changes

- Unfiltered queries over a fully cached table use a direct row scan instead of
  repeating overlay and disk-path checks for every candidate.
- Distance dispatch adds AVX-512 double accumulation ahead of the existing
  AVX2/FMA and scalar paths. Detection remains runtime-only; unsupported CPUs
  execute the prior kernels. Near-distance comparison still calls the exact
  integer accumulator before deciding a tie.
- TopK distance output uses fixed six-decimal `std::to_chars` formatting with an
  `ostringstream` fallback, removing one stream allocation per returned row.
- Row payload and 32-byte record-header buffers are reused within a batch.
  On-disk record bytes, CRC, fsync ordering and transaction publication are
  unchanged.
- Parsing reuses a pointer-only schema index that is updated on DDL rather than
  copying all table definitions for every request.
- SQL keyword and column-name matching use zero-allocation ASCII
  case-insensitive comparison. UTF-8 bytes and case-sensitive stored data are
  unchanged.

## Correctness verification

All 51 commands in `round17-validation.json` passed:

- Normal, scalar-only, and fault-injection builds.
- Official public 20 groups / 95 SQL in both executable modes.
- Exact-distance, adversarial ordering, Unicode, JSONL framing, restart and
  shutdown regressions.
- Differential, publication-failure, result-replay and six allocation-failure
  suites.

The scalar build forces the scalar kernel and does not execute AVX-512 code.

## Development performance

Five paired Windows runs used the official workload shape. Three paired runs
used a higher-dimensional workload to exercise the longer SIMD loop.

| Workload | Phase | Parent median | Candidate median | Change |
| --- | --- | ---: | ---: | ---: |
| 10,000 x 128, 1,000 queries | durable writes | 0.472446 s | 0.470789 s | -0.35% |
| 10,000 x 128, 1,000 queries | queries | 0.185609 s | 0.163162 s | -12.09% |
| 5,000 x 4,096, 32 queries | durable writes | 4.883980 s | 4.855066 s | -0.59% |
| 5,000 x 4,096, 32 queries | queries | 0.093702 s | 0.080390 s | -14.21% |

Raw samples are in `bench/results/round17-fast-paths.json`. These are paired
Windows development measurements, not official Linux cgroup time, I/O, memory,
E or ranking results. AVX-512 benefit applies only when the grading CPU and OS
advertise support; fallback correctness is independent of that benefit.

Linux/Docker validation is provided by the branch workflow. Public success does
not expose the nine withheld formal groups.
