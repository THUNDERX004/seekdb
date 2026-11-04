# Full-text performance optimization

## Round 25 cumulative comparison with the official source original (2026-11-04)

Two complete local pairs compare the official competition source original `5bce9e06d1281347ad61344e4b01fab9158c58a2` with accepted candidate `f56ddd03e66826919d8d9192bc4c0afece8843dd`. The immutable `cumulative-2pair.json` reports mean-time reductions of **build +19.826591%, tokenize +17.494448%, query +41.144971%, equal-category overall +26.155337%**. This measures the cumulative candidate against a locally executed source original. It does not add earlier incremental results or attribute the total to the span change alone. **It is not a new formal result or proof of full score, stability or statistical significance.**

All four full runs use `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`, WARN logging and requested/verified affinity mask **15**. Each metric is first averaged over two runs per variant; its time reduction is then averaged within build/tokenize/query, with equal weight for the three categories. Values below only format the completed comparison. No raw timing correction, outlier removal, SELECT1 subtraction or clipping of individual improvements is applied. The original one-pair snapshot is also retained verbatim.

| Metric | Original mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 8.339500 | 6.517500 | +21.847833% |
| build_ik_content_sec | 6.727500 | 5.461000 | +18.825715% |
| build_beng_en_sec | 3.501500 | 2.843000 | +18.806226% |
| tokenize_ik_avg_ms | 0.179700 | 0.137650 | +23.400111% |
| tokenize_beng_avg_ms | 0.107000 | 0.094600 | +11.588785% |
| query_cn_avg_ms | 1.776650 | 0.974550 | +45.146765% |
| query_beng_avg_ms | 2.528400 | 1.154350 | +54.344645% |
| query_mixed_avg_ms | 2.100350 | 1.035150 | +50.715357% |
| query_limit_avg_ms | 2.508850 | 2.148250 | +14.373119% |

| Pair | Build | Tokenize | Query | Equal-category overall |
| --- | ---: | ---: | ---: | ---: |
| 1 | +15.335599% | +15.914972% | +38.702818% | +23.317796% |
| 2 | +23.995038% | +18.948312% | +43.388402% | +28.777251% |

The primary aggregate uses ratios of per-metric run means, rather than averaging these pair percentages. There are only **two pairs**; all raw sample summaries, variation and runs remain included. This observation does not establish robustness across machines, loads, compiler/platform configurations or longer runs. Local input-validation logs preserve scorer output against fixed CI constants for **both** variants; that output is not treated as a formal or full-score result.

Recorded execution identity is original source `5bce9e06d1281347ad61344e4b01fab9158c58a2` / binary SHA256 `bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`, versus candidate source `f56ddd03e66826919d8d9192bc4c0afece8843dd` / binary SHA256 `c7fcc8676875246a38bc4135f29e6f52b85a9d29a9563679003d5b6140666b01`. The raw reports' `git_head=f56ddd03e`, including the original runs, identifies the harness checkout rather than the executed binary's source. These historical bytes are unchanged. Receipts record binary/source identity; this offline archive neither runs Git nor rehashes mutable current executables as historical proof.

All receipts record successful benchmark and input-validation actions, with CN/BENG/mixed/LIMIT hits **8001/11000/7332/20**. These four runs do not rerun the full SQL/native regression suites. Hit counts and successful scoring validation do not replace complete semantic checks; the accepted changes' separate correctness evidence remains in earlier sections and archives.

The [round25 cumulative manifest](../assets/fts-performance/competition-round25-cumulative/manifest.json) preserves four complete report/receipt/benchmark/input-validation/small-server-log sets, both immutable comparison snapshots, exact `round25-cumulative/summarize.py` and its frozen imported `round19-evidence/summarize-template.py` (SHA256 `0aca605abda2fea4ab7b32335c5fe04302895748e8f63a6ce46887b04b822035`). Per-file gzip preserves original bytes, with raw/packed sizes and SHA256. The archiver verifies inputs without importing or rerunning the summarizers. Official benchmark, scoring and baseline files remain unchanged; prior accepted/rejected archives remain frozen. Only these new report sections and preimage/postimage hashes are added, with existing report content preserved. No builds, runtime, SQL, benchmark, profiling or Git operations are performed during archival.

## Round 19 descriptor-major common-span checkpoint (2026-11-04)

Decision: **retain a bounded incremental checkpoint**, separately from the rejected template V1 below. The five-pair ratio-of-metric-means result is query **+8.820601%**, build **+0.115071%**, tokenize **+0.691649%**, equal-category overall **+3.209107%**. Parent retained it on the combination of four nonnegative query-pair observations and exact semantic-output agreement. **This is not evidence of stable or statistically significant improvement.** Pair 1 slightly regresses, pair 4 is small, and pair 3 has unusually large control BENG/mixed timings. Untouched-path fluctuations are not causal gains from this change.

Only common-byte initialization in `ObStringDiffDecoder::batch_decode` changes. For `row_cap >= 2` the candidate visits descriptors before rows, copying each common span into the same owned row slots; the original row-first helper remains for smaller capacities. Allocation size/count, stride, null/extension handling, variable-row errors, payload decoding, datum publication and scalar decode remain unchanged. It does not copy a first-row template. The exact production source SHA256 is `6140f8f5d70b5108312df925d3f9bba4b2212a94f8c1d32f6c5ebf3c98113499`. Parent reports source plus the established native unit entry committed/pushed as `f56ddd03e66826919d8d9192bc4c0afece8843dd`; this archive performs no Git operation.

All ten full runs use unchanged official parameters `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`, WARN logging and requested/verified affinity mask 15. Recorded CN/BENG/mixed/LIMIT hits remain 8001/11000/7332/20. The immutable `span-comparison-5pair.json` first averages each metric over five runs per variant, then applies within-category arithmetic means and equal category weighting, retaining negative changes. SELECT1 is not subtracted. Tables format existing values without replacing the comparison.

| Metric | Control mean | Span mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 6.591200 | 6.542200 | +0.743415% |
| build_ik_content_sec | 5.291800 | 5.234800 | +1.077138% |
| build_beng_en_sec | 2.846800 | 2.888800 | -1.475341% |
| tokenize_ik_avg_ms | 0.138620 | 0.136180 | +1.760208% |
| tokenize_beng_avg_ms | 0.100820 | 0.101200 | -0.376909% |
| query_cn_avg_ms | 0.963300 | 0.933200 | +3.124676% |
| query_beng_avg_ms | 1.366180 | 1.173020 | +14.138693% |
| query_mixed_avg_ms | 1.200960 | 1.023760 | +14.754863% |
| query_limit_avg_ms | 2.330760 | 2.254680 | +3.264171% |

| Pair | Query time reduction | Equal-category overall |
| --- | ---: | ---: |
| 1 | -0.208687% | -2.555710% |
| 2 | +5.598214% | +4.213473% |
| 3 | +20.993236% | +8.556581% |
| 4 | +0.266746% | +3.000032% |
| 5 | +7.100278% | -1.639954% |

**Pair 3 is influential:** control BENG/mixed are **1.9279/1.8758 ms**, versus candidate **1.1291/1.0723 ms**. All five pairs and negative metrics remain included; no outlier removal, significance test or stability claim is made. The ratio-of-run-means aggregate differs from the descriptive mean of pair improvements. The unchanged build/tokenize results do not establish benefits caused by common-span traversal.

Candidate a1 records PASS for **315 sort metadata/result cases, 4,416 materialization cases, 432 ranked cases / 142,692 rows, 192 key-type cases / 68,852 rows, 709 focused count-union comparisons and seven existing suites**. The five span-control receipts record benchmark/input-validation only; they are not presented as newly rerunning those regression suites. Hints, hit counts and SQL equality do not prove exact internal paths or arbitrary-input equivalence.

The actual production-linked original and span native runs both PASS **230 cases**. Complete stdout, stderr and deterministic records match byte-for-byte. Stdout SHA256 is `f67d52774c96396878ffae09d7df65c7d1c4c670c3f04165900f87e68e5b0221`; normalized record SHA256 is `3502db6cf34b46e0c8ae0cee09673d1767c70a6f9885265bdca1d1c03efae7f5`. There are **13,649 CASE/B/T/PASS record lines, including 12,976 T records**. The unit source `unittest/storage/encoding/test_stringdiff_batch.cpp` is an exact copy of the executed extended source, SHA256 `0cbd3e08a391b4b2f5ddb88fdfc773d5312589250fb5ab4a97f96632852e73c9`. Finite synthetic fixtures check scalar/source bytes, nulls, fixed/variable raw/hex layouts, allocation failure, partial-error publication/retry and ownership; they do not prove encoder selection, malformed-metadata safety, concurrent use, read-bound safety or Linux CI coverage.

