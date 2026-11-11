# Exact quantized screening — 2026-11-11

Comparison baseline: round two, b951bafbc6d65fb8861441165920be9c971e03be.
Candidate implementation: baa27a5f767035d132e52a219f98ced5c40c307e.
Candidate source SHA256:
29cdccb42aac5c0941c2717e5f0c8036cf99565475d1730963be21c4e4ce842e.
Later evidence/workflow commits do not change this source.

No competition submission was made. The last reported platform score remains
E=2816.094040 for the earlier admitted version; no score or ranking is inferred
from the following development measurements. Hidden admission cases remain
unavailable. agent/conversation.json was not edited. Its optimization checkout
raw SHA256 remains
c56b5d4911627c975b7c7cfb1fd283d8c091f80b4e68149112fc7120562ad6cb.
The original competition checkout has a different raw-file fingerprint,
1ced4b41d1a30011ae7d5ad01717f9835611ce9f6faaf7875e1ff6cdb6f5316a;
it is preserved separately. The tracked file has no diff in this iteration.

## Source references and adaptation

Read [Qdrant](https://github.com/qdrant/qdrant/tree/6ab21cac18ebb6f4ae29102c7f8f5cc11affd5de)
at 6ab21cac18ebb6f4ae29102c7f8f5cc11affd5de:
lib/quantization/src/encoded_vectors_u8.rs and
lib/segment/src/vector_storage/quantized/quantized_vectors.rs.
The useful ideas are compact scalar codes, contiguous scans, integer SIMD
distance evaluation and original-vector scoring. No Qdrant dependency is added.

Read [pgvector](https://github.com/pgvector/pgvector/tree/f37c13f68b57d2c3472b2214fbcff699d6d34876)
at f37c13f68b57d2c3472b2214fbcff699d6d34876:
README.md (half precision and binary quantization), src/vector.c,
src/halfvec.c, src/halfutils.c and src/bitutils.c. Its binary-quantization example
reranks a limited candidate set with original vectors to improve recall.
That alone is insufficient for this implementation's exact contract: with
q=1, a=4 and b=-1, sign-bit distance favors a although b is closer in L2.
Half rounding can likewise merge distinct distances. We use a conservative
per-row rejection bound, with no fixed approximate shortlist.

## Implementation and invariants

- Eligible fully cached candidate lists lazily build contiguous int8 codes
  plus uint32 squared code norms. The shared per-table cache budget includes
  raw rows, full/equality directories, every code matrix and its norms.
  Each preparation is capped at 16 MiB. Supported dimensions are 16..16384;
  lists smaller than 256 and all-zero lists use the existing scan.
- A common power-of-two step h makes float32-to-double scaling exact. Codes
  are clamp(round(value/h), -127, 127). Queries outside the scaled open range
  (-128,128) use the existing float scan. Small equality-filter lists with
  fewer than 2048 candidates also retain that scan, avoiding duplicate code
  caches and their preparation cost.
- Four-row AVX2, AVX512BW and AVX512VNNI kernels compute exact integer code
  distances. Runtime CPU dispatch selects supported instructions. VNNI uses
  biased unsigned rows, signed queries and cached norms to recover squared L2.
- Codes only reject demonstrably worse rows once the TopK heap is full.
  Every survivor uses the existing float screen, double score and exact
  integer-lattice tie comparator. Primary-key ties, OFFSET, filters and output
  distances retain their existing semantics.
- Committed changes, equality-column changes and cache eviction invalidate
  codes together with their borrowed vector directory. Transaction overlays
  retain their original execution path. Optional code/norm/query allocation
  failures fall back to the float scan. No storage or durability format changes.

Let P clamp each component to [-127,127] and Q=round(P). P is nonexpansive and
each rounding error has magnitude at most 1/2, so

    ||Q(x/h)-Q(q/h)|| <= ||x-q||/h + sqrt(d).

The implemented rejection threshold is ceil((sqrt(W)/h+sqrt(d)+1)^2), where W
is the existing conservative upper bound on the heap's worst squared distance.
The additional whole code unit dominates binary64 evaluation error for the
useful radii below 32768; larger radii disable rejection. The comparison is
strict, preserving true ties. Per-component differences are at most 254, hence
all code scores are <=64516*16384=1057030144<2^30. VNNI biased dot products and
their reduction subsets have absolute value <=32385*16384<2^29; final bias
correction is evaluated in int64.

## Experiments, including unsuccessful versions

Windows11 / Ryzen7 9700X / GCC16.2. Serial JSON exchanges, fresh directories,
alternating binary order, exact ordered IDs/distances and all-column restart
verification. These process counters and timings are not official cgroup E.

| Trial | Paired runs / queries | Median query ratio | Median write ratio |
| --- | --- | ---: | ---: |
| Initial AVX2 int8 |3 /3000|0.9614x|1.0044x|
| AVX512BW |3 /5000|1.1082x|0.9706x|
| VNNI with loose bound |3 /5000|0.9931x|1.0279x|
| Tight bound, all eligible buckets |5 /1000|1.0687x|1.0072x|
| Candidate with small-filter fallback |5 /1000|1.1860x|0.9864x|

These are medians of within-pair baseline/candidate ratios, not ratios of
independent best times. Early results were noisy and did not justify adoption.
The tight-bound version added about2.3MiB Windows peak working set; avoiding
small bucket codes reduced the candidate's added peak to about1.1MiB.

Linux uses the digest-pinned submission GCC14.3/Debian compiler. Query code
preparation is inside these end-to-end timings, with no separate warmup.
Oracle computation, SQL generation and verification are outside timed calls.
All queries and complete persisted rows are independently checked.

| Linux comparison | Runs | Query ratio | Write ratio | Filtered / unfiltered |
| --- | ---: | ---: | ---: | --- |
| e4f0a925, all eligible buckets, 10000x128D |7|1.0806x|0.9766x|0.9469 /1.1646 |
| baa27a5, adaptive candidate, 10000x128D |7|1.1987x|0.9985x|0.9814 /1.3534 |
| baa27a5, 10000x512D |3|1.2333x|1.0168x|0.9364 /1.3707 |
| baa27a5, 1000x128D |3|0.9156x|1.2149x|0.8948 /0.9347 |
| Unchanged candidate repeat, 10000x128D |7|1.2999x|0.9780x|0.9733 /1.4907 |
| Unchanged candidate repeat, 10000x512D |3|1.4167x|1.0088x|1.0625 /1.5711 |
| Unchanged candidate repeat, 1000x128D |7|1.0136x|0.9868x|1.0117 /1.0154 |
| Same candidate vs itself, 1000x128D |7|1.0282x|1.0122x|1.0233 /1.0331 |

Run42979111275 used an EPYC9V45 with AVX512; run42980185375 used EPYC7763 with
AVX2. Therefore the two runs do not isolate the effect of the filter policy.
Each run's comparison baseline is b951 and shares that run's host/toolchain.
The default candidate query ratios were1.1161..1.2787, all above1; its median
corresponds to about16.6% less query time. Write timing is essentially unchanged.
Repeat run42981071142/job130391373518 used an Intel Xeon Platinum8573C. Its
unchanged candidate gained1.2999x at the default size (about23.1% less query
time), with all seven ratios above1.19. The small-table repeat was1.0136x;
its ratios0.9276..1.1266 overlap the same-binary control's0.8630..1.1045.
This does not reproduce a stable small-table regression or establish a useful
small-table speedup. The earlier negative result is retained. Filtered queries
alone have no established gain. The candidate is retained for repeated broad
scan gains across AVX2 and Intel hardware, accepting its measured memory cost.

## Resource evidence

Run42980718848, job130390231321 used three alternating pairs of fresh
256MiB, 2CPU, no-swap, network-disabled containers. Host driver/oracle stayed
outside each cgroup. Kernel memory.peak and memory.stat were read after load
and after all1000 queries, before verification/restart. Every exact query and
all10000 persisted rows passed. All six containers had zero OOM events.

| Pair | Baseline peak after queries | Candidate peak | Added peak |
| --- | ---: | ---: | ---: |
|1|17444864|18837504|1392640|
|2|17551360|18509824|958464|
|3|17444864|18804736|1359872|

Values are bytes. Candidate anonymous memory was1,269,760..1,277,952 bytes
higher (about1.22MiB); file memory stayed within4096bytes and shmem was zero.
Total peak increased0.91..1.33MiB, including kernel-accounting noise. This is
an explicit memory/time tradeoff. Snapshots are not atomic, and the host's
io.stat is retained as diagnostic attribution rather than official physical
I/O. No claim is made that query construction costs were excluded from memory.

## Correctness and review evidence

- Linux candidate run42980185322/job130388536403 passed59 validation commands:
  normal/scalar public and edge suites, transactions, recovery, long
  differential runs, I/O faults and six statement/commit/catalog allocation
  harnesses. The unchanged official JSONL checker, real image/mount/EOF
  rollback, 256MiB/20000x4096D and64MiB/20000x1024D workloads all passed.
- Standalone native tests checked1648 four-row batches,6592 independent
  uint64-oracle scores per supported AVX2/AVX512BW/VNNI kernel. All three ran
  successfully on Windows and in Linux validation, including unaligned arrays,
  dimensional tails, signed extremes and16384-dimensional overflow patterns.
- Five new black-box groups check rounding/clamping, exact near ties,
  subnormals/extremes, out-of-range probes, equality/full views, transactions,
  cache mutation/eviction and restart. Larger filtered fixtures contain at
  least2048 candidates to actually reach the candidate's filter gate.
- Two injected optional cache-allocation failures and successful cache reuse
  passed locally and in Linux. The failure retains borrowed row pointers and
  publishes no partial codes. Query allocation failure is statically checked
  to disable only the optional path before scanning.
- A Windows diagnostic copy with AVX512 dispatch disabled passed quantized,
  exact-distance and cached-TopK suites. The exact-distance suite checked761
  SQL statements, including680 ordered SELECTs.
- Initial Windows53-command and Linux59-command validations also passed on
  e4f0a925. The initial new suite incorrectly used unsupported DROP TABLE for
  fixture cleanup; removing that unnecessary cleanup fixed the test itself,
  without changing the SQL contract or relaxing assertions.
- Final independent read-only review of b951..4684f81b found no actionable
  Blocker/Major finding in the source, tests or workflows. Earlier mathematical
  and lifecycle reviews identified the optional-allocation fallback issue,
  which was fixed and exercised before Linux candidate validation.

Machine-readable compact reports and fingerprints:
bench/results/performance-quantized-20261111.json. Raw local outputs and
downloaded CI logs: build/performance-quantized-20261111/.

Known preexisting limitation remains: a large distance-sorted LIMIT can retain
many disk rows before result spooling (for example20000 rows of1024D under a
64MiB limit). This change does not address or establish safety for that case.
