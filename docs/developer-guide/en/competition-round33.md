# Round 33 counted full-text ordering — retained (2026-11-05)

The bounded R33 rule is retained. The parent's source/test/fixture promotion
receipt records commit and remote
`f1f93b43176bf62f3a883d082ee86eb9d68f867a`. For a restricted immediate COUNT(*)
over a limited full-text view, preprocessing can omit synthesized MATCH DESC
ordering. Explicit-order and value-consuming shapes keep their existing paths.
The target plan retains relevance calculation and LIMIT, avoids index-back,
and records an exact LIMIT parameter constraint.

The correction accepts known signed nonnegative integer LIMIT/OFFSET with
sum at most UINT32_MAX, checking by subtraction. Unsupported representations
and oversized bounds retain the old validation route. Existing exact-value
parameter constraints are registered before skipping implicit ordering;
registration errors propagate. This bounds a change that initially bypassed
legacy validation and changed errors into successful counts.

Three complete incremental pairs against accepted R32 record target LIMIT
**2.481967 → 0.706700 ms**, a **71.526612%** time reduction. Pair reductions
are +75.056730%, +67.954220%, +71.049435%. Three separate cumulative pairs use
the official-original `5bce9e06d1281347ad61344e4b01fab9158c58a2` binary as control.

| Recorded time reduction | Incremental vs R32 | Cumulative vs official |
| --- | ---: | ---: |
| Build category | +7.037943% | +6.650805% |
| Tokenize category | +18.067508% | +21.132691% |
| Query category | +28.526104% | +57.719835% |
| Equal-category overall | +17.877185% | +28.501110% |

These local scopes are separate. Untouched metric volatility cannot be
causally attributed to this rule. All signed samples remain, including the
third incremental pair's build regression; no outlier deletion, timing
correction or SELECT1 subtraction occurs. Both six-run comparisons retain
ROWS20000/BATCH500/ROUNDS3000/QUERY_ROUNDS200/SAMPLES3/WARMUP30/SKIP_LOAD0,
verified affinity15 and expected hits. There is **no new formal/full-score or
statistical-stability claim**; the cumulative result is local evidence only.

The complete initial 54-record differential remains archived: indices10,11,30,31
returned counts where official original returned error1210 for extreme prepared
LIMIT/OFFSET. Bounded104 records then match official original. Initial bounded
regression-a1 still records its relative-reference FileNotFoundError; successful
a2 is separate. Ranked432, keytypes192, the399-case R32 focused test and seven
existing suites have their actual action receipts, without turning the failed
combined action sequence into an all-PASS run.

Test v1 and its Major review are preserved: Python optimization removes assert
gates, so differential or empty-reference failure could exit successfully.
This is source-proven; no executed v1 false-PASS demonstration is claimed.
V2 uses unconditional validation/raises and distinct aliases for literal
sequence phases. Final official and candidate positive runs match all104
ordered result/error records in the official fixture:40 prepared,22 immediate
reuse,28 literal-reuse and14 variants. The old unbounded candidate fails with
21 differences; empty reference fails its required104-record check. Reviews,
v1/v2 sources, fixture, full outputs and negative receipts remain unchanged.

**The `r33-bounded-candidate-final-test-v2-opt-a1` label is not an optimization
mode run:** the parent says PYTHONOPTIMIZE was unset. The parent identifies
`...-opt-a2` as PYTHONOPTIMIZE=1 and the promotion receipt attests optimized
positive/negative/empty-reference gates. Individual experiment receipts do not
record environment or sys.flags.optimize, so that mode attribution is parent
execution evidence, not independently reconstructed from labels. Complete plan
text and stdout hashes are not claimed identical; plan addresses differ.
Cache-hit coverage, allocation-failure injection and universal input equivalence
remain unproven. Static follow-up found no new concrete Major/Blocker after fixes.

[The exact-byte archive](../assets/fts-performance/competition-round33-count-only-order/manifest.json)
includes all R33 snapshots, patch versions, helpers, reviews, timing/raw outputs,
build logs, failure history and commit-helper source; no experiment/commit helper was executed by
the archiver. Production snapshot SHA-256 is
`408fac5f05ca084168b3d1f45df567c33cbadfe42317df5b008d019461b544ac`;
committed v2 test LF SHA-256 is
`800cecebf41834341e2e3102b6041b8f11ac4e70fdef3d9a9c902e16eef40f8d`.
Historical runtime commit/dirty labels remain intact; source/patch pins govern
experiment identity and the later parent receipt records promotion. Binary
pins are inherited, with no fresh binary hashing. No R34/R35/publicresearch,
native binary, runtime/data or secret is included, and no live checkout
cleanliness or later-round validation is asserted.