Historical benchmark binaries are pinned as control `b271980a025bf9b20086e69f116ec2c24f164d21edd814b4e03d418cadd239fc` and span `c7fcc8676875246a38bc4135f29e6f52b85a9d29a9563679003d5b6140666b01`. Runtime labels such as `856864757de18f383dd9feb44b5a0e492811bd23+round19-span` remain historical metadata: exact source/patch and recorded binary/library hashes identify the dirty candidate. Mutable current binary paths are not historical oracles.

[Accepted-span manifest](../assets/fts-performance/competition-round19-span/manifest.json) preserves ten reports/receipts/log sets, all five immutable comparisons and summaries, exact proposal/application/build records, full span-native output and the committed unit source. Gzip preserves raw bytes and records raw/packed SHA256 and sizes. The original control-230 output/build/helper evidence is hash-referenced to the unchanged [rejected V1 archive](../assets/fts-performance/competition-round19-template/manifest.json); raw V1 evidence is not duplicated. Historical pending proposal text is preserved, with later execution explained here. The archiver invokes no builds/workloads or Git. Official protected files are not edited; parent baseline verification remains separate. Round21 sampling, other proposals, binaries/PDB/LIB/OBJ/DLL, runtime/data/temp and large server logs are excluded. **No new formal result, cumulative score or full-score claim is made.**

## Round 19 string-diff template V1 experiment (2026-11-04)

Decision: **reject V1**. The three-pair local aggregate is **+2.264429%**, but build is **-3.256386%**, tokenize **+6.680873%** and query **+3.368800%**. Query reductions vary across pairs (**+6.710130%, +5.416746%, -3.561439%**); the untouched tokenize path is not a causal gain from this decoder change. These observations do not establish a stable benefit. The rejected proposal and restoration history are archived; this section does not retain or promote the candidate.

The candidate builds a common small-string row template in `ObStringDiffDecoder::batch_decode` and reuses it only under its guarded layout/length/descriptor conditions; other rows retain the existing path. Exact original/candidate source and patch identify this dirty historical experiment. The source snapshots are incremental preimages, not an official-original baseline. The parent's restoration build log records history; later live working-tree contents belong to separate experiments and are not substituted into this archive.

All six full runs use unchanged official parameters `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`, WARN logging and requested/verified affinity mask 15 (logical CPUs 0–3). Recorded hit counts are 8001/11000/7332/20. The immutable `template-comparison-3pair.json` first averages each timing across runs, then applies arithmetic metric means within categories and equal category weighting, preserving negative results. SELECT1 is not subtracted. Values below are formatted from that existing comparison; it was not overwritten or recomputed by the archiver.

| Metric | Control mean | V1 mean | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 6.453 | 6.794 | -5.284364% |
| `build_ik_content_sec` | 5.076667 | 5.245 | -3.315824% |
| `build_beng_en_sec` | 2.823 | 2.856 | -1.168969% |
| `tokenize_ik_avg_ms` | 0.1405667 | 0.1364 | +2.964193% |
| `tokenize_beng_avg_ms` | 0.1199 | 0.1074333 | +10.397554% |
| `query_cn_avg_ms` | 0.9728667 | 1.016167 | -4.450764% |
| `query_beng_avg_ms` | 1.188233 | 1.172633 | +1.312873% |
| `query_mixed_avg_ms` | 1.1042 | 0.9909333 | +10.257804% |
| `query_limit_avg_ms` | 2.4022 | 2.249533 | +6.355285% |

| Pair | Query time reduction | Equal-category overall |
| --- | ---: | ---: |
| 1 | +6.710130% | -2.531425% |
| 2 | +5.416746% | +2.289857% |
| 3 | -3.561439% | +2.938963% |

Both a1 execution receipts/logs record PASS for **4,416 materialization cases, 432 ranked cases / 142,692 result rows, 192 key-type cases / 68,852 rows, 709 focused count-union comparisons and seven established suites**. Hit counts alone are not correctness proof; these are bounded existing regression results, not arbitrary-input equivalence or Linux/platform validation.

Parent-run production-linked native comparisons also PASS. The unchanged original **152 cases / 5,399 tuple lines** share tuple SHA256 `53f000b4106540bcc1acc54e72fe2675c372578b772bd256edfe19941b613bc6` and complete stdout SHA256 `6df75ad24e9b917367175426a0965582ca7a9be817093c2767d62093009e6450`. The separately extended **230 cases / 13,649 tuple lines** share tuple SHA256 `3502db6cf34b46e0c8ae0cee09673d1767c70a6f9885265bdca1d1c03efae7f5` and stdout SHA256 `f67d52774c96396878ffae09d7df65c7d1c4c670c3f04165900f87e68e5b0221`. Within each control/candidate pair, full stdout, stderr and tuple bytes match; the original 152-case stdout prefix is unchanged in the extension. The initial corpus did not cover the variable-layout template guard; the separate extension adds variable-template and partial-failure/retry cases. Native evidence does not override the performance rejection. The immutable parent `extended-template-comparison.json` is preserved separately from the archiver's offline byte-check receipt.

Native control-build1 failed on standalone datum declarations and MemoryContext boolean use; control-build2 failed on an invalid datum include path. Both exact failed sources and compile logs are retained. Later original/extended builds and runs are supported by their actual receipts, command response files and logs. Historical proposal/README “pending” text remains byte-for-byte unchanged; later parent executions are described here rather than rewritten into preparation records. This archival task did not build or execute them.

Historical benchmark binary pins: control `b271980a025bf9b20086e69f116ec2c24f164d21edd814b4e03d418cadd239fc`; candidate `2881dce4a7f2c5eea50057b74bf5cc3a77c0142a4eeb7d1ca45adfa1526ae5cb`. Commit labels such as `856864757de18f383dd9feb44b5a0e492811bd23+round19-template` are historical metadata; source/patch and recorded library/binary hashes govern the dirty candidate's identity, not mutable current binary paths.

[Round19 exact-byte manifest](../assets/fts-performance/competition-round19-template/manifest.json) includes six full reports/metadata and all bounded run logs, source/patch/proposal records, native test/helper versions, failed and successful build records, every allowlisted native stdout/stderr/tuple stream, comparisons, summarizer and full local harness. Raw streams are gzip-packed without newline conversion, with raw and packed SHA256/size pins. The six launcher server logs are only 314 bytes each and are retained; large server logs, runtime/data/temp, binaries/PDB/LIB/OBJ/DLL, span experiments and round20 are excluded. The native selection uses explicit filenames in the six approved build directories, four run directories and seven root files, with no future-file discovery. Official baseline/test/scoring/build/CI files are not edited by this archive. **No new formal result, cumulative score or full-score claim is made.**


## Round 18 bounded count-union checkpoint (2026-11-04)

Decision: **retain this bounded incremental checkpoint**. The existing natural-language, score-free union for at most three dimensions preserves champion selection and deduplication. Only unmaterialized results with no valid pushed LIMIT/OFFSET increment the local count directly; input/output counters receive the produced-row delta before return. Materialized and valid LIMIT/OFFSET paths retain `cache_result`. Saved token-ID access uses `get_data` with the index assertion and refill path retained; borrowed array views remain local. SQL equality does not prove every internal invariant or error path.

Three complete serial pairs use the unchanged official parameters `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`, WARN and matching logical CPU 0–3 affinity. All hits remain 8001/11000/7332/20. SELECT1 is not subtracted. The preserved `count-comparison-3pair.json` uses ratios of per-metric run means, category arithmetic means and equal category weighting; all negatives remain. Positive reduction means faster:

| Metric | Control mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.591333 | 6.973333 | +8.140862% |
| build_ik_content_sec | 5.327667 | 5.291667 | +0.675718% |
| build_beng_en_sec | 3.003 | 2.970667 | +1.076701% |
| tokenize_ik_avg_ms | 0.1467333 | 0.1540667 | -4.997728% |
| tokenize_beng_avg_ms | 0.1116667 | 0.1085333 | +2.805970% |
| query_cn_avg_ms | 1.0139 | 0.984 | +2.949009% |
| query_beng_avg_ms | 1.2888 | 1.182833 | +8.222119% |
| query_mixed_avg_ms | 1.108333 | 0.9920667 | +10.490226% |
| query_limit_avg_ms | 2.369433 | 2.417567 | -2.031428% |

