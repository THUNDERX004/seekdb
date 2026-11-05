/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t idx;
  cuvsCagraSearchParams_t sp;
  float *query_dev;
  unsigned *neighbors_dev;
  float *distances_dev;
  size_t query_bytes;
  size_t result_bytes;
  size_t distances_bytes;
  long n, dim;
} sk_cuvs_index;

#ifndef SEEKDB_CUVS_NN_DESCENT_NITER_LOW_DIM
#define SEEKDB_CUVS_NN_DESCENT_NITER_LOW_DIM 5
#endif
#ifndef SEEKDB_CUVS_NN_DESCENT_NITER_HIGH_DIM
#define SEEKDB_CUVS_NN_DESCENT_NITER_HIGH_DIM 50
#endif
#ifndef SEEKDB_CUVS_GRAPH_DEGREE_LOW_DIM
#define SEEKDB_CUVS_GRAPH_DEGREE_LOW_DIM 96
#endif
#ifndef SEEKDB_CUVS_INTERMEDIATE_DEGREE_LOW_DIM
#define SEEKDB_CUVS_INTERMEDIATE_DEGREE_LOW_DIM 128
#endif
#ifndef SEEKDB_CUVS_GRAPH_DEGREE_HIGH_DIM
#define SEEKDB_CUVS_GRAPH_DEGREE_HIGH_DIM 128
#endif
#ifndef SEEKDB_CUVS_INTERMEDIATE_DEGREE_HIGH_DIM
#define SEEKDB_CUVS_INTERMEDIATE_DEGREE_HIGH_DIM 128
#endif

static void sk_release_device_buffers(sk_cuvs_index *h) {
  if (!h || !h->res)
    return;
  if (h->distances_dev) {
    cuvsRMMFree(h->res, h->distances_dev, h->distances_bytes);
    h->distances_dev = NULL;
  }
  if (h->neighbors_dev) {
    cuvsRMMFree(h->res, h->neighbors_dev, h->result_bytes);
    h->neighbors_dev = NULL;
  }
  if (h->query_dev) {
    cuvsRMMFree(h->res, h->query_dev, h->query_bytes);
    h->query_dev = NULL;
  }
  h->query_bytes = 0;
  h->result_bytes = 0;
  h->distances_bytes = 0;
}

static void sk_destroy(sk_cuvs_index *h) {
  if (!h)
    return;
  sk_release_device_buffers(h);
  if (h->sp)
    cuvsCagraSearchParamsDestroy(h->sp);
  if (h->idx)
    cuvsCagraIndexDestroy(h->idx);
  if (h->ip)
    cuvsCagraIndexParamsDestroy(h->ip);
  if (h->res)
    cuvsResourcesDestroy(h->res);
  free(h);
}

static int sk_create_runtime(sk_cuvs_index *h) {
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS)
    return 1;
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS)
    return 2;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS)
    return 3;
  if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS)
    return 4;
  h->sp->search_width = 4;
  return 0;
}

static int sk_resize_device_buffer(cuvsResources_t res, void **ptr,
                                   size_t *capacity, size_t required) {
  void *replacement = NULL;
  if (required <= *capacity)
    return 0;
  if (cuvsRMMAlloc(res, &replacement, required) != CUVS_SUCCESS)
    return 1;
  if (*ptr)
    cuvsRMMFree(res, *ptr, *capacity);
  *ptr = replacement;
  *capacity = required;
  return 0;
}

void *seekdb_cuvs_build(const float *base, long n, long dim) {
  if (!base || n <= 0 || (uint64_t)n > UINT32_MAX || dim <= 0)
    return NULL;
  sk_cuvs_index *h = (sk_cuvs_index *)calloc(1, sizeof(*h));
  if (!h)
    return NULL;
  int runtime_rc = sk_create_runtime(h);
  if (runtime_rc != 0) {
    fprintf(stderr, "[seekdb_cuvs] runtime initialization failed: rc=%d %s\n",
            runtime_rc, cuvsGetLastErrorText());
    sk_destroy(h);
    return NULL;
  }

  DLManagedTensor dset;
  memset(&dset, 0, sizeof(dset));
  int64_t ds[2] = {n, dim};
  dset.dl_tensor.data = (void *)base;
  dset.dl_tensor.device.device_type = kDLCPU;
  dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat;
  dset.dl_tensor.dtype.bits = 32;
  dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds;
  dset.dl_tensor.strides = 0;
  dset.dl_tensor.byte_offset = 0;

  const int high_dim = dim > 128;
  /* The 128-d SIFT path reaches its recall plateau with the 96/128 graph after
   * very few NN-descent rounds. The clustered 256-d Hidden250K workload retains
   * the 128/128/50 graph to preserve the Recall@100 baseline delta. IVF-PQ is
   * not used because it exceeded both Recall and VRAM gates. */
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = high_dim ? SEEKDB_CUVS_GRAPH_DEGREE_HIGH_DIM
                                 : SEEKDB_CUVS_GRAPH_DEGREE_LOW_DIM;
  h->ip->intermediate_graph_degree =
      high_dim ? SEEKDB_CUVS_INTERMEDIATE_DEGREE_HIGH_DIM
               : SEEKDB_CUVS_INTERMEDIATE_DEGREE_LOW_DIM;
  h->ip->nn_descent_niter = high_dim ? SEEKDB_CUVS_NN_DESCENT_NITER_HIGH_DIM
                                     : SEEKDB_CUVS_NN_DESCENT_NITER_LOW_DIM;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS)
    goto fail;
  h->n = n;
  h->dim = dim;
  return h;

