# Round 31 advance comparison — rejected (2026-11-05)

**Rejected; no production or established-test promotion.** The first complete
control/candidate pair records equal-category time reduction **−2.796834%**.
The later rebuilt-control comparison is exploratory, **−2.110622%**, and does
not replace the chronological pair. LIMIT is **2.0064 ms** for the candidate
versus **1.9979 ms** for rebuilt control, showing no gain. Diagnostic timings
remain separately labeled and are not pooled into a performance result.
All original samples and signed regressions are retained, without timing
correction. Unrelated BENG movement has no established causal explanation.

The actual linked native fixture passes **1,844 helper cases**: 625 fast-path,
1,219 fallback, and 1,844 independent real-comparator oracle calls. Its complete
9,221-line stdout is preserved, SHA-256
`ec8fbe41bfc2eca0e975754e634340abfac25ec0d499597c8be53cf5d5140b22`, with empty
stderr. **Helper equivalence does not establish either iterator callsite's
coverage, refill or advance control flow.** Candidate ranked/key-type SQL and
seven existing suites pass. The plans, diagnostic benchmark, console benchmark
and console ranked receipts are retained. No marker was observed, but the
instrumentation was not independently validated; absence is not proof of zero
execution. The initial diagnostic compile failed because `LOG_WARN` required
`ret`; its exact log/source and subsequent explicit-return logger and stderr
repairs remain archived, without declaring the reachability question resolved.

The parity audit establishes no concrete compiler/linker configuration drift;
matching recorded flags/DLLs do not prove complete historical build/runtime
parity. Changed static-library contents and missing historical provenance are disclosed.
The final closure records exact accepted-source restoration to checkpoint
`f56ddd03e66826919d8d9192bc4c0afece8843dd`, with an empty tracked source diff.
The accepted-source rebuild passed **before** removal of the unused experimental
header and unpromoted test TU; no later build is claimed. Accepted production
remains that checkpoint. Historical C7 round25 **26.155337%** cumulative
improvement was local evidence only, not a new round31 or formal score.

[Exact-byte round31 archive](../assets/fts-performance/competition-round31-advance/manifest.json)
preserves all eight experiment labels, frozen proposals/reviews, native helper,
full outputs, failed/repaired diagnostics, parity audit and restoration records.
Historical pending metadata is unchanged; later receipts describe execution.
Historical runtime commit/dirty labels remain unchanged; frozen source snapshots,
patches and hashes identify the experimental source.
No full-score, statistical-stability, all-input or allocation-failure claim follows.
[Historical round25 archive](../assets/fts-performance/competition-round25-cumulative/manifest.json).