Category reductions: **build +3.297760%, tokenize -1.095879%, query +4.907481%**; equal-category overall **+2.369788%**. Untouched build/tokenize fluctuations are not causal gains from this query change. This is local incremental evidence, not cumulative improvement from the official baseline, statistical significance, a new formal score or proof of 50/50.

| Complete pair | Query reduction | Equal-category overall |
| --- | ---: | ---: |
| 1 | +5.695043% | +1.941551% |
| 2 | +4.498129% | +0.364383% |
| 3 | +4.479829% | +4.773081% |

Candidate a1 passed **4,416 materialization cases, 432 ranked cases / 142,692 rows, 192 key-type cases / 68,852 rows and seven established suites**, according to logs and zero action exit statuses. Both pinned control and candidate passed the new independent SQL test: **131 explicit ASCII BENG documents, 96 query/batch groups, 36 prepared count executions and 709 exact comparisons per binary**. Complete ordered IDs are checked against Python OR membership, alongside unlimited direct/derived `COUNT(*)`, `COUNT(id)` and positive LIMIT/OFFSET controls. Queries cover 1/2/3/4/8/9 terms, overlap, disjoint/missing postings, duplicates and all/no hits. Requested `rowsets_max_rows` values are 1/2/3/7/16/64. Hints and SQL results do not prove effective internal capacities, internal input/output or batch counters, physical branch selection, error injection, native equivalence or concurrency safety. Prepared rebinding is not an instrumented internal rescan; existing ranked fixtures remain unchanged.

Unscored cycle-selected IP diagnosis is separate from benchmark intervals. Preflight CN has only 12 samples (4 target / 8 external). Full CN records **1,814 = 432 target + 1,382 external**, and BENG **2,188 = 621 + 1,567**. Build2 phase samples are only **356/268/148**, with target **139/106/51** and external 217/162/97: weak, small build evidence. These are instantaneous RIP observations after selected thread-cycle deltas, not stacks, CPU utilization, per-symbol CPU shares or a full bottleneck-cost measurement.

Build1 performed sampling but **report serialization failed with `TypeError: Object of type Decimal is not JSON serializable` (exit 1)**. Its truncated invalid JSON and full traceback are retained exactly; partial `status: complete` text is not a successful receipt. The parent converted byte-total metadata to integers only after exact source/copy totals and binary/null-safe row equality checks; build2 then produced a complete report (exit 0). No broad Decimal coercion or benchmark change was added. Current helper bytes are archived; the failed source revision was not independently frozen. Prior no-op restoration and actual seven-task control rebuild logs are referenced in round17 evidence, not duplicated.

[Round 18 exact-byte evidence](../assets/fts-performance/competition-round18/manifest.json) contains all six reports/metadata/logs, all candidate-a1 regressions, both focused SQL outcomes, exact original/candidate snapshots, patch/static report, candidate build log, current auxiliary harness snapshots and all five profile attempts including failed partial output. Raw/gzip SHA256 preserve LF/CRLF and exact bytes. Historical proposal/preparation status text retains its original meaning; later parent execution is recorded separately. Binaries, PDB/DLL/LIB/OBJ, runtime/data, secrets and unfinished stringdiff are excluded.

The live two-file candidate was independently hash-checked against staged snapshots at archival. Control executable SHA256: `ddd423ff4e7b7da6b80ac9cb034dbed8e7c76be24eb41f7b2764c7741a72896a`; candidate: `b271980a025bf9b20086e69f116ec2c24f164d21edd814b4e03d418cadd239fc`. Commit labels alone do not identify dirty-tree builds; exact source/binary pins accompany them. The archival sidecar performs no seekdb build, SQL/native/performance run, production change or Git operation. Only the earlier user-supplied formal result exists; **no new formal score or full marks is established**.

Historical runtime source_commit labels remain acd6333eb344fda9f51c02db20ab233789ade1eb. The parent reports the retained source and focused SQL test committed/pushed and remote-verified at 55d2be86621360fefce3d6fe41f22a96556cdf27; this is separate provenance, not a relabeling or rerun of those binaries.


## Round 17 BMW saturation-bound experiment (2026-11-04)

Decision: **reject both V1 and V2**. V1 replaced the full per-term block-stat prepass with the existing BM25 query-token saturation bound while retaining actual scores and local block bounds, but changed ordered Boolean LIMIT results. V2 restricted the shortcut to natural-language mode, non-function lookups, no dimension weights and unit field boost, retaining nonnegative-count, finite-positive-average and finite-positive-bound guards. V2 passed the existing correctness checks but regressed the target query. A numeric upper bound does not establish traversal, ordering or performance compatibility.

The first V1 ranked-suite failure was `root[49][4][0][0]`. Subsequent complete 432-case candidate/control diagnoses identify **two differing cases, 49 and 265**, for simple_docs/composite_docs at the built stage: Boolean `+database -audit`, `ORDER BY score DESC,id LIMIT 1`. Expected ID **1585** becomes **2170**, both at score **0.5001172075859774**. The control has zero differences across all 432 cases. Equal scores or hit counts do not excuse a changed selected ID; candidate output was never substituted for expected results. Both complete actual-output gzip streams, diagnoses, original failure traceback, comparator and original expected fixture are preserved.

V2 passed the unchanged **432 ranked cases (142,692 result rows), 192 key-type cases (68,852 result rows) and seven existing suites**, as recorded in logs and exit statuses. This is neither arbitrary-input equivalence nor a newly recorded complete V2 output fixture. Performance evidence contains **one complete adjacent candidate/control pair, not three pairs**. The existing comparison.json values are retained below; positive reduction means faster and negative means slower:

| Metric | Control | V2 candidate | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 7.886 | 6.424 | 18.5392% |
| `build_ik_content_sec` | 6.024 | 4.807 | 20.2025% |
| `build_beng_en_sec` | 2.992 | 2.686 | 10.2273% |
| `tokenize_ik_avg_ms` | 0.1365 | 0.1305 | 4.3956% |
| `tokenize_beng_avg_ms` | 0.0957 | 0.0971 | -1.4629% |
| `query_cn_avg_ms` | 1.0507 | 1.0195 | 2.9694% |
| `query_beng_avg_ms` | 1.3671 | 1.5087 | -10.3577% |
| `query_mixed_avg_ms` | 1.2252 | 1.1114 | 9.2883% |
| `query_limit_avg_ms` | 2.2823 | 3.2295 | **-41.5020%** |

Category reductions are **build +16.3230%, tokenize +1.4663%, query -9.9005%**; the existing equal-category overall **+2.6296%** is also retained. Favorable fluctuations in untouched build/tokenize paths cannot be attributed to this BMW change. The target LIMIT query is approximately **41.5% slower**, so the proposal was rejected after this pair. No statistical stability, significance or formal-score claim is made. Increased later traversal from a looser bound is a hypothesis; this round did not measure a prepass-versus-traversal cost breakdown.

Both reports use WARN, logical CPUs 0–3 affinity and the full fixed parameters `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`; hit counts are 8001/11000/7332/20. SELECT1 is not subtracted, negative metrics remain, and separate diagnoses/restoration builds are not performance samples. Executable SHA256 values are V1 `44c16395e1fab585b725cdf93ef6708701bec09346f9880bb7397799bae01856`, V2 `df0547dbdf5d79d971be202f5cfc50480ad66f637ebbdf00e8d15218024f6159`, and control `58a14c4b3cfb44464882de1d87b0f8018c76595a563e039ff3cbd8be2351f07d`. Commit labels are runtime metadata; dirty-tree identities also depend on exact source/patch hashes.

All four production files were restored to the saved V2 original bytes and independently hash-checked again during archival. The initial restore build logged `ninja: no work to do`, which is not recompilation evidence. The parent attributes this to inherited old timestamps, then touched verified originals and performed the actual seven-task rebuild. Both logs are preserved separately. The archival sidecar only reads those historical records; it does not build or measure a post-restoration executable.

Extra SQL regression preparation stopped before a runnable test or new fixture was written. The production-linked native numeric helper contains only **616,096 planned cases** (ceiling one million) and a real plan-only capture: **compile, link and execution all NOT RUN**. No native PASS exists. Even an eventual numeric pass would not validate natural-language/Boolean gating, pivots/ties, local block statistics, initialization/reuse or query integration.

