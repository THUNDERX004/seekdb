# Parsing and cached scan follow-up — 2026-11-11

Comparison baseline: `3593447905bb00771d7d72ae341ba6a961f71639`.
The platform result for that baseline is still unknown. The last platform-admitted
version remains b235ce181, E=2816.094040. No competition submission was made here.

Final candidate source SHA256 (including bounded spool writeback):
`beab3cd1edd981288906de66c6d85bb457adc5496e6a413575c41d08030cde0e`.
Implementation commit: `cfa446a38fd38750e89dc4574b71b6a6b09f4b4b`.
Earlier parser/scan-only source:
`e974042b58f697ead39ceaf2e6222fb84f73c3a5c49d8fc6edfa05763742bf37`.

## Changes and contract

- JSON unescaped ASCII strings and ordinary SQL quoted-string prefixes are
  copied once instead of growing a string character by character. JSON controls,
  escapes and non-ASCII continue through the original decoder/UTF-8 validator.
  SQL doubled quotes and backslash semantics are preserved.
- Existing cached vector pointer directories are sorted by allocation address
  to improve access locality. No vector payload is duplicated. Exact distance
  and primary-key comparisons still determine output membership and ordering.
  Disk scan directories retain physical file order.
- AVX2/FMA and AVX512 kernels screen four independent cached rows per call,
  sharing probe loads. Each score is checked against the *current* conservative
  rejection boundary when its row is consumed. All survivors, nonfinite scores,
  and ambiguous candidates use the original double/integer comparator. Scalar
  CPUs and dimensions above16384 retain the original path. Four-row scores use
  fixed stack storage; no cache budget or durability behavior is changed.

The prior screen error envelope already bounds as many as dimension+16
accumulation/reduction steps. The batched kernels have no more than that bound;
the conservative multipliers and subnormal allowance are unchanged.

## Local measurements (pre-Linux)

Windows11 / Ryzen7 9700X / WinLibs GCC16.2 O3. Independent JSON benchmark;
fresh directories; alternating baseline/candidate order; all requests included;
exact ordered IDs, distances and every stored field checked after restart.

| Trial | Runs / queries per run | Query speedup median | Write speedup median |
| --- | --- | ---: | ---: |
| Parser only, exploratory | 3 /1000 | 0.8979x | 1.0965x |
| Same machine-code rebuild control | 3 /1000 | 1.0527x | 0.9937x |
| Parser + address ordering | 3 /5000 | 1.0353x | 1.1516x |
| Integrated batched candidate | 3 /5000 | 1.1142x | 1.1418x |
| Batch ablation vs parser+ordering | 3 /5000 | 1.0484x | 0.9666x |
| Frozen candidate vs359, AVX512 | 3 /1000 | 0.9891x | 1.0312x |
| Frozen candidate vs359, forced AVX2 | 3 /1000 | 1.1484x | 1.1026x |

These are medians of within-run baseline/candidate ratios. The short A/A control
ranged from0.8521x to1.1520x, so the frozen AVX512 endpoint result establishes
neither a reliable gain nor a reliable regression. Exploratory and unfavorable
results are retained. The larger integrated run had all three query ratios above
1.07x; its candidate peak working set median was16433152 vs16203776 bytes.
These Windows process counters are not Linux cgroup memory or physical I/O.
No platform E or ranking is inferred. Linux confirmation is still required.

`bench/results/performance-parser-20261111.json` retains seven compact reports,
binary/source fingerprints, validation results and raw-report hashes. Complete
local reports remain under `build/performance-parser-20261111/`.

## Correctness and review

- All57 frozen-source local validation commands passed in `validation-confirm`:
  normal/scalar public suites, protocol, transactions, restart/recovery, fault
  injection, the long differential run, and six allocation harnesses.
- The initial full run passed56/57: one scalar text-protocol test timed out after
  3seconds on its initial CREATE TABLE. The exact failing suite subsequently
  passed with unchanged candidate and baseline; all57 then passed in a complete
  rerun. The cause of that one timeout is unconfirmed. Failure evidence remains
  in `validation.json`; no timeout or assertion was relaxed.
- Four additional suites passed on a diagnostic copy with AVX512 dispatch
  disabled: exact distance, cached TopK, differential, and string parsing.
- Seven new string tests cover owning token boundaries, both quote styles,
  long literals, escapes, Unicode/surrogates, malformed UTF-8/controls/truncation,
  decoded duplicate keys and rejection without partial changes.
- Cached TopK now also tests varied finite scores, four-row batch tails,
  filtered subsets, K/OFFSET and dimensions1/7/8/9/15/16/17/127/128/129/16384
  against an independent exact integer-lattice oracle.
- Independent static review found no reproducible Blocker or Major findings.

## Linux comparison and memory follow-up

The initial Linux paired run42970901738 verified all14,000 exact queries and
restart contents. Its median write speedup was1.1383x; query speedup0.9811x.
The independent run42971392581 repeated seven paired seeds and added ablations:

| Comparison (numerator / denominator) | Query median ratio | Write median ratio |
| --- | ---: | ---: |
|359 / integrated |1.0478x|1.3223x|
|359 / parser-only |1.0204x|1.3383x|
|integrated / parser-only |0.9707x|1.0001x|

