# JSON response timeout repair — 2026-11-11

The reported failed submission is e45ce3c25b52acaa25df496c9bff32592a56644d. This repair builds on 0f88baeba3fbbe4f6164111c0a854cce0aaa5912, retaining the five later optimization commits and the pre-existing pending source changes. Official round2 branch f4d4e26653af823a7601668802f80e374ad6643c was checked remotely and remains an ancestor. Official mysqltest, protocol_check, and protocol document are unchanged.

## Reproduced defects and repairs

1. Primary-key range predicates previously read/deserialized the entire vector table before filtering. Scalar ORDER BY id LIMIT also scanned all rows. Add a lazily sorted, 8-byte-per-key directory, binary-search bounds, ordered merge with transaction changes, and early LIMIT termination. Invalidate the directory only when key membership changes. Inclusive BIGINT bounds handle both signed endpoints without overflow. Range pushdown also applies to UPDATE, DELETE, and filtered vector selection.
2. CREATE TABLE compared every new name against all prior columns and repeatedly allocated lowercase names. Use a set of normalized names, preserving case-insensitive duplicate errors and statement atomicity.

## Measurements

| Workload | Before | Final repair |
| --- | ---: | ---: |
| 30,000 rows × 768 dimensions, 1 MiB engine cache, 469 checked pages | 27.290802 s | 0.189778 s |
| Duplicate final column, 10,000 preceding columns, 60-byte names | 4.264518 s | 0.008917 s |
| Duplicate final column, 50,000 preceding columns, 60-byte names | not timed in this comparison | 0.052168 s |

Paging compares the pre-repair local source with final repair, not the old submitted version. Parser compares the exact reported failed commit with final repair. Timings include JSONL round trips. These are local Windows measurements with a forced engine cache budget, not Linux/cgroup measurements or official performance scores.

The exact failed commit fails the new deterministic range I/O regression: each query reads approximately 1.79 MB of unrelated row data in a 3,000-row fixture; final repair reads below the 128 KiB bound. This test uses process read-byte counters rather than a machine-dependent speed threshold.

## Validation and reproduction

All 53 final validation commands pass across optimized/scalar builds, fault injection, allocation failure, large results, transaction/restart, protocol and SQL tests. Each default/scalar published checker run passes 20 groups and 95 SQL statements. Raw evidence is timeout-fix-20261111-validation.json; compact timing and binary/source fingerprints are timeout-fix-20261111.json.

    python -X utf8 -B tests/test_primary_ranges.py build/minivectordb.exe -v
    python -X utf8 -B tests/test_json_protocol.py build/minivectordb.exe

The official driver caps both per-request time and the remaining whole-case time. Consequently, repeated scans may ultimately produce a JSON response timeout even when every individual request is below 60 seconds. The platform input and failure stage are unavailable, so the online failure's unique cause is not established and no hidden-case pass is claimed.

No conversation file was edited. Its SHA-256 is 1ced4b41d1a30011ae7d5ad01717f9835611ce9f6faaf7875e1ff6cdb6f5316a. No formal platform submission was made. Linux/container CI is tracked separately after pushing this commit.