[Round 17 exact-byte evidence](../assets/fts-performance/competition-round17/manifest.json) preserves V1/V2 originals and candidates, all patches, complete diagnoses and fixture identity, the single complete performance pair, restoration logs, reviews and unexecuted helper preparations. Each file records raw/gzip SHA256; already-compressed results receive an outer gzip layer to preserve their original compressed bytes. Historical proposal preparation statuses retain their original meaning; later parent execution is recorded separately. Runtime data, binaries, PDB/OBJ/LIB/DLL, temporary directories and unrelated round18 profiles are excluded. No new formal score exists; **50/50 remains unproven**.


## Round 16 lazy DDL rows (2026-11-04)

Decision: **reject**. Three full serial pairs show build +2.6705%, tokenize -3.9781%, query +1.6349% and equal-category overall only **+0.1091%** time reduction. Pair totals are -1.6662%, -1.3330%, +3.0223%; repeatable benefit is unproven. Both DAS production files were restored and their raw original SHA256 values verified. This checkpoint archives evidence only; no new formal score or 50/50 result exists.

The candidate retained parsing, map iteration order and four-column tuples while emitting through one reusable row. It removed bulk row/pointer-array construction. Existing four-cell rows already fit inline storage; one heap allocation per token was not removed. Longer map retention and the doc-ID snapshot also have costs. A separate complete DEBUG official run passed and exact segment/row TRACE messages prove execution of this path. Its perturbed timings are excluded.

The production-linked native differential passed **180 cases, 2,646 complete tuple comparisons, 2,022 stored rows and 241,483 checks**, including poisoned inputs and lifecycle transitions. Its oracle is the unchanged eager function in the same binary, sharing current parser/map dependencies, with ngram only. It does not establish an independent-original oracle, allocation-failure injection, concurrency, forced spill or Linux coverage. The initial missing-public-type compile failure is preserved; after a test-only include fix, compilation, linking and execution actually passed. The candidate also passed ranked, key-type and seven established SQL suites.

All six scored reports use unchanged complete official parameters, WARN level and matching inherited affinity to logical CPUs 0..3. Keep them separate from older unrestricted series. Every negative is retained, SELECT1 is not subtracted and hits remain 8001/11000/7332/20. [Round 16 evidence](../assets/fts-performance/competition-round16/manifest.json) preserves 98 gzip-protected byte-exact artifacts and hashes: reports, exact patch/original/candidate sources, failures, native receipts, audits and bounded runtime excerpts. The 7.18GB DEBUG runtime stream and database files are excluded; its hash and excerpt provenance remain.

## Round 15 IK chain-directory experiment (2026-11-04)

Decision: **reject** the small-array chain directory. Three full pairs show build +0.6844%, tokenize -0.1953%, query -4.3628%, and equal-category overall **-1.2912%** time reduction. Pair totals are -4.0185%, +0.7303%, -1.0909%. The unscored engine-only IK diagnostic improves about 6.74%; scored IK TOKENIZE means are equal. Retain unchanged BENG fluctuations without assigning a cause.

The pinned candidate passed 1,560 complete TOKENIZE cases, seven established suites, and **972 new cases** spanning IK modes, both output modes, three UTF8 collations, capacity inputs, overlapping words, NUL and the 1000-character segmentation boundary. The new fixture was independently recorded from the official original and compares complete ordered JSON bytes, metadata, warnings and 12 original errors. No direct private-directory allocation-failure test ran; SQL equality does not prove that path.

[Round 15 evidence](../assets/fts-performance/competition-round15/manifest.json) preserves all six full runs, diagnostics, exact patch/source/binary identities, review and original-fixture provenance. Both production files were restored. No new formal score or full marks is claimed.

## Round 14 rejected experiments (2026-11-04)

Both production experiments were restored. Each has three serial, complete pairs against the same-configuration parent executable. Reductions use ratios of the three metric means, followed by equal category weighting; all negative values remain.

| Experiment | Build | Tokenize | Query | Overall time reduction |
| --- | ---: | ---: | ---: | ---: |
| Same-collation stopword conversion shortcut | 1.7601% | -2.7078% | -2.2793% | **-1.0757%** |
| Batched TOKENIZE JSON result nodes | 0.1237% | -0.9232% | 1.8809% | **0.3605%** |

Only one JSON pair has a positive overall result; a repeatable tokenize benefit is not established. Earlier stopword comparisons used a configuration-mismatched control and are excluded. The historical average of paired ratios is retained separately from the ratio-of-means result. SELECT1 is not subtracted and no fastest sample is selected. Official workloads, SQL, timers, scoring and build files are unchanged.

The new `test_tokenize_json_nodes.py` compares all ordered JSON bytes, column metadata, warnings and original SQL errors for 1,560 cases. Its fixture was independently recorded on the official-original executable SHA256 `bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`; fixture SHA256 is `e87f9ca69192116ed37cb376e4a55b18da1b7e47b9f30a306a5d51388c4771ab`. The JSON candidate passed all cases and the seven established FTS suites. Stable ASCII aliases, boolean metadata and VARCHAR input for the original server's unsupported IK LONGTEXT locators were corrected before recording; the full corpus remains. IK untyped NULL retains original error 1235.

The native failure-injection helper is an unexecuted design. Its build attempt failed before compilation because `build-settings.json` was absent; no native lifetime or failure-path pass is claimed. The stopword candidate passed 2,064 SQL cases without a new direct native check. Its exact source patch was not retained, limiting source reproducibility of that rejected experiment.

[Round 14 evidence](../assets/fts-performance/competition-round14/manifest.json) retains all twelve full runs, microbenchmarks, failed attempts, exact JSON source patch/snapshots, binary identities and limitations. The independent fixture lives in the established unittest directory. The only formal result remains the user's earlier 21.099/100 raw score, about **10.55/50** weighted. No new formal result or full marks is claimed.

## Round-13 sort-cache experiment (2026-11-04)

Decision: **reject the sort descriptor cache** and restore both production files. Three full adjacent pairs against the saved parent show only **0.4536%** equal-category time reduction, while the query category is **2.9049% slower** and the first two pairs regress overall. The small aggregate gain comes from build/tokenize movements unrelated to the sort-only change and is not a stable FTS benefit. There is no new formal score.

Before rejection, the candidate built successfully, matched all 315 official-original complete SQL results, metadata and warnings, and passed a direct production-linked initialization test (12 invalid-count cases, 8 valid initializations and 8 resets). Plans included ordinary SORT, TOP-N SORT, MERGE GROUP BY, windows and text retrieval. Forcing `enable_newsort` on caused the original executable connection to drop; those failed recordings are preserved and that mode is excluded, while default, rowset-size and encode-off cases remain.

The [round-13 evidence manifest](../assets/fts-performance/competition-round13/manifest.json) includes both exact source snapshots, the candidate patch and executable identity, original fixture and plans, the native linker receipt, all six complete benchmark reports and failed original attempts. The rejected cache is not present in the production source.

## Round-12 checkpoint (2026-11-04)

Decision: **reject** the derived ASCII stopword shortcut. Three full adjacent parent/candidate comparisons show **-4.0261%** equal-category time reduction: build 0.5244%, tokenize -7.0180%, query -5.5846%. These are local incremental results; there is no new formal score and no demonstrated full marks.

The candidate derives fixed membership keys from the unchanged built-in list and actual GENERAL_CI weights. Only letter-only inputs in the three supported UTF8 collations use the shortcut. Nonletters, non-ASCII and all other charsets retain the original conversion and hash set. The generic set remains initialized. No input, parser rule or benchmark changes are made.

The official-original SQL recording covers 2,064 complete ordered result/error cases (1,458 successes, 576 original invalid-option errors, 30 original unsupported-charset errors). The actual production checker also passes 37,948 direct comparisons, including case variants, byte boundaries, trailing spaces/NUL, conversions and init/destroy/reinit. Candidate-1 passes query-materialization, casecopy, BMP, ranked, key types and all seven established suites. All failed original-recording attempts and the initial fixture are retained.