fail:
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  sk_destroy(h);
  return NULL;
}

int seekdb_cuvs_search(void *handle, const float *query, long nq, long topk,
                       unsigned int *out_ids, float *out_dist) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || h->dim <= 0)
    return 1;
  if ((size_t)nq > SIZE_MAX / sizeof(float) / (size_t)h->dim ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)topk ||
      (size_t)nq > SIZE_MAX / sizeof(unsigned) / (size_t)topk ||
      (size_t)topk > SIZE_MAX / 2) {
    return 2;
  }
  const size_t query_bytes = (size_t)nq * (size_t)h->dim * sizeof(float);
  const size_t result_bytes = (size_t)nq * (size_t)topk * sizeof(float);
  int rc = 0;
  if (sk_resize_device_buffer(h->res, (void **)&h->query_dev, &h->query_bytes,
                              query_bytes) != 0) {
    rc = 10;
    goto done;
  }
  if (sk_resize_device_buffer(h->res, (void **)&h->neighbors_dev,
                              &h->result_bytes, result_bytes) != 0) {
    rc = 11;
    goto done;
  }
  if (sk_resize_device_buffer(h->res, (void **)&h->distances_dev,
                              &h->distances_bytes, result_bytes) != 0) {
    rc = 12;
    goto done;
  }
  if (cudaMemcpy(h->query_dev, query, query_bytes, cudaMemcpyHostToDevice) !=
      cudaSuccess) {
    rc = 13;
    goto done;
  }
  {
    DLManagedTensor qt, nt, dt;
    int64_t qs[2] = {nq, h->dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
    memset(&qt, 0, sizeof(qt));
    qt.dl_tensor.data = h->query_dev;
    qt.dl_tensor.device.device_type = kDLCUDA;
    qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2;
    qt.dl_tensor.dtype.code = kDLFloat;
    qt.dl_tensor.dtype.bits = 32;
    qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = qs;
    qt.dl_tensor.strides = 0;
    qt.dl_tensor.byte_offset = 0;
    nt = qt;
    nt.dl_tensor.data = h->neighbors_dev;
    nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.shape = nsh;
    dt = qt;
    dt.dl_tensor.data = h->distances_dev;
    dt.dl_tensor.dtype.code = kDLFloat;
    dt.dl_tensor.shape = dsh;
    {
      const size_t itopk_factor = h->dim > 128 ? 4 : 2;
      const size_t itopk_floor = h->dim > 128 ? 512 : 256;
      size_t required_itopk = (size_t)topk * itopk_factor;
      if (required_itopk < itopk_floor)
        required_itopk = itopk_floor;
      if (h->sp->itopk_size < required_itopk)
        h->sp->itopk_size = required_itopk;
    }
    cuvsFilter f;
    f.type = NO_FILTER;
    f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) !=
        CUVS_SUCCESS) {
      rc = 15;
      goto done;
    }
    if (cudaMemcpy(out_ids, h->neighbors_dev,
                   (size_t)nq * topk * sizeof(unsigned),
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
      rc = 16;
      goto done;
    }
    if (out_dist && cudaMemcpy(out_dist, h->distances_dev,
                               (size_t)nq * topk * sizeof(float),
                               cudaMemcpyDeviceToHost) != cudaSuccess) {
      rc = 17;
      goto done;
    }
  }
done:
  if (rc)
    fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc,
            cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void *handle) { sk_destroy((sk_cuvs_index *)handle); }

int seekdb_cuvs_serialize(void *handle, const char *filename) {
  sk_cuvs_index *index = (sk_cuvs_index *)handle;
  if (!index || !filename)
    return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) !=
      CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n",
            cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void *seekdb_cuvs_deserialize(const char *filename, long n, long dim) {
  if (!filename || n <= 0 || dim <= 0)
    return NULL;
  sk_cuvs_index *index = (sk_cuvs_index *)calloc(1, sizeof(*index));
  if (!index)
    return NULL;
  int runtime_rc = sk_create_runtime(index);
  if (runtime_rc != 0)
    goto fail;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS)
    goto fail;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr,
              "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld "
              "x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n",
          cuvsGetLastErrorText());
  sk_destroy(index);
  return NULL;
}

