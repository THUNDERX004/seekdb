# Round 20: materialize only surviving disk TopK rows

Date: 2026-11-11.
Parent submission: `724c0589f1f5e9b1eef1e03b11530d72d9bc99ce`.

## Changes

- Compact disk TopK scans now deserialize a complete Row only when a candidate
  actually enters or replaces an entry in the bounded heap. Other candidates
  remain as validated serialized payloads plus a reusable vector scratch.
- Selected rows are output directly from the heap, eliminating one extra random
  file read per returned row.
- A single ordinary-column equality can use the same compact path through the
  physical-offset candidate index. The stored primary key and indexed predicate
  are rechecked against the payload before distance calculation.
- Multi-condition, range, overlay, cached-table and large-window queries retain
  the previous general path. Near-distance and equal-distance ordering still
  use the original float32 exact comparison.

## Correctness verification

All 51 commands in `round20-validation.json` passed, including normal, scalar,
fault-injection, public SQL, official JSONL, exact-distance, differential,
recovery, transaction publication and allocation-failure suites.

Final hashes:

```text
source  4530ee87173fd8bf1d5ae8a29a9e542fabf919071399bdd04d4b548a58e3519c
normal  d284f87625db3568a4e3fc15945312efb70737be3b8b467e5251602dfafc33dd
scalar  9e993dc189a8281993e190eba627f03d46f3bfc8cd31c84760dce1d8aa47dc34
fault   664bfe70c27cfbb995a78f989b852364971c887958e3b6dc90b963e932e7742c
```

## Development performance

Three run pairs were interleaved in opposite orders against round19.

| Workload | Phase | Parent median | Candidate median | Paired median speedup |
| --- | --- | ---: | ---: | ---: |
| 10,000 x 128 cached | all queries | 0.1563234 s | 0.1520835 s | 1.02x |
| 3,000 x 128, 1 MiB | all queries | 0.3023659 s | 0.2466731 s | 1.23x |
| 3,000 x 128, 1 MiB | filtered queries | 0.0848431 s | 0.0821002 s | 1.03x |
| 3,000 x 128, 1 MiB | unfiltered queries | 0.2179517 s | 0.1647963 s | 1.32x |
| 1,000 x 4,096, 1 MiB | all queries | 0.0712913 s | 0.0678450 s | 1.04x |

Every measured run retained exact ordering and Recall@10 = 1.0. These are paired
Windows development measurements, not official Linux cgroup metrics.
