# CAGRA build optimization report

## Baseline

- Official contest commit: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Branch: `codex/amd-cagra-performance`
- Input and SQL contract: FP32, L2, `type=hnsw, lib=cuvs, m=16,
  ef_construction=200, ef_search=64`
- Official reference build time: SIFT250K 20-27 s, Hidden250K 29-35 s
- Official reference recall: SIFT250K 1.000/1.000, Hidden250K 0.985/0.914

The published reference values are context only. Final comparison must use the
evaluator's paired frozen-baseline run.

## Candidate changes

1. Build parameters are dimension-adaptive. The 128-dimensional path uses
   degree 96/128 with 20 NN-Descent iterations. The 256-dimensional path keeps
   the validated final degree 128 and builds a degree-192 intermediate graph
   with IVF-PQ. A 32-iteration NN-Descent experiment failed the evaluator's
   `hidden250k:recall@100_baseline_delta` gate and was rejected.
2. `OBCUVS03` persists only the final uint32 adjacency graph. The previous V5
   path serialized the complete CAGRA index, including a second dataset copy,
   to a temporary file and then copied that file into the seekdb snapshot.
3. Snapshot reload uses the pinned hipVS `cuvsCagraIndexFromArgs` API with the
   persisted adjacency graph and original FP32 vectors. Search still uses the
   pinned `cuvsCagraSearch` implementation.
4. The bridge caches CAGRA search parameters and device query/result buffers on
   the index handle. It uses `itopk_size=max(2*k, 256)` with `search_width=4`;
   public 250K sweeps measured unchanged Recall and 1.2x-1.3x query throughput
   at this queue size.

`OBCUVS02` snapshots remain readable for backward compatibility.

The previous passing evaluator run reached `build=1.375x, query=1.083x` at
commit `eb25cf55c...`. The parameter combination above targets the published
3.0x build threshold by moving the expensive 256-dimensional path to IVF-PQ and
the 128-dimensional path to the measured 96/128/20 profile.

## Correctness invariants

- Build and query remain on the `lib=cuvs` GPU path.
- The FP32 vectors and `id -> row-offset` mapping are preserved in the snapshot.
- Query search still calls the evaluator-provided hipVS/cuVS CAGRA search.
- Official SQL, index parameters, inputs, and datasets are not changed.
- `lib=vsag` continues to use the existing CPU VSAG implementation.

## Reproduction

Build with the contest commands from the task statement:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

Run persistence, graph reconstruction, restart, GPU-path, and stale-ID checks:

```bash
docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_persistence_smoke.sh \
  <container> <container-tsv-path> <host-runtime-dir> <database>
```

Measure the DDL plus first-query interval for either backend:

```bash
docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_build_benchmark.sh \
  <container> <container-tsv-path> <output-dir> <database> cuvs candidate

docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_build_benchmark.sh \
  <container> <container-tsv-path> <output-dir> <database> vsag cpu
```

Use `nq=4096` Top-10 and Top-100 SQL runs for the official query gate and
compute Recall against the exact results. Do not substitute the single-row
timing probe for the official query benchmark.

## Local validation status

The source-only candidate has passed:

- strict GCC C checking against API-shape stubs derived from the pinned hipVS
  commit's public headers;
- strict C++ syntax checking of the bridge persistence test;
- unity compilation of `ob_vsag_adaptor.cpp` on the repository's Windows
  RelWithDebInfo command line with both `OB_BUILD_CUVS=OFF` and
  `OB_BUILD_CUVS=ON`;
- `git diff --check`; and
- shell syntax validation of the benchmark and persistence runners.

This Windows host has no ROCm/AMD runtime, so GPU build time, Recall, QPS, and
VRAM must be recorded from the official cloud environment before treating this
as a scored performance result.
