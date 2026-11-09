// hipVS CAGRA graph-index build / search benchmark for the
// "accelerate HNSW index build with AMD Radeon GPU + hipVS" task.
//
// Build: hipcc -O3 -std=c++17 hipvs_cagra_bench.cpp -o hipvs_cagra_bench -lcuvs
// Run:   ./hipvs_cagra_bench [n] [dim] [k]

#include <hip/hip_runtime.h>

#include <cuvs/core/c_api.h>
#include <cuvs/distance/distance.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/common.h>
#include <dlpack/dlpack.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#define HIP_CHECK(expr)                                                      \
  do {                                                                       \
    hipError_t _e = (expr);                                                  \
    if (_e != hipSuccess) {                                                   \
      fprintf(stderr, "HIP error %s at %s:%d\n", hipGetErrorString(_e), __FILE__, __LINE__); \
      exit(1);                                                               \
    }                                                                        \
  } while (0)

#define CUVS_CHECK(expr)                                                     \
  do {                                                                       \
    cuvsError_t _e = (expr);                                                 \
    if (_e != CUVS_SUCCESS) {                                                 \
      fprintf(stderr, "cuvs error: %s at %s:%d\n", cuvsGetLastErrorText(), __FILE__, __LINE__); \
      exit(1);                                                               \
    }                                                                        \
  } while (0)

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// dlpack helpers -------------------------------------------------------------
DLManagedTensor make_tensor(void *data, int64_t rows, int64_t cols, DLDataType dtype,
                            int64_t *shape_storage, int64_t *stride_storage)
{
  shape_storage[0] = rows;
  shape_storage[1] = cols;
  stride_storage[0] = cols;
  stride_storage[1] = 1;
  DLManagedTensor t;
  t.dl_tensor.data = data;
  t.dl_tensor.device = DLDevice{kDLCUDA, 0};
  t.dl_tensor.ndim = 2;
  t.dl_tensor.dtype = dtype;
  t.dl_tensor.shape = shape_storage;
  t.dl_tensor.strides = stride_storage;
  t.dl_tensor.byte_offset = 0;
  t.manager_ctx = nullptr;
  t.deleter = nullptr;
  return t;
}

constexpr DLDataType kFloat32{kDLFloat, 32, 1};
constexpr DLDataType kUInt32{kDLUInt, 32, 1};

}  // namespace

