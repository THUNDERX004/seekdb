# Round 21: reuse startup verification while the data file is unchanged

Date: 2026-11-11.
Parent submission: `9a3be67f8f0d5d186571ba111df90d4c7d47884e`.

## Changes

- Startup recovery already reads every valid row record and verifies its CRC.
  Disk scans now record the verified file size and modification time and reuse
  that verification while the file remains unchanged.
- If the file size or timestamp changes, scans return to per-record CRC
  validation. The disk test now flips a payload byte during a running session,
  updates the timestamp, and verifies that the next query reports
  `CORRUPT_DATA`.
- Successful internal commits refresh the verified snapshot only after durable
  publication. Record bounds, operation types, serialized field bounds and
  transaction visibility checks remain active even when CRC computation is
  skipped.
- Cached scans do not inspect the data file at all. The file snapshot check is
  deferred until the first row that actually requires disk I/O, avoiding fixed
  filesystem overhead on the fully cached benchmark.

## Correctness verification

All 51 commands in `round21-validation.json` passed, including the new external
payload-corruption regression, normal/scalar/fault builds, public SQL, official
JSONL, exact distance, differential, restart, transaction publication and
allocation-failure suites.

Final hashes:

```text
source  84088160be9db39ada63e734330778efd996d7897da580a81e66ceb21ea6fcd9
normal  93b160dc83b613daba802ca4f4ee2ac772f668f4df12179c4cb8ef9e3e5a7e4b
scalar  cc7d56199d72a11b7d22e7ffa7cfa43a347537fc50562d3ad64dab2c6f1229d6
fault   ac187e62f84a47dd3ab6e93982a4ef0e34f98f3b469eb36bcce06277766e9935
```

## Development performance

Three run pairs were interleaved in opposite orders against round20.

| Workload | Phase | Parent median | Candidate median | Paired median speedup |
| --- | --- | ---: | ---: | ---: |
| 10,000 x 128 cached | all queries | 0.1531812 s | 0.1527003 s | neutral |
| 3,000 x 128, 1 MiB | all queries | 0.2558869 s | 0.1996961 s | 1.28x |
| 3,000 x 128, 1 MiB | filtered queries | 0.0839201 s | 0.0807694 s | 1.04x |
| 3,000 x 128, 1 MiB | unfiltered queries | 0.1715112 s | 0.1189267 s | 1.44x |
| 1,000 x 4,096, 1 MiB | all queries | 0.0662545 s | 0.0388928 s | 1.70x |

Every measured run retained exact ordering and Recall@10 = 1.0. These are paired
Windows development measurements, not official Linux cgroup metrics.