| Metric | Parent mean | Candidate mean | Reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 6.572667 | 6.707000 | -2.0438% |
| `build_ik_content_sec` | 5.423333 | 5.015333 | 7.5230% |
| `build_beng_en_sec` | 2.824667 | 2.935000 | -3.9061% |
| `tokenize_ik_avg_ms` | 0.145600 | 0.151400 | -3.9835% |
| `tokenize_beng_avg_ms` | 0.101467 | 0.111667 | -10.0526% |
| `query_cn_avg_ms` | 0.967933 | 1.063633 | -9.8870% |
| `query_beng_avg_ms` | 1.217000 | 1.368767 | -12.4706% |
| `query_mixed_avg_ms` | 1.049067 | 1.105167 | -5.3476% |
| `query_limit_avg_ms` | 2.461967 | 2.329833 | 5.3670% |

All runs, negative metrics, exact patches, build logs, production-link receipt, test drivers and fixtures are in the [round-12 manifest](../assets/fts-performance/competition-round12/manifest.json). Every gzip member preserves raw bytes and has both archive and raw SHA256. Performance is serialized and uses the unchanged full workload (20,000 rows, load enabled). The first candidate sample is noisy and retained. The direct check and correctness suites run outside timed samples. No Windows result establishes clean Linux CI.

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

## Round-10 checkpoint (2026-11-04)

Two new, separate adjacent official-original/candidate pairs measure a **26.19%** equal-category reduction: build **22.60%**, tokenize **20.68%**, query **35.29%**. This remains below 50%; the user's only formal platform result is still 21.099/100 raw, or 10.550/50 after weighting. No later platform evaluation is available. These local measurements do not replace that result.

| Metric | Official original mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 8.27500 | 6.43050 | 22.29% |
| `build_ik_content_sec` | 6.77950 | 4.90350 | 27.67% |
| `build_beng_en_sec` | 3.45400 | 2.83800 | 17.83% |
| `tokenize_ik_avg_ms` | 0.18860 | 0.14170 | 24.87% |
| `tokenize_beng_avg_ms` | 0.12490 | 0.10430 | 16.49% |
| `query_cn_avg_ms` | 1.76145 | 1.03195 | 41.41% |
| `query_beng_avg_ms` | 2.43550 | 1.31490 | 46.01% |
| `query_mixed_avg_ms` | 1.98385 | 1.14900 | 42.08% |
| `query_limit_avg_ms` | 2.47540 | 2.18725 | 11.64% |

After successful IK context initialization, the three supported UTF8 collations decode each subsequent character once and classify its codepoint directly. The first character and every nonpositive decode result use the complete existing validation/classification chain. In particular, ILSEQ is zero: surrogate byte sequences preserve the original success/length-3/USELESS result through fallback. Other collations retain their original dispatch. No segmentation rule changes. The unproved posting-batch galloping experiment is withdrawn.

Three additional adjacent pairs isolate this UTF8 change against the same parent without galloping. They show only **1.41%** overall reduction (build 2.49%, tokenize 0.25%, query 1.48%), with **IK TOKENIZE 4.92% slower**. Unchanged BENG also varied, so a stable isolated speedup is not established. All six runs and every negative metric remain in the calculation; the separate cumulative original comparison above uses its own two pairs. No SELECT1 subtraction, negative clipping or fastest-sample selection is used.

The source-extracted native regression passes **2,429,473** cases, including every Unicode scalar, all surrogate encodings, exhaustive one/two-byte inputs, multibyte boundaries/truncations, deterministic random strings, cached state and init/fallback checks. Its native control is pinned to immutable parent source blobs, while SQL expected results remain independently recorded on the official competition original. Framework/allocator/charset-descriptor initialization is stubbed in the native leaf; it does not replace integration testing. All seven established SQL suites, BMP dictionary, complete ranked/key-type fixtures and 16,359 Unicode/case-copy SQL groups pass on the candidate. Of those SQL groups, 15,435 return full results and 924 preserve original errors. The trimmed build rejects latin1/gbk/utf16 with 1115; their conversion contents are not validated. Benchmark hits remain 8001/11000/7332/20.

Rejected round-8 node-allocator specialization measured 0.76% overall reduction but a 5.29% build regression over three pairs. Rejected round-9 lowercase pre-copy elimination measured **-1.12%** overall reduction over two pairs. Both source experiments are reverted; their complete evidence and the initial fixture-recording failure remain retained. The Unicode regression is kept independently of the rejected lowercase optimization.

Full native compilation/linking and source/hash checks passed on Windows. Profiling/native leaf execution occurred outside performance measurements. Linux CI and a new formal score remain unverified. The official baseline/benchmark/build/CI files are unchanged. [Round-10 evidence](../assets/fts-performance/competition-round10/manifest.json), [round-9 rejection](../assets/fts-performance/competition-round9/manifest.json) and [round-8 rejection](../assets/fts-performance/competition-round8/manifest.json) retain all reports, logs, exact patches and binary hashes.

## Round-7 checkpoint (2026-11-04)

Two full runs per official-original/candidate binary give a **26.47%** equal-category reduction (build 12.77%, tokenize 27.30%, query 39.35%). Two adjacent runs per previously pushed a2cd7c9cd/candidate binary give a **7.65%** incremental reduction, with a **2.44% query regression**. The first original LIMIT run was noisy and is retained. This is below 50% and does not establish a new formal platform score.

| Metric | Official original mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 8.44700 | 7.33250 | 13.19% |
| `build_ik_content_sec` | 6.93350 | 5.74650 | 17.12% |
| `build_beng_en_sec` | 3.61350 | 3.32500 | 7.98% |
| `tokenize_ik_avg_ms` | 0.21455 | 0.16385 | 23.63% |
| `tokenize_beng_avg_ms` | 0.15885 | 0.10965 | 30.97% |
| `query_cn_avg_ms` | 1.82590 | 1.11120 | 39.14% |
| `query_beng_avg_ms` | 2.58855 | 1.35485 | 47.66% |
| `query_mixed_avg_ms` | 2.01380 | 1.12005 | 44.38% |
| `query_limit_avg_ms` | 3.23545 | 2.38755 | 26.21% |

IK's four fixed processors now share one arena allocation and retain their exact per-character invocation order. TOKENIZE serializes its parser name into an evaluation-owned 64-byte buffer. UTF8 general_ci comparisons avoid duplicate weight lookups for equal successfully decoded codepoints, preserving invalid-input and trailing-space behavior. This checkpoint also includes adaptive linear/galloping searches over saved posting batches using the original comparator and unchanged cross-batch path; its isolated benefit remains unproven and is subject to follow-up or reversal.

All seven established suites, 2,488 BMP checks and two normal close/reopen cycles pass. Two new independently recorded official-original fixtures compare 432 complete ranked result sets (142,692 rows) and 192 key-type result sets (68,852 rows). Ordered IDs/complete keys are exact; only scores allow 1e-12 relative/absolute tolerance. Coverage includes UInt64 high bits, Int64 extrema, NUL/trailing-space binary/composite keys, sparse/disjoint postings, ranked LIMIT/offset, updates/deletes and rollback. Shared collation checks cover 27 cases and 52,963 rows. Benchmark hits remain 8001/11000/7332/20.

Rejected experiments remain auditable: the round-4 combination regressed 3.15% against the pushed checkpoint; the round-6 interleaved-DAT/galloping combination regressed 5.80% against the IK control, so its optional DAT table was removed. That table requested 8N+63 extra bytes per DAT, where N includes empty slots. Round-5's three paired IK runs and engine-only diagnostics are also retained; variation in unchanged BENG is not attributed to IK.

[Evidence and manifest](../assets/fts-performance/competition-round7/manifest.json) retain all reports, build/regression logs, exact source patches and hashes. Executable metadata identifies the real binary/source; benchmark git_head/git_dirty describe the working checkout. No SELECT1 subtraction, negative clipping, input/timing/scoring/baseline or official build changes are used.

## Round 3 on the official competition baseline (2026-11-04)

The official `2026-competition` original 5bce9e06d1281347ad61344e4b01fab9158c58a2 and the final candidate each ran the unchanged complete benchmark twice. All samples and negative changes are retained.
Category reductions are **build 7.43%, tokenization 17.84%, query 41.62%**, for an equally weighted **22.30%** overall reduction. Local variance remains substantial. This has not achieved the 50% target and does not establish a formal platform score.
The user's prior platform result was 21.099/100 raw performance points, or 10.550/50 after the task weight. This candidate requires another platform evaluation.

