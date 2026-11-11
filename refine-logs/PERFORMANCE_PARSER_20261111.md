# Parsing and cached scan follow-up — 2026-11-11

Comparison baseline: `3593447905bb00771d7d72ae341ba6a961f71639`.
The platform result for that baseline is still unknown. The last platform-admitted
version remains b235ce181, E=2816.094040. No competition submission was made here.

Candidate source SHA256:
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

Linux CI and paired measurement results must be checked for the actual commit.
The existing full correctness/container workflow is unchanged. A separate
read-only-permission workflow runs serial Linux comparisons against pinned359
with the submission's pinned GCC image; it does not contact the competition.

`agent/conversation.json` retains Git blob
`3f1b5f2674f4478f2c977af4fd55424cf104bcb3`. It was not edited.
