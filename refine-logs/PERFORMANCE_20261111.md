# Performance follow-up — 2026-11-11

Baseline: platform-admitted b235ce1819cf8b41a8426cd01ca114476441ed80 (visible E 2816.094040).
Source baseline: official round-two f4d4e26653af823a7601668802f80e374ad6643c.
The candidate is the commit containing this report; source SHA256 `25907995b32d5c2fec8ae339faf7d6bbcfc092479f7e53046228a17d0988d1ed`.
No platform performance result exists for this candidate at report creation.

## Final frozen local comparison

Three serial paired JSON runs, alternating executable order, fresh directories,
seeds 20261123/20261124/20261125, 10,000 rows, 128 dimensions, 100-row durable
batches, 1,000 TopK queries, K10, half scalar-equality filtered. Existing benchmark
generation and validation are unchanged. CPU: Ryzen 7 9700X, Windows 11, GCC O3;
runtime normal path supports AVX512. Timers include request/response IPC.

| Phase | Baseline median seconds | Candidate median seconds | Median paired baseline/candidate |
| --- | ---: | ---: | ---: |
| Durable batch writes | 0.2351469 | 0.1971000 | 1.2163x |
| All TopK queries | 0.2653958 | 0.1860577 | 1.3815x |
| Unfiltered queries | 0.1643984 | 0.1139648 | 1.4341x |
| Equality-filtered queries | 0.1009974 | 0.0720929 | 1.3276x |

Paired speedups are medians of within-round ratios, not ratios of phase medians.
All 6,000 baseline/candidate queries were checked against the float32-derived
independent NumPy oracle; ordered IDs and distances matched, and both groups had
Recall@10=1.0 in every run. All-field persistence checks passed after restart.

Windows process peak working-set medians: 15695872 -> 16367616
bytes (+0.641 MiB). This excludes Linux
cgroup/page-cache accounting. Windows I/O counters also include pipes; they are
not physical-device bytes. No official E or ranking is inferred from these data.

## Changes

- Fully cached TopK scans borrow a bounded contiguous directory of PK, Row and
  vector pointers; cached equality buckets reuse this representation. Rows and
  vector payloads are not duplicated. Remaining row-cache space pays for both
  directory capacity and equality-index estimates. Building occurs off to the
  side; insufficient space retains the existing scan. All row replacement,
  deletion, eviction and cache-budget changes invalidate pointers before use.
- A conservative float SIMD screen rejects clearly worse candidates. All retained
  and ambiguous candidates use the prior double kernel and exact integer distance
  comparator. The heap, PK ties, OFFSET and returned distances remain exact.
- Bounded 64 KiB append buffers aggregate row headers and payloads. Each table
  stream is flushed and synced, then closed before the next stream is opened;
  the WAL marker is still synced before publication and success acknowledgement.
  Many-table commits therefore do not accumulate one large buffer per table.

## Screen error envelope

For dimension d <= 16384, finite nonnegative float sums have no accumulation
cancellation. The standard positive-term error bound includes rounded subtraction,
squaring/FMA and at most d+16 accumulation/reduction operations per term. The
chosen relative expansion, 32*(d+16)*float_epsilon, is deliberately much wider
than that bound throughout this gated range. An additional
64*(d+16)*float_min_normal allowance covers subnormal/flush-to-zero errors.
The heap threshold already includes the pre-existing conservative double margin.
Only finite float scores strictly exceeding the expanded threshold are rejected.
Overflow/nonfinite results, near-boundary results, scalar CPUs, and dimensions
above 16384 use the double path. No float score is returned as a distance.

Normal near-unit ULP fixtures, true ties, opposite FLT_MAX values, subnormal tails,
SIMD tails, dimension16385, and an independent integer-lattice oracle exercise
these boundaries. All new AVX2 and AVX512 screen code is also explicitly covered
by local normal/forced-AVX2 tests; forced-scalar coverage keeps the fallback.

## Verification

The frozen source passed all 55 local validation commands, including both normal
and scalar public20-group/95-SQL runs, transaction/recovery and injected failures,
the 27,034-statement long randomized differential sequence, six allocation
harnesses and the new cached TopK suite. Three targeted suites also passed on a
diagnostic copy with only the AVX512 dispatch gate disabled, exercising AVX2.
Source, test and binary fingerprints were stable during full validation.

Linux Docker validation is provided by the unchanged existing GitHub Actions
workflow for this commit; its actual result must be checked separately. Local
Windows results do not establish hidden-case acceptance or Linux cgroup limits.
agent/conversation.json remains byte-for-byte unchanged.

## Experiments retained

The compact JSON evidence includes the earlier initial-screen trial, tree-reduction
plus-buffer trial, pre-closure directory integration and screen ablation. Initial
screening alone gave only ~1.03x paired query speedup. The buffer trial's overall
query median ratio was ~0.96x, so it was not treated as a query improvement.
The integrated screen ablation gave ~1.05x query speedup with the directory fixed.
These are local samples with visible variation; none predicts platform E.

The separate transaction-spill experiment at505bddb8c is parked on its own branch
and is not included. Packed-vector copies and parser rewrites were deferred.

Machine-readable evidence: `bench/results/performance-20261111-json.json`.
Raw local reports: `build/performance-20261111/` (not submitted as benchmark data).
