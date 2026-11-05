# Round 32 counted LIMIT projection — retained (2026-11-05)

R32 retains 68 added lines in the existing projection-pruning rule, with a
399-case SQL regression. The parent's receipt records source/test commit and
remote `5cc947bad132c7705e07da461089ff666dc63e60`. The guarded COUNT(*) over a
limited full-text derived table can discard unused projected columns and avoid
their index-back work. MATCH, ordering, LIMIT and scoring remain required.

Three complete local pairs against rebuilt accepted-f56 control record LIMIT
mean **2.627033 → 2.305400 ms**, a **12.243215%** reduction. The fixed parameters
are ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30,
SKIP_LOAD=0; all six receipts verify affinity15 and expected hit counts.

| Recorded time reduction | Aggregate | Pair 1 | Pair 2 | Pair 3 |
| --- | ---: | ---: | ---: | ---: |
| Target LIMIT | +12.243215% | +7.962788% | +25.196208% | −1.418691% |
| Equal-category overall | +5.120836% | +15.095085% | +2.023134% | −7.840775% |

Aggregate category reductions are build +2.221665%, tokenize +6.705538% and
query +6.435304%. These are descriptive local measurements. Untouched metrics
are volatile and are not causally attributed to this rule. All signed runs are
retained without timing corrections, outlier deletion or SELECT1 subtraction.
This is incremental evidence, with no new formal, cumulative, full-score or
statistical-stability claim. The official competition baseline remains
`5bce9e06d1281347ad61344e4b01fab9158c58a2`.

Final v4 focused runs on official-original a3 and candidate regression-a2 pass
**399 CASE records plus PASS, 400 complete stdout lines**, byte-identical at
SHA-256 `0ffb886aaa1192c85d7516449a53e2de42424c52dbcba36240542419335ddeda`.
Child NO_REWRITE/NO_PROJECT_PRUNE plans retain index-back=true and docs.id;
the candidate's unhinted EXTENDED outline contains PROJECT_PRUNE and its replay
returns count20 with index-back=false. Candidate regression-a1 also passes
ranked432, keytypes192 and seven existing suites. Static review found no
concrete production Major/Blocker; these finite tests are not exhaustive proof.

The initial official a1 failure is preserved: the test expected SUM(id)=52,
but received3580 after incorrectly assuming tied ranked LIMIT membership
matched an unbounded result slice. The value-consuming fallback now uses
unique ORDER BY id. V3's 398 result cases passed, but outer-block hints and
plain EXPLAIN did not prove opt-out; its saved hinted plans were pruned.
V4 uses child hints, EXPLAIN EXTENDED, explicit index-back assertions and
generated-outline replay. All four sources, update receipts, failed output
and review history remain immutable. Earlier pending review text describes
its original observation time; final execution is established by later receipts.

The first profile remains insufficient (1,103 raw / 387 effective target RIPs).
The longer capture records **4,267 raw / 1,499 effective / zero unresolved
target RIPs**, with all 32,000 measured query outputs20. Source resolution
covers170 selected addresses/566 samples, including35 DaaT advance_to samples.
ICF template aliases cannot identify real callers; debug locations are not
runtime stacks or CPU percentages. Separate intrusive counters capture the
first thread reaching **4,096 calls**:10,081 comparisons,5,985 skips,24 refills,
maximum skip17;77 calls skip at least8. This bounded snapshot does not establish
all-query coverage or scored timing. Instrumentation restoration is recorded
exactly; diagnostic results are separate from the six fixed-parameter benchmark runs.

[The exact-byte archive](../assets/fts-performance/competition-round32-projection/manifest.json)
preserves complete reports, receipts, small logs, both full profile outputs,
counter/source-line evidence, source/patch identities and test versions.
Candidate source SHA-256 is
`192d7920768202dd761d32ede91b5af55feadacd8a572f8f0d360959e7a09768`;
final test is `9a7589557f23e98cc230c2796f49e80e2d57224fb1158c4a1cda44135a5578db`.
Historical runtime labels, including dirty candidate labels, remain unchanged;
frozen source/patch pins identify the experiment, while the later parent receipt
records promotion. Binary pins are inherited receipts, not new binary hashes.
No live-checkout cleanliness, R33 validation or R32 native fixture is claimed.