The parser supplies the measured write benefit; cached scan changes supplied
about3% query benefit in the direct ablation. This does not erase the earlier
negative query result, and neither run measures the official cgroup E score.
The integrated scan remains the candidate pending final correctness checks.

Linux validation42971024966 passed all57 correctness commands, the unchanged
official public checker, mounted-image/EOF rollback tests, and the256MiB
20000x4096D resource workload. It **failed** the64MiB, cache8MiB,
20000x1024D workload: the third session was OOM-killed before the response to
`SELECT id,tag,v,v FROM t;`. The189449 output bytes contain exactly the ready
message and previous three responses. This is not a JSON newline/flush timeout.

The relevant disk scan, recovery, result buffering and spool paths are identical
to359. The result buffer is bounded to1MiB, but this query produces an82326681
byte result spool; dirty file pages also count toward cgroup memory. Attribution
to page-cache pressure is a hypothesis until baseline/original/fixed telemetry
is available. No failed run was discarded, and limits were not relaxed.

The final candidate bounds dirty result-spool writes to4MiB with
`fflush -> fdatasync -> POSIX_FADV_DONTNEED` on Linux. Small results never enter
this path. Sync errors remain before response publication. Fault injection
covers both a mid-result and final-tail failure, full retry and production hook
isolation. Local spool8checks and large-result5tests passed in normal/scalar;
JSON19tests passed in normal. Independent static review found no Blocker/Major.

Linux memory controls42974002746 built359, ac126, and the fixed candidate with
identical pinned toolchain/runtime. All six workloads passed, in order
359/ac126/fixed/fixed/ac126/359, with three fresh-process sessions per workload.
The earlier OOM did not reproduce in the controls, so its precise cause remains
unconfirmed; passing controls are not described as proof that ac126 was safe.
All mounts were ext4, shmem was zero, and anonymous memory remained about9–10MiB.
At50ms sampling, query-phase dirty page peaks were:

| Variant | Four query sessions' sampled dirty peaks (MiB) |
| --- | --- |
|359 |15.49,12.21,15.70,16.63|
|ac126 |12.54,16.63,16.86,12.40|
|fixed |3.78,3.75,3.80,3.89|

The controls demonstrate lower dirty-page pressure and successful constrained
execution. Total cgroup memory still approaches64MiB because clean file cache
is reclaimable; no total-memory reduction is claimed. Process exit removes the
cgroup files, so final kernel memory.peak/events are unavailable; sampled peaks
and observed OOMKilled/ExitCode are retained separately. Existing limits and
assertions were not weakened. The fix is retained for this measured reduction.

Final Linux performance run42974002567, seven alternating paired seeds against
359, verified all14,000 exact queries and restart contents. Median paired write
speedup is1.3366x (about25.2% less time), query1.0497x (about4.7% less time).
Query ratios range0.9766–1.1119, so gains vary. The repeated integrated/parser
ablation ratio0.9393 supports retaining batch scans. This test does not measure
official E, hard-cgroup peak memory, or physical-disk I/O; no ranking is predicted.

Full Linux validation42974139587 **passed** all57commands, the unchanged official
public protocol checker, actual image startup/mount/EOF rollback, and both the
256MiB/20000x4096D and64MiB/20000x1024D resource workloads. The final source and
binary fingerprints were checked in the job log. No competition submission was
made; formal hidden-case results remain a platform-only check.

## Qdrant source reference

Read upstream at fixed commit
`6ab21cac18ebb6f4ae29102c7f8f5cc11affd5de`; no Qdrant engine/library is linked.

- `lib/common/common/src/universal_io/mmap/mod.rs`, `clear_ram_cache` and its
  dual-mapping test: clean vs dirty file pages and tmpfs are distinct. The test
  syncs before eviction. This supports the spool diagnostic/control design,
  not an assertion that every cache-advice call releases memory.
- `lib/segment/src/vector_storage/chunked_vectors/read_only/read_ops.rs` and
  `query_scorer/metric_query_scorer.rs`: chunked dense storage and batched reads
  improve locality. Our pointer directory and four-row SIMD screening already
  address part of this. Larger batches/prefetch need their own paired evidence;
  duplicating the whole128D matrix would add4.88MiB before metadata.
- `lib/segment/src/index/query_estimator.rs` and
  `struct_payload_index/read_view/optimizer.rs`: choose selective candidates
  and predicate order. Compound-filter planning is a future experiment; exact
  membership and the complete predicate must still be checked.

Broader dense-storage, HNSW and quantization changes are not part of this round.
Exact TopK retains the existing double/exact-integer comparison and PK ties.

Known pre-existing limitation found during the audit: large distance ORDER BY
selection can retain many complete disk rows (for example LIMIT20000 on1024D
vectors). Result spooling happens afterward and does not bound that selection
heap. This is separate from the unordered projection OOM and is unchanged here;
the current improvements do not establish arbitrary-large-K memory safety.

The existing full correctness/container workflow is unchanged. A separate
read-only-permission workflow runs serial Linux comparisons against pinned359
with the submission's pinned GCC image; it does not contact the competition.

`agent/conversation.json` retains Git blob
`3f1b5f2674f4478f2c977af4fd55424cf104bcb3`. It was not edited.
