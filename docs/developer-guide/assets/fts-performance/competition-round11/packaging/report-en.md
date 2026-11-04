## Round-11 checkpoint (2026-11-04)

Retain this only as a **small correctness-validated checkpoint**. Three adjacent parent/candidate pairs in `round11-three-pair-comparison.json` show an equal-category time reduction of **0.25310035648%**: build **1.2031%**, query **2.8238%**, and **tokenization 3.2676% slower**. A stable incremental benefit or full marks is not established. There is **no new formal platform score**; the earlier reported 21.099/100 raw, approximately 10.550/50 weighted, remains historical. These are local incremental measurements against the round-10 materializing parent, not a new official-original cumulative comparison. Do not add this percentage to round-10's separate result.

| Metric | Materializing parent mean | Final candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 6.402000 | 6.391333 | 0.1666% |
| `build_ik_content_sec` | 5.079333 | 4.905667 | 3.4191% |
| `build_beng_en_sec` | 2.825333 | 2.824667 | 0.0236% |
| `tokenize_ik_avg_ms` | 0.136833 | 0.143400 | **-4.7990%** |
| `tokenize_beng_avg_ms` | 0.105600 | 0.107433 | **-1.7361%** |
| `query_cn_avg_ms` | 1.042000 | 0.998400 | 4.1843% |
| `query_beng_avg_ms` | 1.325100 | 1.286367 | 2.9230% |
| `query_mixed_avg_ms` | 1.105333 | 1.072200 | 2.9976% |
| `query_limit_avg_ms` | 2.192933 | 2.166833 | 1.1902% |

The first pair and the first two pairs were negative overall and remain available, including every per-metric negative result, in `round11-first-pair-comparison.json` and `round11-incremental-comparison.json`:

| Retained comparison | Build reduction | Tokenize reduction | Query reduction | Equal-category reduction |
| --- | ---: | ---: | ---: | ---: |
| First pair | 0.3544% | -6.3665% | -1.6228% | **-2.54495%** |
| First two pairs | 1.1130% | -6.3251% | 1.9148% | **-1.09909%** |
| All three pairs | 1.2031% | **-3.2676%** | 2.8238% | 0.25310035648% |

All six complete runs are retained. Reductions use ratios of the per-metric run means, then equal means within each category and across the three categories. No SELECT1 subtraction, negative clipping, fastest-sample selection or incomplete-run substitution is used. Fixed parameters remain rows=20000, batch=500, rounds=3000, query_rounds=200, samples=3, warmup=30, skip_load=0; all six reports retain hits 8001/11000/7332/20. Performance experiments use WARN logging; diagnostic runs are separate from the scored samples.

The change skips external ID/relevance datum materialization only for an eligible value-free, top-level natural-language IR root. Code generation clears the existing external output array only after excluding value consumers, residual filters, lookup/index-back, ordering and other metadata dependencies; internal ID expressions and retrieval metadata remain. Runtime eligibility additionally keeps relevance, score-filter, top-k, functional-lookup and other algorithm paths conservative. The runtime-only `materialize_results_` flag defaults to true; no new serialized flag is introduced. DaaT still advances and counts results while omitting eligible datum writes and projection.

The initial LOG_DEBUG build's SQL diagnostic completed, but the diagnostic literal was absent from that executable. It is **not runtime path evidence**. Retain its exact `round11-source.patch`, metadata, build log, diagnostic-1 experiment and output. The final LOG_TRACE build's **diagnostic-2** and `round11-runtime-materialization.log` provide five path observations in diagnostic query order: the three value-free COUNT queries have `materialize_results:false`; ID/order-sensitive and LIMIT/top-k controls have `materialize_results:true`. This establishes those observed cases, not every optimizer or distributed execution path. The diagnostic mixed COUNT is 6666 for its distinct SQL; the benchmark mixed hit count remains 7332.

The official-original fixture is `unittest/storage/fts/query_materialization_expected.json.gz`, independently recorded by `round11-original-query-record-3` on official commit `5bce9e06d1281347ad61344e4b01fab9158c58a2`, original executable SHA256 `bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`. The candidate passes **all 4,416 complete-result cases**, covering rows, IDs, expressions, counts, warnings and original SQL errors. The driver requests four execution modes; this alone does not establish that every requested optimizer mode took effect. The two earlier recording failures are preserved: Python bool metadata serialization, then bytes-to-Decimal conversion followed by a packet-sequence error during cleanup. The third recording succeeds; no candidate output is used as expected data. Fixture SHA256: `8bb2ccc372e011a1fc290221d601857ca5093d4cdd5fc1449eae20b73b8a2c93`.

Candidate-1 also passes all seven established SQL suites, BMP (2,488 complete outputs), case-copy (16,359 groups, including 924 preserved original errors), ranked (432 cases, 142,692 rows), and key types (192 cases, 68,852 rows). These are recorded historical executions; evidence packaging does not rerun them.

Final production identity is parent `ecbf498d120dabaa01b60b89c49aa18a6b9926a1` plus **`round11-trace-source.patch`**, SHA256 **`40ea9aa36f6efd0559d4efad439ece943822e32c19ef2dbd90a6337fb904deaf`**. Candidate executable: `build/fts-round11-trace-value-free/seekdb.exe`, SHA256 **`9c239e363020a77c201c31456c940084f948d5b5741ec73472af55632c9b91da`**, consistent with candidate experiment metadata and the independently read file hash. Report `git_head` identifies a checkout, not the saved executable. The parent task reports that `git diff --binary -- src` matches the production patch byte-for-byte, remote `upstream/2026-competition` still resolves to `5bce9e06d1281347ad61344e4b01fab9158c58a2`, baseline ancestry passes, and protected files are unchanged; these checks are attributed to the parent in the package rather than rerun by packaging.

[Round-11 evidence](../assets/fts-performance/competition-round11/manifest.json) preserves every raw round11 file, both exact source patches, both build logs (including their runtime-DLL notice), full original proposal snapshots and their earlier offline-only manifests, fixture and final driver, failed recordings and diagnostics. The proposal and initial `pending` metadata are historical and remain unchanged; neither is the final production identity. Files are copied byte-for-byte, including original line endings; the compressed production patch is supplemental. The manifest hashes each payload and `SHA256SUMS` also hashes the manifest. No round-12 or later evidence is included. Windows compilation/linking logs do not establish Linux CI success, a stable speedup, full marks or a new platform score.
