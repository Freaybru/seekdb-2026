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
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  cudaStream_t stream;
  pthread_mutex_t search_mutex;
  float* query_device;
  unsigned* ids_device;
  float* distances_device;
  size_t query_capacity, ids_capacity, distances_capacity;
  long n, dim;
} sk_cuvs_index;

/* Query workspaces belong to one index, never to a previous build or dataset.
 * Limit retained memory so a one-off large batch does not inflate serving VRAM. */
#define SK_CUVS_MAX_CACHED_WORKSPACE (8UL * 1024 * 1024)

static int sk_cuvs_reserve(sk_cuvs_index* h, void** pointer, size_t* capacity,
                           size_t bytes)
{
  if (*capacity >= bytes) return 0;
  void* replacement = NULL;
  if (cuvsRMMAlloc(h->res, &replacement, bytes) != CUVS_SUCCESS) return 1;
  if (*pointer && cuvsRMMFree(h->res, *pointer, *capacity) != CUVS_SUCCESS) {
    cuvsRMMFree(h->res, replacement, bytes);
    return 1;
  }
  *pointer = replacement;
  *capacity = bytes;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  if (!base || n <= 0 || n > UINT_MAX || dim <= 0 ||
      (size_t)n > SIZE_MAX / sizeof(float) / (size_t)dim) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->search_mutex, NULL) != 0) { free(h); return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) goto fail_mutex;
  if (cuvsStreamGet(h->res, &h->stream) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) goto fail_res;

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_sp;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 64;
  h->ip->intermediate_graph_degree = 192;
  /* Reduce NN-descent/pruning work and snapshot size, then use a wider native
   * GPU search to retain end-to-end recall. Input vectors remain FP32. */
  h->ip->nn_descent_niter = 10;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_sp:  cuvsCagraSearchParamsDestroy(h->sp);
fail_res: cuvsResourcesDestroy(h->res);
fail_mutex: pthread_mutex_destroy(&h->search_mutex); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)h->dim ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)topk ||
      (size_t)topk > SIZE_MAX / 4) return 1;
  if (pthread_mutex_lock(&h->search_mutex) != 0) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0;
  cuvsCagraSearchParams_t sp = h->sp;
  const size_t query_bytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t ids_bytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  const size_t distances_bytes = sizeof(float) * (size_t)nq * (size_t)topk;
  const int cached = query_bytes <= SK_CUVS_MAX_CACHED_WORKSPACE / 3 &&
                     ids_bytes <= SK_CUVS_MAX_CACHED_WORKSPACE / 3 &&
                     distances_bytes <= SK_CUVS_MAX_CACHED_WORKSPACE / 3;
  if (cached) {
    if (sk_cuvs_reserve(h, (void**)&h->query_device, &h->query_capacity, query_bytes) ||
        sk_cuvs_reserve(h, (void**)&h->ids_device, &h->ids_capacity, ids_bytes) ||
        sk_cuvs_reserve(h, (void**)&h->distances_device, &h->distances_capacity, distances_bytes)) {
      rc = 10; goto done;
    }
    qd = h->query_device; nd = h->ids_device; dd = h->distances_device;
  } else {
    if (cuvsRMMAlloc(h->res, (void**)&qd, query_bytes) != CUVS_SUCCESS) { rc=10; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&nd, ids_bytes) != CUVS_SUCCESS) { rc=11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&dd, distances_bytes) != CUVS_SUCCESS) { rc=12; goto done; }
  }
  if (cudaMemcpyAsync(qd, query, query_bytes, cudaMemcpyHostToDevice, h->stream) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 4096) required_itopk = 4096;
      sp->itopk_size = required_itopk;
      sp->search_width = 4;
      /* The fixed hipVS SINGLE_CTA kernel supports at most 512 candidates.
       * MULTI_CTA supports this wider beam and parallelizes individual SQLs. */
      sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpyAsync(out_ids, nd, ids_bytes, cudaMemcpyDeviceToHost, h->stream) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpyAsync(out_dist, dd, distances_bytes, cudaMemcpyDeviceToHost, h->stream) != cudaSuccess) { rc=17; goto done; }
  }
done:
  /* cuVS cleanup calls reset its thread-local diagnostic, so report it first. */
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  /* Include all GPU work and host output copies in the call, including errors. */
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS && rc == 0) {
    rc = 18;
    fprintf(stderr, "[seekdb_cuvs] search sync failed: %s\n", cuvsGetLastErrorText());
  }
  if (!cached) {
    if (dd) cuvsRMMFree(h->res, dd, distances_bytes);
    if (nd) cuvsRMMFree(h->res, nd, ids_bytes);
    if (qd) cuvsRMMFree(h->res, qd, query_bytes);
  }
  pthread_mutex_unlock(&h->search_mutex);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  if (h->query_device) cuvsRMMFree(h->res, h->query_device, h->query_capacity);
  if (h->ids_device) cuvsRMMFree(h->res, h->ids_device, h->ids_capacity);
  if (h->distances_device) cuvsRMMFree(h->res, h->distances_device, h->distances_capacity);
  cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->search_mutex);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (pthread_mutex_lock(&index->search_mutex) != 0) return 1;
  int rc = cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS;
  if (cuvsStreamSync(index->res) != CUVS_SUCCESS) rc = 1;
  pthread_mutex_unlock(&index->search_mutex);
  if (rc) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || n > UINT_MAX || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->search_mutex, NULL) != 0) { free(index); return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsStreamGet(index->res, &index->stream) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraSearchParamsCreate(&index->sp) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_sp;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsStreamSync(index->res) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_sp:
  cuvsCagraSearchParamsDestroy(index->sp);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  pthread_mutex_destroy(&index->search_mutex);
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