| Metric (seconds or milliseconds) | Original mean of two runs | Candidate mean of two runs | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 8.20200 | 8.26250 | -0.74% |
| `build_ik_content_sec` | 6.93550 | 6.11850 | 11.78% |
| `build_beng_en_sec` | 3.98600 | 3.53800 | 11.24% |
| `tokenize_ik_avg_ms` | 0.21360 | 0.17640 | 17.42% |
| `tokenize_beng_avg_ms` | 0.14205 | 0.11610 | 18.27% |
| `query_cn_avg_ms` | 2.09900 | 1.02970 | 50.94% |
| `query_beng_avg_ms` | 2.64155 | 1.34505 | 49.08% |
| `query_mixed_avg_ms` | 2.18695 | 1.07250 | 50.96% |
| `query_limit_avg_ms` | 2.60460 | 2.20055 | 15.51% |

IK samples identified per-character hash and range lookups. Read-only BMP code/range tables are derived from the original dictionaries; supplementary/non-canonical inputs and allocation failures retain the original lookups. Memory costs 256KiB per DAT range plus 128KiB per range dictionary, owned by the shared dictionary lifetime; serialized dictionaries and cache formats are unchanged. The invocation-owned arbitration map avoids bucket locks, and empty TOKENIZE properties directly construct equivalent defaults. Query samples identified per-posting heap work and charset dispatch. Short score-free natural-language unions directly merge IDs, advance duplicates together and copy output before batch refills, honoring the caller's batch capacity. Ranked, boolean and functional lookup paths retain their algorithms. Binary ID comparison preserves trailing spaces, NUL and length ordering. Shared strcmpsp reuses initialized charset entries while retaining the existing cold/invalid-argument path.

The final candidate passed seven complete-result suites, 2,488 complete BMP outputs independently recorded on the official original, 480 expanded batch-boundary checks, and two normal close/reopen cycles. Shared sorting checks cover 27 groups and 52,963 full result rows. Benchmark hits remain 8001/11000/7332/20.
Report git_head describes the checkout; selected executable provenance and SHA-256 come from experiment metadata.
[Complete evidence and limitations](../assets/fts-performance/competition-round3/comparison.json) retain raw reports and diagnostics. Official workload, SQL, timing, scoring, baseline and build files are unchanged. Migration and master results below remain historical records.

> Baseline correction (2026-11-04): the submission now uses the official
> `upstream/2026-competition` baseline
> `5bce9e06d1281347ad61344e4b01fab9158c58a2` on
> `codex/fts-performance-competition`. The former `codex/fts-performance`
> used master. The historical timings, improvements and old regression records below
> describe the former master build; migration validation is recorded separately.
> Official benchmark and build files are inherited without modification. See
> [competition-migration.json](../assets/fts-performance/competition-migration.json).

Migration validation: the official competition original and candidate were
built with the same Windows x64 CMake configuration. Four fixtures were
independently re-recorded on the official original, and both binaries passed
all seven SQL integration suites. Physical standby is disabled locally;
these checks do not establish Linux CI success or new performance results.
The candidate also completed the unchanged official benchmark with 20,000
documents, batch 500, tokenization 3,000, queries 200, 3 samples, 30 warmups
and SKIP_LOAD=0. The official score script accepted its parameters and metrics.
The [candidate report](../assets/fts-performance/competition-candidate-report.txt)
records successful execution; no new original performance control was measured.
Binary hashes, logs and configuration are retained in the migration evidence.

Historical master result: the final v22 local Windows native-client control measures a 9.65%
category-equal time reduction: build 7.42%, tokenization 5.54% and query
15.98%. All three complete runs of each binary are included, with negative
metrics retained. One original IK-all build was unusually slow (10.638 s), so
build improvement is uncertain. The earlier v20 control measured 10.18% overall.
The 50% overall reduction target and formal VLDB CI remain unverified.

## Bottlenecks and changes

- IK classified a character through several predicates that each decoded the
  same bytes. It now decodes once, preserving predicate precedence and UTF-16
  surrogate handling, and passes the already fetched character to processors.
- Every IK parser previously constructed immutable dictionary readers and
  cache-handle containers. The dictionary hub now lazily owns three readers
  and their pins. A cold-path mutex and acquire/release publication protect
  initialization; failed initialization is discarded and can be retried.
  Shutdown releases readers and pins before the backing dictionary/global
  cache is destroyed, after runtime users have stopped.
- BENG previously allocated a tokenizer/normalizer pipeline and copied each
  normalized token again. Embedded pipeline components now normalize directly
  into the caller's arena, keeping tokens alive after parser destruction.
  Whitespace classification combines the same ctype flags; ASCII character
  length bypasses the decoder only for encodings with minimum byte length one.
- Natural-language MATCH used only as a positive WHERE predicate performed
  BM25 estimation, read token frequencies and document lengths, and computed
  scores that the caller never consumed. Conservative expression-use analysis
  now allows score-free driving scans for a SELECT with one WHERE condition,
  no nested statements or column boosts, and no LIMIT/OFFSET. It recognizes
  BOOL(MATCH), MATCH > constrained zero, and zero < MATCH. Projection, ordering,
  boolean mode, nonzero thresholds, additional predicates, and functional
  lookups keep the scored path. Zero thresholds add plan-cache constraints.
- Score-free scans read posting IDs without BM25 aggregates or score-only
  columns. DaaT streams a deduplicated union for ordinary token counts and
  skips per-posting score collection/filter-scope setup. The >256-token TaaT
  fallback still deduplicates IDs, including overlap across posting lists.
  ID-only batch consumers initialize their skip vector; scalar projection and
  token-range rescan paths handle the absence of score expressions.
- Auxiliary build sampling observed collation comparison and sorter symbols
  among active server samples (8.176 s wall, 7.344 s process CPU). This was
  coarse sampling, not a reliable percentage attribution. Identical UTF-8
  general-ci sort keys now return equality without decoding; ASCII decoding
  avoids a function call while retaining the same Unicode weight mapping,
  multibyte validation, and end-space behavior. No sort-key collation changes.

The auxiliary parser microbenchmark used RAND-dependent input inside
BENCHMARK(10000, TOKENIZE(...)) to avoid constant folding. Earlier measurements
showed IK 0.03390 -> 0.03142 ms and BENG 0.00987 -> 0.00932 ms per invocation.
These are diagnostic measurements, not contest scoring metrics.

## Correctness validation

See [test instructions](../../../unittest/storage/fts/README.md).

The original executable was independently verified against source revision
e3c04befd2c9b6559f0005c4bf72c62e244cf06c. Fixtures were recorded only on that
executable and were rechecked on it after formatting the fixture.

The final candidate passed:

- 408 complete token/frequency/document-length cases, 144 ranked MATCH
  results, 920 predicate results and 144 concurrent first-use parses.
- 280 independently derived complete-ID checks on 2,051 documents per table,
  with simple/composite primary keys, retrieval batch boundaries, duplicate
  and >256-token queries, LIMIT/OFFSET, updates, deletes, and rollback.
- 27 shared-collation cases covering 52,963 complete result rows: pairwise
  comparisons, full/top-N sorting, grouping, distinct and index access.
- Two normal open/close cycles through the Windows public C driver, checking
  shared dictionary recreation and a persistent full-text index. One initial
  run returned SQL 1205 while acquiring an index-build snapshot; its log was
  retained. An unchanged isolated rerun passed in 3.900 s.
- Compilation and linking of all affected native unity units, plus a clean
  git diff --check.

The larger regression exposed an OFFSET/top-N runtime-filter projection
difference on an intermediate score-free candidate; the original passed the
same test. LIMIT/OFFSET statements now deliberately retain the established
scored path. The new regression and an explicit plan assertion prevent that
unsafe eligibility from returning.

## Measurement protocol and results

Measurements were collected on 2026-11-04, using a dedicated native Windows
instance on SQL port 2891/RPC port 2892, cpu_count=4, memory_budget=2G and
log_disk_size=2G. The installed service and its data were not used.

The same cached Bazel native compiler options and dependencies were used for
both binaries. Candidate native units were recompiled and linked with the
unchanged Rust/native dependencies. A fresh standard Bazel build was blocked
by cargo-bazel's Windows symlink privilege requirement; no clean Linux VLDB CI
build is claimed.

The current source checkout did not contain the contest benchmark or its
VLDB workflow. The five benchmark files were restored locally, unmodified,
from upstream/2026-competition at 5bce9e06d, and their Git blob hashes were
verified. They are not part of this optimization patch.

The unmodified official shell script, generator, setup SQL, timing and scoring
logic ran with ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3,
WARMUP=30 and SKIP_LOAD=0. Every full run reimported data and built all indexes
into a new report file. No build or profiler ran concurrently with measured
runs. A PyMySQL batch client supplied the local MySQL-compatible command
interface because mysql.exe was unavailable; this is a local comparison,
not an exact reproduction of the CI client/platform.