int main(int argc, char **argv)
{
  const int64_t n = (argc > 1) ? atoll(argv[1]) : 100000;
  const int64_t dim = (argc > 2) ? atoll(argv[2]) : 128;
  const int64_t k = (argc > 3) ? atoll(argv[3]) : 10;
  const int64_t n_queries = std::min<int64_t>(100, n);
  const size_t graph_degree = 64;
  const size_t intermediate_degree = 96;

  printf("dataset: n=%lld dim=%lld k=%lld queries=%lld\n",
         (long long)n, (long long)dim, (long long)k, (long long)n_queries);

  // ---- host data -----------------------------------------------------------
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> h_base(static_cast<size_t>(n) * dim);
  for (auto &v : h_base) v = dist(rng);
  std::vector<float> h_query(static_cast<size_t>(n_queries) * dim);
  for (int64_t q = 0; q < n_queries; ++q) {
    for (int64_t d = 0; d < dim; ++d) h_query[q * dim + d] = dist(rng);
  }

  // ---- device buffers ------------------------------------------------------
  float *d_base = nullptr;
  float *d_query = nullptr;
  uint32_t *d_neighbors = nullptr;
  float *d_distances = nullptr;
  HIP_CHECK(hipMalloc(&d_base, h_base.size() * sizeof(float)));
  HIP_CHECK(hipMalloc(&d_query, h_query.size() * sizeof(float)));
  HIP_CHECK(hipMalloc(&d_neighbors, sizeof(uint32_t) * n_queries * k));
  HIP_CHECK(hipMalloc(&d_distances, sizeof(float) * n_queries * k));
  HIP_CHECK(hipMemcpy(d_base, h_base.data(), h_base.size() * sizeof(float), hipMemcpyHostToDevice));
  HIP_CHECK(hipMemcpy(d_query, h_query.data(), h_query.size() * sizeof(float), hipMemcpyHostToDevice));

  int64_t shape[2], strides[2];
  DLManagedTensor dataset = make_tensor(d_base, n, dim, kFloat32, shape, strides);
  int64_t q_shape[2], q_strides[2];
  DLManagedTensor queries = make_tensor(d_query, n_queries, dim, kFloat32, q_shape, q_strides);
  int64_t nb_shape[2], nb_strides[2];
  DLManagedTensor neighbors = make_tensor(d_neighbors, n_queries, k, kUInt32, nb_shape, nb_strides);
  int64_t ds_shape[2], ds_strides[2];
  DLManagedTensor distances = make_tensor(d_distances, n_queries, k, kFloat32, ds_shape, ds_strides);

  // ---- build (the step this task wants to speed up) ------------------------
  cuvsResources_t res;
  cuvsCagraIndexParams_t index_params;
  cuvsCagraIndex_t index;
  CUVS_CHECK(cuvsResourcesCreate(&res));
  CUVS_CHECK(cuvsCagraIndexParamsCreate(&index_params));
  index_params->metric = L2Expanded;
  index_params->graph_degree = graph_degree;
  index_params->intermediate_graph_degree = intermediate_degree;
  index_params->build_algo = NN_DESCENT;
  CUVS_CHECK(cuvsCagraIndexCreate(&index));

  HIP_CHECK(hipDeviceSynchronize());
  auto t0 = Clock::now();
  CUVS_CHECK(cuvsCagraBuild(res, index_params, &dataset, index));
  HIP_CHECK(hipDeviceSynchronize());
  const double build_ms = ms_since(t0);

  // ---- batch search --------------------------------------------------------
  cuvsCagraSearchParams_t search_params;
  CUVS_CHECK(cuvsCagraSearchParamsCreate(&search_params));
  cuvsFilter filter{};
  filter.type = NO_FILTER;

  HIP_CHECK(hipDeviceSynchronize());
  auto t1 = Clock::now();
  CUVS_CHECK(cuvsCagraSearch(res, search_params, index, &queries, &neighbors, &distances, filter));
  HIP_CHECK(hipDeviceSynchronize());
  const double batch_ms = ms_since(t1);

  std::vector<uint32_t> h_neighbors(static_cast<size_t>(n_queries) * k);
  HIP_CHECK(hipMemcpy(h_neighbors.data(), d_neighbors, h_neighbors.size() * sizeof(uint32_t),
                      hipMemcpyDeviceToHost));

  // ---- CPU exact ground truth + recall@k -----------------------------------
  std::vector<int64_t> gt(static_cast<size_t>(n_queries) * k, -1);
  auto t2 = Clock::now();
#pragma omp parallel for schedule(dynamic)
  for (int64_t q = 0; q < n_queries; ++q) {
    std::vector<std::pair<float, int64_t>> top(k, {1e30f, -1});
    for (int64_t i = 0; i < n; ++i) {
      float s = 0.0f;
      for (int64_t d = 0; d < dim; ++d) {
        const float diff = h_query[q * dim + d] - h_base[i * dim + d];
        s += diff * diff;
      }
      if (s < top.back().first) {
        top.back() = {s, i};
        std::sort(top.begin(), top.end());
      }
    }
    for (int64_t j = 0; j < k; ++j) gt[q * k + j] = top[j].second;
  }
  const double gt_ms = ms_since(t2);

  double hit = 0.0;
  for (int64_t q = 0; q < n_queries; ++q) {
    for (int64_t j = 0; j < k; ++j) {
      const int64_t id = static_cast<int64_t>(h_neighbors[q * k + j]);
      if (std::find(gt.begin() + q * k, gt.begin() + (q + 1) * k, id) != gt.begin() + (q + 1) * k) {
        hit += 1.0;
      }
    }
  }
  const double recall = hit / static_cast<double>(n_queries * k);

  printf("\n===== hipVS CAGRA on AMD GPU =====\n");
  printf("build (graph construction)   : %10.2f ms   (%.1f K vec/s)\n", build_ms,
         n / std::max(build_ms, 1e-6));
  printf("batch search (%lld queries)    : %10.2f ms   (%.0f queries/s)\n",
         (long long)n_queries, batch_ms, n_queries / std::max(batch_ms, 1e-6) * 1000.0);
  printf("per-query latency            : %10.3f ms\n", batch_ms / n_queries);
  printf("recall@%lld                    : %10.4f\n", (long long)k, recall);
  printf("cpu exact ground truth       : %10.2f ms\n", gt_ms);
  printf("graph_degree / intermediate  : %zu / %zu\n", graph_degree, intermediate_degree);

  // ---- cleanup -------------------------------------------------------------
  CUVS_CHECK(cuvsCagraSearchParamsDestroy(search_params));
  CUVS_CHECK(cuvsCagraIndexDestroy(index));
  CUVS_CHECK(cuvsCagraIndexParamsDestroy(index_params));
  CUVS_CHECK(cuvsResourcesDestroy(res));
  HIP_CHECK(hipFree(d_base));
  HIP_CHECK(hipFree(d_query));
  HIP_CHECK(hipFree(d_neighbors));
  HIP_CHECK(hipFree(d_distances));
  return 0;
}
