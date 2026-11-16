# Exact code centering — 2026-11-16

Baseline implementation: `e0b4c4794c64dd9ac0980b6bb1632a2d36168b99`,
also published as round2 `fe2d98ddc971f4b847ea84a11e58eca0a57ab892`.

## Change and exactness

The optional int8 distance cache now measures finite coordinate extrema and
uses their common midpoint when doing so recovers at least one whole bit of
power-of-two code resolution. A zero-radius table keeps the previous encoding.
Queries use the same cached center and step. Shared equality slices borrow
these parameters from their code matrix; swaps carry the center with the codes,
and existing cache invalidation discards them together before mutation.

A common translation preserves true L2 distance. Code distances still only
reject provably worse candidates; survivors retain the original-vector float
screen, double score and exact lattice/tie comparison. Query coordinates
outside the centered encoding range retain the raw-vector path.

Double subtraction can round the translated coordinates. After power-of-two
scaling, each coordinate lies strictly inside (-128,128), so each subtraction
error is below 2^-45 code units. The combined row/query translation norm is
below 2^-37 for at most 16384 axes. The existing additional whole code unit in
the triangle bound covers that error as well as threshold evaluation error.
Measured code residuals cover the encoding actually emitted, including an
adjacent rounding choice immediately next to a half step. Returned scores and
ordering continue to use original vectors.

The matrix shape and optional allocation count stay the same. A center double
is added to each directory's fixed metadata, with its existing budget charged
through `sizeof(IndexedRows)`. Preparation failure publishes no partial center
or codes and retains the fallback. Parallel policy remains at 65536 candidates.

## Measured scope

Evidence: `bench/results/exact-center-20261116.json`. Linux WSL2, Ryzen 7 9700X,
GCC 15.2, `-std=c++20 -O3 -DNDEBUG -pthread`. Each group has a fresh process
and data directory. Three alternating serial baseline/candidate pairs use
10000 rows, 128 axes, K=10, 32 different probes and 1024 warm timed queries.
Every SELECT response is checked, including first-query and warmup responses:
**38052 independently checked responses** across both binaries.

The oracle uses exact binary32 integer/65536 coordinates, Python integer
squared distances and primary-key tie order. This finer lattice avoids
aligning most coordinates with the existing eight-bit code grid. Four balanced
equality buckets give 2500 candidates for filtered queries. The shifted group
adds 32 to every coordinate; its numerators still fit 24 significant bits.

| Distribution | Unfiltered speedup | Single equality speedup |
| --- | ---: | ---: |
| Uniform [-1,1) | 0.996x | 1.000x |
| Uniform [0,1) | 1.009x | 0.997x |
| Uniform [32,33) | 1.477x | 1.161x |

These are medians of within-pair baseline/candidate pipe round-trip time
ratios. For shifted data all three unfiltered ratios were 1.468–1.478x;
filtered ratios were 1.141–1.178x. Ordinary distributions showed no established
warm-query gain. This is a targeted correction for wasted code range on
offset data, not evidence of a broad score improvement.

Cold preparation remains a tradeoff: in the final signed-data run, median
paired first-query ratios were 0.860x unfiltered and 0.808x filtered, with
millisecond-scale first-query times and variation across runs. Shifted-data
first-query ratios were 1.157x and 1.006x. Writes also varied; this iteration
does not establish a write improvement. These measurements do not provide
cgroup I/O/memory, official E or a hidden-workload prediction.

Two experiments were rejected: lowering the parallel threshold to 8192 slowed
the 10000-row unfiltered case by about 17%; doubling the 64-row VNNI dependency
chains slowed isolated 128/512-axis kernel measurements. Both were reverted.

## Validation

`bench/results/exact-center-validation-20261116.json` records **57/57** Linux
regression commands passing with normal/scalar builds, published cases,
protocol and exact ordering, low-cache disk reads, transactions, restart,
injected I/O/publication failures and three 4000-step random runs.

Additional checks passed:

- Quantized preparation units in normal and forced-scalar builds: extrema,
  positive/negative offsets, bit recovery boundaries, constants, all scales,
  subnormal/extreme floats, dimensional tails, residual guards and cache swaps.
- Quantized allocation failure/reuse and inclusive integer cutoff units.
- Both large TopK black-box tests, including shifted codes and exact ties
  across worker halves and shared equality slices.
- Windows GCC 16.2 static build: seven quantized-screen tests and sixteen
  JSON/legacy protocol tests.

Raw local logs are under `build/round-scan-20261116/`. The implementation was
unchanged during final measurements and regression validation. No conversation
file was edited and no competition-platform evaluation was submitted.

Reproduce performance with independently built baseline and candidate:

```sh
python3 -B bench/evaluate_exact_scan.py --baseline /path/to/baseline \
  --candidate /path/to/candidate --report /new/path/exact-center.json \
  --rows 10000 --dim 128 --queries 1024 --unique-probes 32 --runs 3 --k 10
```