Raw reports are retained for both original runs (baseline-final,
baseline-paired) and both final candidate runs (candidate-v15 and its repeat).
Intermediate experiments and the initial noisy baseline were not substituted
for these controls. Report Git HEAD/dirty fields describe the checkout at
measurement time; actual executed binaries are identified by
[provenance](../assets/fts-performance/provenance.json).

Build metrics below are seconds; tokenization and query metrics are
milliseconds. Reductions use the arithmetic mean of the two complete runs
for each version. Negative reductions are retained.

| Metric | Original (mean of 2) | Candidate (mean of 2) | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.71450 | 7.64000 | 0.97% |
| build_ik_content_sec | 6.10150 | 6.12300 | -0.35% |
| build_beng_en_sec | 3.56000 | 3.50100 | 1.66% |
| tokenize_ik_avg_ms | 0.22915 | 0.22385 | 2.31% |
| tokenize_beng_avg_ms | 0.18430 | 0.18605 | -0.95% |
| query_cn_avg_ms | 2.19240 | 1.82955 | 16.55% |
| query_beng_avg_ms | 2.74000 | 2.27535 | 16.96% |
| query_mixed_avg_ms | 2.37995 | 1.92400 | 19.16% |
| query_limit_avg_ms | 2.56005 | 2.68960 | -5.06% |

Category reductions are build 0.76%, tokenize 0.68% and query 11.90%.
Their equal-weight mean is 4.45%; using the contest formula against the local
control gives 8.89/100. This comparison quantifies the patch, not a formal
contest result. The four auxiliary hit counts remained 8001, 11000, 7332 and 20;
the complete-result regressions above provide the correctness evidence.

Running the unchanged scorer against the fixed CI baseline gives 100/100
for both final local reports. The unmodified original also reaches the cap
locally. Thus that reference score must not be attributed to this patch:
platform and source-version differences dominate that cross-environment
comparison. Formal performance/scoring still requires the supplied VLDB CI
environment. The nine metrics were not averaged directly.

The raw reports, category comparison and benchmark hashes are under
[the results directory](../assets/fts-performance/).


## Additional native-client control

A subsequent original-binary control (baseline-after) still measured the LIMIT
query at 2.5435 ms with the Python client. A longer direct-connection probe
measured about 1.667 ms for that same query, motivating a separate client
overhead check rather than attributing all differences to the kernel.

The standalone Windows native batch client is retained with the test
instructions for reproducibility. It sends every statement and consumes every
result through the MySQL protocol; it contains no benchmark data, result
shortcuts or timing code. It is restricted to the dedicated passwordless
127.0.0.1:2891 test endpoint. Chinese input, quoting/comments, NULL, TOKENIZE
results and counts were compared with the existing client; result contents
matched (line endings differed). The official benchmark files stayed
byte-identical and all fixed parameters, SQL, repetitions and timer logic
were retained.

Two more complete original runs and two more complete candidate runs used
that same native client. These are a separate comparison; their times are
not averaged with the Python-client reports.

| Metric | Original native mean | Candidate native mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.58800 | 7.45900 | 1.70% |
| build_ik_content_sec | 6.04750 | 6.25200 | -3.38% |
| build_beng_en_sec | 3.61000 | 3.38650 | 6.19% |
| tokenize_ik_avg_ms | 0.13175 | 0.12785 | 2.96% |
| tokenize_beng_avg_ms | 0.08795 | 0.09250 | -5.17% |
| query_cn_avg_ms | 1.46700 | 1.15210 | 21.47% |
| query_beng_avg_ms | 2.06545 | 1.55965 | 24.49% |
| query_mixed_avg_ms | 1.64315 | 1.24600 | 24.17% |
| query_limit_avg_ms | 1.97755 | 1.90485 | 3.68% |

Native-client category reductions are build 1.50%, tokenize -1.11%
and query 18.45%; the category-equal mean is 6.28%. Negative metrics,
including the second candidate IK-content build and BENG tokenization, were
retained. The LIMIT result is sensitive to client/sampling conditions: this
native pair does not show the Python pair's regression. A robust LIMIT
improvement is not claimed. These data reinforce the ordinary-query benefit
but still do not demonstrate 50% overall reduction. Both original and
candidate again cap the fixed CI reference score locally; formal CI remains
unverified.

An additional per-document IK charset metadata cache passed the full result
regressions but changed the adjacent parser microbenchmark by only about 1%,
inside its sample spread. That experiment was rejected; the committed kernel
and candidate executable were restored exactly to the checkpoint hash.
Raw samples and the rejection decision are retained with the results.

## Further word aggregation and TOKENIZE configuration optimization

The word aggregation hot path called get_refactored followed by an overwriting
set_refactored for each word. This hashed and looked up the same word twice;
the overwrite also allocated a replacement hash node for repeated words.
It now uses the existing set_or_update operation. An in-place callback updates
both the frequency and key, retaining the established initial-count convention
and the latest spelling for collation-equivalent words.

TOKENIZE previously rebuilt/validated its parser properties, serialized that
JSON tree, and immediately parsed the serialization again when initializing
the parse helper. It now materializes the same properties directly from the
validated tree and passes the parsed parser identity/configuration to the
helper. Stored index configurations still use the existing string entry point.
That path and the typed entry point share property decoding and initialization;
the difference between an absent stored string and a nonempty JSON object
remains explicit. All argument, parser and property validation still runs.

A new regression records 768 complete configuration, grouping and error cases
on the independently verified original executable. It covers all five built-in
parsers, default/explicit output, custom and invalid settings, dynamic row
parameters, prepared execution, case/accent-equivalent keys and repeated words.
Exact outputs, including retained word spelling, are compared with the fixture.
It exposed that preserving the first spelling was incompatible: the original
overwriting hash insertion retained the latest spelling. The callback was
corrected before final performance runs. The new fixture was also rechecked
after restarting the original binary. The final candidate passed this suite,
the existing hot-path and batch suites, all 27 collation cases/52,963 result
rows, and two normal driver open/close cycles (3.877 seconds).

The complete benchmark ran in order candidate, original, candidate, original
with the same native client and the same CPU/memory/data-directory configuration
described above. Every run used SKIP_LOAD=0 and all fixed official parameters.
The initial attempt stopped at raw data load because the Windows Python
generator's default stdout encoding was not UTF-8; it produced no complete
report. PYTHONUTF8=1 and PYTHONIOENCODING=utf-8 were then set for all four
complete runs. Official scripts, SQL and data generation were unchanged and
their Git blob hashes were rechecked. No complete run or slow sample was
excluded, and no compilation or profiling ran alongside the measurements.

Build values are seconds; all other values are milliseconds. Each column is
the arithmetic mean of two complete runs.

| Metric | Original native mean | v19 candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.71200 | 7.47150 | 3.12% |
| build_ik_content_sec | 6.04250 | 5.92550 | 1.94% |
| build_beng_en_sec | 3.47850 | 3.35850 | 3.45% |
| tokenize_ik_avg_ms | 0.12805 | 0.12220 | 4.57% |
| tokenize_beng_avg_ms | 0.09115 | 0.08425 | 7.57% |
| query_cn_avg_ms | 1.44500 | 1.09700 | 24.08% |
| query_beng_avg_ms | 2.02215 | 1.52375 | 24.65% |
| query_mixed_avg_ms | 1.64125 | 1.21790 | 25.79% |
| query_limit_avg_ms | 1.85320 | 1.91865 | -3.53% |

The three category reductions are 2.83%, 6.07% and 17.75%; the category-equal
mean is 8.8841%. The unchanged scoring function gives 17.77/100 when supplied
the local original means as an analytical control. This derived comparison
does not replace the checked-in CI baseline. All four reports independently
cap the fixed CI reference score locally, including both originals; that
100/100 reference cannot be attributed to this patch. LIMIT regression and
run-to-run variation remain visible in the calculation. Hit counts remain
8001, 11000, 7332 and 20; the separate complete-result regressions establish
correctness.

Adjacent auxiliary parser diagnostics measured the previous checkpoint at
0.031966/0.009313 ms for IK/BENG and the final candidate at 0.029756/0.008069 ms.
These diagnostics support reduced parser overhead, but are not scoring data.
Intermediate diagnostic results, including a slower intermediate sample, are
also retained. The final native binary hash is
dee9a1fddd9e7fd26ba5ca6914261330bde05a8ad8b0219c884828b7aed22e5d.
Raw reports, sample logs, category calculations, diagnostics and provenance
are the v19 files in [the results directory](../assets/fts-performance/).

