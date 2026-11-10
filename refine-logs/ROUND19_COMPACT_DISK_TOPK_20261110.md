# Round 19: compact disk TopK, faster CRC and integer equality keys

Date: 2026-11-10.
Parent submission: `649953e465bb66744ef3215a02f74c3aa45b9641`.

## Changes

- Non-cached unfiltered distance TopK queries can scan serialized row payloads
  directly. The scan validates record bounds and CRC, extracts only the primary
  key and target vector into reusable storage, and avoids constructing strings,
  unrelated values or a complete Row for every candidate.
- The candidate heap retains the original float32 vector. Near-distance
  comparisons continue through the existing exact accumulator, and equal
  distances still use ascending primary-key order.
- Only the selected output window is fully deserialized after TopK. SELECT
  projections, aliases, distance text, JSON escaping and transaction overlay
  semantics continue through the existing output path.
- The compact path is limited to non-cached tables with no WHERE predicates, no
  current transaction overlay and `LIMIT + OFFSET <= 256`. Candidate vector
  storage is further limited to 4 MiB; other queries use the prior general path.
- IEEE CRC32 now uses slicing-by-8 tables while preserving the existing checksum
  byte for byte. Recovery, row reads, catalog reads and writes therefore keep
  the same on-disk format and validation semantics.
- Ordinary BIGINT equality indexes use integer keys instead of formatting every
  value with `std::to_string`; VARCHAR indexes retain exact string keys.

## Correctness verification

All 51 commands in `round19-validation.json` passed:

- Normal, scalar-only and fault-injection builds.
- Official public cases in text and JSONL modes.
- Exact-distance, adversarial ordering, Unicode, JSONL framing, restart,
  shutdown and transaction publication regressions.
- Differential, recovery, result-replay and six allocation-failure suites.
- Additional low-cache exact-distance and hidden-edge runs.
- A 1,000-row, `VECTOR(4096)`, 1 MiB-cache benchmark retained Recall@10 = 1.0
  for both filtered and unfiltered queries.

Final hashes:

```text
source  28e5c8f43715a383e0caabb8f6bf2e34d2bbc0a2aadc31261359aa86a0ce0b42
normal  e23d2e48b627109ced39dcba8c58330dc112238c824ef845bde76394366a10aa
scalar  d9a8e944695270115e2d8d5ce4dda4cebd4a239456ae7c38ddca812dc6a16d44
fault   4ca22024a3999bd5cb36b015fdcdea93b4fa6df16068ad9f2c558a5bc6c9cea3
```

## Development performance

Three fresh run pairs were interleaved in opposite orders. All comparisons use
round18 as the parent and the same Windows machine.

| Workload | Phase | Parent median | Candidate median | Paired median speedup |
| --- | --- | ---: | ---: | ---: |
| 10,000 x 128 cached | durable writes | 0.2312137 s | 0.2291809 s | 1.01x |
| 10,000 x 128 cached | all queries | 0.1609722 s | 0.1604855 s | 1.02x |
| 3,000 x 128, 1 MiB | all queries | 0.5030172 s | 0.3006851 s | 1.67x |
| 3,000 x 128, 1 MiB | filtered queries | 0.1089783 s | 0.0820054 s | 1.31x |
| 3,000 x 128, 1 MiB | unfiltered queries | 0.3933088 s | 0.2187187 s | 1.81x |
| 1,000 x 4,096, 1 MiB | all queries | 0.1708433 s | 0.0727672 s | 2.35x |
| 1,000 x 4,096, 1 MiB | unfiltered queries | 0.1261918 s | 0.0533120 s | 2.37x |

Every measured run retained exact result ordering. Raw paired samples are in
`bench/results/round19-fast-paths.json`.

These are paired Windows development measurements. They are not official Linux
cgroup time, disk-I/O, memory or ranking results. Public local success does not
expose the withheld formal SQL groups.