int seekdb_cuvs_graph_info(void *handle, uint64_t *n, uint64_t *graph_degree,
                           uint64_t *graph_bytes) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h || !h->idx || !n || !graph_degree || !graph_bytes)
    return 1;
  int64_t actual_n = 0;
  int64_t degree = 0;
  if (cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
      cuvsCagraIndexGetGraphDegree(h->idx, &degree) != CUVS_SUCCESS ||
      actual_n <= 0 || (uint64_t)actual_n > UINT32_MAX || degree <= 0 ||
      (uint64_t)actual_n > UINT64_MAX / (uint64_t)degree / sizeof(uint32_t)) {
    fprintf(stderr, "[seekdb_cuvs] graph metadata failed: %s\n",
            cuvsGetLastErrorText());
    return 2;
  }
  *n = (uint64_t)actual_n;
  *graph_degree = (uint64_t)degree;
  *graph_bytes = (uint64_t)actual_n * (uint64_t)degree * sizeof(uint32_t);
  return 0;
}

int seekdb_cuvs_copy_graph(void *handle, void *out_graph,
                           uint64_t graph_bytes) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  uint64_t n = 0;
  uint64_t degree = 0;
  uint64_t expected_bytes = 0;
  if (!out_graph ||
      seekdb_cuvs_graph_info(handle, &n, &degree, &expected_bytes) != 0 ||
      graph_bytes != expected_bytes) {
    return 1;
  }
  DLManagedTensor graph;
  memset(&graph, 0, sizeof(graph));
  if (cuvsCagraIndexGetGraph(h->idx, &graph) != CUVS_SUCCESS ||
      !graph.dl_tensor.data || graph.dl_tensor.dtype.code != kDLUInt ||
      graph.dl_tensor.dtype.bits != 32) {
    if (graph.deleter)
      graph.deleter(&graph);
    fprintf(stderr, "[seekdb_cuvs] graph export failed: %s\n",
            cuvsGetLastErrorText());
    return 2;
  }
  cudaError_t copy_rc = cudaMemcpy(out_graph, graph.dl_tensor.data,
                                   (size_t)graph_bytes, cudaMemcpyDeviceToHost);
  if (graph.deleter)
    graph.deleter(&graph);
  if (copy_rc != cudaSuccess) {
    fprintf(stderr, "[seekdb_cuvs] graph copy failed: %s\n",
            cudaGetErrorString(copy_rc));
    return 3;
  }
  return 0;
}

void *seekdb_cuvs_deserialize_from_graph(const void *graph,
                                         uint64_t graph_bytes,
                                         uint64_t graph_degree,
                                         const float *base, long n, long dim) {
  if (!graph || !base || n <= 0 || dim <= 0 || (uint64_t)n > UINT32_MAX ||
      graph_degree <= 0 || graph_degree > UINT32_MAX ||
      (uint64_t)n > SIZE_MAX / sizeof(float) / (size_t)dim ||
      (uint64_t)n > UINT64_MAX / graph_degree / sizeof(uint32_t) ||
      graph_bytes > SIZE_MAX ||
      graph_bytes != (uint64_t)n * graph_degree * sizeof(uint32_t)) {
    return NULL;
  }
  sk_cuvs_index *h = (sk_cuvs_index *)calloc(1, sizeof(*h));
  if (!h)
    return NULL;
  int runtime_rc = sk_create_runtime(h);
  if (runtime_rc != 0)
    goto fail;

  DLManagedTensor dset;
  DLManagedTensor dgraph;
  memset(&dset, 0, sizeof(dset));
  memset(&dgraph, 0, sizeof(dgraph));
  int64_t ds[2] = {n, dim};
  int64_t gs[2] = {n, (int64_t)graph_degree};
  dset.dl_tensor.data = (void *)base;
  dset.dl_tensor.device.device_type = kDLCPU;
  dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat;
  dset.dl_tensor.dtype.bits = 32;
  dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds;
  dgraph.dl_tensor.data = (void *)graph;
  dgraph.dl_tensor.device.device_type = kDLCPU;
  dgraph.dl_tensor.device.device_id = 0;
  dgraph.dl_tensor.ndim = 2;
  dgraph.dl_tensor.dtype.code = kDLUInt;
  dgraph.dl_tensor.dtype.bits = 32;
  dgraph.dl_tensor.dtype.lanes = 1;
  dgraph.dl_tensor.shape = gs;

  if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &dgraph, &dset, h->idx) !=
      CUVS_SUCCESS) {
    goto fail;
  }
  h->n = n;
  h->dim = dim;
  return h;

fail:
  fprintf(stderr, "[seekdb_cuvs] graph deserialize failed: %s\n",
          cuvsGetLastErrorText());
  sk_destroy(h);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float *base, long n, long dim,
                          const float *query, long nq, long topk,
                          unsigned int *out_ids) {
  void *h = seekdb_cuvs_build(base, n, dim);
  if (!h)
    return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