## Per-evaluation word maps without bucket locks

An auxiliary BENG instruction-pointer sample on the v19 checkpoint observed
word-map latch lock/unlock functions, alongside stopword checks, charset/hash
work and JSON output. There were only 73 samples, with 33 outside the server
image; this is directional evidence, not an exact CPU percentage. Source
inspection found every mutable word-map instance on the stack of one
synchronous document or query evaluation: TOKENIZE, domain-index row generation,
optimizer query parsing and both legacy/current query iterators.

ObFTWordMap now selects the existing NoPthreadDefendMode. The three query
callers that previously spelled out the locked template use the same alias.
No mutable word map is shared across evaluations; reusable parser helpers do
not own these maps. Shared dictionaries and the stopword set retain their
existing synchronization. All seven affected native compilation units were
rebuilt, including the optimizer and both query iteration implementations.

The adjacent parser diagnostic changed IK 0.029716 -> 0.027223 ms and BENG
0.007968 -> 0.006439 ms, approximately 8.39% and 19.19% reductions from v19.
An additional concurrent regression derives expected IDs independently from
127 input documents, records sequential full rankings, and verifies 1,440
complete ID/ranking/token results across eight independent sessions. This
checks that concurrent query parsing and document parsing remain isolated.
The final candidate passed it, all 768 configuration cases, the existing
408-token/144-ranking/920-predicate/144-concurrent-parse suite, all 280 batch
cases, and all 27 collation cases/52,963 result rows. Two normal driver
open/close cycles also passed in 4.114 seconds.

Two more complete original runs and two complete v20 runs used the same
unmodified native client, official fixed parameters and resource settings.
The order was candidate, original, candidate, original. A local wrapper initially
selected Windows PowerShell 5 and treated normal benchmark stderr progress as
an error before measurement. After switching to PowerShell 7, the first
candidate run completed; a stale Windows TCP listener snapshot then delayed
the next process switch. The port was verified free before resuming at the
next original run. No completed run was repeated, overwritten or excluded.
All samples, including slow English-query samples in both versions, remain
in the arithmetic means below. No compiler or profiler ran during timing.

Build values are seconds; tokenization and query values are milliseconds.

| Metric | Original native mean | v20 candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 7.68950 | 7.52200 | 2.18% |
| build_ik_content_sec | 6.03900 | 5.82500 | 3.54% |
| build_beng_en_sec | 3.59750 | 3.32000 | 7.71% |
| tokenize_ik_avg_ms | 0.12710 | 0.11645 | 8.38% |
| tokenize_beng_avg_ms | 0.08820 | 0.08110 | 8.05% |
| query_cn_avg_ms | 1.43010 | 1.07140 | 25.08% |
| query_beng_avg_ms | 2.08385 | 1.60845 | 22.81% |
| query_mixed_avg_ms | 1.63120 | 1.21660 | 25.42% |
| query_limit_avg_ms | 1.85730 | 1.89315 | -1.93% |

The category improvements are build 4.4785%, tokenize 8.2146% and query
17.8456%; their equal-weight mean is 10.1796%. Applying the unchanged formula
to this separate local control gives 20.36/100. Against the fixed CI reference,
all four local reports again cap at 100/100, including the originals. Formal
CI and a 50% overall reduction remain unproven. The native-client series are
kept separate from earlier experiments; the coarse profiles and micro timings
are not included in scoring.

The v20 files in [the results directory](../assets/fts-performance/) retain
all raw reports, sample logs, score calculations, diagnostics, the five-call-site
ownership audit and exact binary/source hashes. The final native binary hash is
3e1b144c5f8b243936c408270fd5b6f65994896bd0b89bd8b896af2724ce69a6.

## Immutable stopword lookups and final control

The built-in stopword set is private and populated only during parser-data
initialization before query workers can use it. Shutdown destroys it after
workers stop. There is no runtime mutation API. The ordinary zero-timeout
exist_refactored lookup only reads nodes; it does not insert fake/missing
entries. Its key hash/comparison functions read immutable keys, and the
MProtectGuard is a no-op. The set now uses the existing NoPthreadDefendMode.
Only this immutable set changes; dictionary initialization/publication keeps
its synchronization and mutable maps remain scoped to their evaluations.
All three affected native units were rebuilt.

A new independent regression checks 510 complete stopword token/frequency/
document-length outputs across BENG/whitespace parsing, three collations and
eight concurrent sessions. Every built-in stopword, mixed case, repeated
retained words, short tokens, empty/stopword-only input and lookup misses are
covered. The other final regressions also passed: 768 configurations,
408 token cases, 144 rankings, 920 predicates, 144 concurrent parses,
1,440 concurrent map checks, 280 batch cases and 52,963 collation result rows.
Normal driver open/close passed twice in 3.884 seconds. The added IK-prefix
fixture, recorded and rechecked only on the original, adds 810 exact output
checks including four concurrent sessions.

An experimental per-character IK dictionary-code cache was rejected. The
adjacent checkpoint IK micro timings were 0.026367 and 0.026340 ms; the
existing-prefix and full-prefix variants measured 0.026662 and 0.026922 ms.
Both variants passed their complete-result regressions, but neither showed
benefit. The kernel changes were reverted and the tested v20 executable was
restored before the stopword experiment. Rejection diagnostics are retained.
The final stopword-only diagnostic measured BENG 0.006241 ms against checkpoint
0.006527 ms; IK was essentially unchanged. These diagnostics are not scoring
measurements. A corrected PE-image-range build sample still observed charset,
comparison and sort work, with only sparse in-image samples; exact CPU
percentages or a large build-speed claim cannot be inferred from it.

Six complete official-parameter native-client reports ran in alternating order
candidate/original three times. The third pair was added after original IK-all
build times differed (7.590 and 10.638 seconds). Every completed run and slow
sample is retained. No profiler, compiler or regression ran during measurement.
The auxiliary wrapper initially used the wrong working directory and failed
before producing any samples/report. Its failure log is retained; the wrapper
then set its source directory explicitly. Official scripts were unchanged.

Build values are seconds; tokenization and query values are milliseconds.
Each column averages all three complete runs.

| Metric | Original mean | v22 candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 8.64433 | 7.44433 | 13.88% |
| build_ik_content_sec | 6.03767 | 5.92433 | 1.88% |
| build_beng_en_sec | 3.54333 | 3.31267 | 6.51% |
| tokenize_ik_avg_ms | 0.12480 | 0.11960 | 4.17% |
| tokenize_beng_avg_ms | 0.08730 | 0.08127 | 6.91% |
| query_cn_avg_ms | 1.43063 | 1.13347 | 20.77% |
| query_beng_avg_ms | 2.03850 | 1.54887 | 24.02% |
| query_mixed_avg_ms | 1.62913 | 1.23677 | 24.08% |
| query_limit_avg_ms | 1.84913 | 1.94107 | -4.97% |

The category reductions are 7.4230%, 5.5389% and 15.9759%; their
equal-weight mean is 9.6459%. The unchanged formula against this separate
local control gives 19.29/100. The observed LIMIT change is
-4.97% and remains in the calculation. The one slow original build
makes the estimated build/category total sensitive to machine/instance
variation. The more stable v20 series remains separately reported above;
these series are not mixed or used to select faster samples.

All six reports cap the fixed CI reference locally, including all originals.
This is not a formal contest score or evidence that the patch earns 100/100.
A clean standard Bazel/Linux CI build and a 50% overall reduction remain
unverified. Candidate SHA-256: 3b0265d960838504f08fa0c5aa75d8882d7b84f5851caf7e635a58d0c406cbaf.
The v22 raw reports, every sample log, category calculation, lifecycle audit
and source/binary provenance are retained in
[the results directory](../assets/fts-performance/).

## Reproduction

Use a dedicated instance. With an ordinary MySQL-compatible client and the
unchanged competition benchmark available, run from the repository root:

~~~bash
export MYSQL='mysql -h127.0.0.1 -P2891 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2891 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
~~~

Use the same hardware, compiler flags, server resource settings and client
for an unmodified control and a candidate. Keep each report file fresh.
The benchmark deletes/recreates fts_large_bench. Tests and build/profile
activity should finish before timing begins.
