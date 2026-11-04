// Standalone GPU regression: compile against the production bridge C ABI.
#include "tools/hipvs_bridge/seekdb_cuvs_bridge.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char **argv)
{
  require(argc == 2, "provide a snapshot filename in the test run directory");
  constexpr long count = 4096;
  constexpr long dim = 128;
  std::mt19937 random(20261104);
  std::normal_distribution<float> normal;
  std::vector<float> base(count * dim);
  for (float &x : base) { x = normal(random); }
  require(!seekdb_cuvs_build(nullptr, count, dim), "reject null build input");
  void *handle = seekdb_cuvs_build(base.data(), count, dim);
  require(handle != nullptr, "build index");
  std::vector<unsigned> expected(10);
  std::vector<float> distances(10);
  require(seekdb_cuvs_search(handle, base.data(), 1, 10, expected.data(), distances.data()) == 0,
          "first query");
  require(expected.front() == 0 && std::fabs(distances.front()) < 0.001f, "self match");
  require(seekdb_cuvs_search(handle, nullptr, 1, 10, expected.data(), nullptr) != 0, "reject null query");
  require(seekdb_cuvs_search(handle, base.data(), 0, 10, expected.data(), nullptr) != 0, "reject empty query");
  require(seekdb_cuvs_search(handle, base.data(), 1, count + 1, expected.data(), nullptr) != 0,
          "reject oversized topk");

  // The first batch grows cached buffers; the second exceeds the cache cap.
  for (long nq : {37L, 8192L}) {
    std::vector<float> queries(nq * dim);
    for (long i = 0; i < nq; ++i) {
      std::copy_n(base.data() + (i % count) * dim, dim, queries.data() + i * dim);
    }
    std::vector<unsigned> ids(nq * 100);
    std::vector<float> ds(nq * 100);
    require(seekdb_cuvs_search(handle, queries.data(), nq, 100, ids.data(), ds.data()) == 0,
            "cached and uncached batches");
    for (long i = 0; i < nq; ++i) {
      std::set<unsigned> unique;
      require(ids[i * 100] == static_cast<unsigned>(i % count), "batch self match");
      for (long k = 0; k < 100; ++k) {
        unsigned id = ids[i * 100 + k];
        require(id < count && unique.insert(id).second, "valid unique IDs");
        double exact = 0;
        for (long d = 0; d < dim; ++d) {
          const double diff = double(queries[i * dim + d]) - base[id * dim + d];
          exact += diff * diff;
        }
        require(std::isfinite(ds[i * 100 + k]) &&
                std::fabs(exact - ds[i * 100 + k]) < 0.01 + exact * 0.0001,
                "FP32 squared L2 distances");
      }
    }
  }

  std::atomic<bool> success{true};
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&] {
      for (int repeat = 0; repeat < 40; ++repeat) {
        unsigned ids[10];
        if (seekdb_cuvs_search(handle, base.data(), 1, 10, ids, nullptr) != 0 ||
            !std::equal(std::begin(ids), std::end(ids), expected.begin())) {
          success = false;
        }
      }
    });
  }
  for (auto &thread : threads) { thread.join(); }
  require(success, "concurrent queries and workspace growth preserve results");
  require(seekdb_cuvs_serialize(handle, argv[1]) == 0, "serialize after cached queries");
  seekdb_cuvs_free(handle);
  require(!seekdb_cuvs_deserialize(argv[1], count - 1, dim), "reject snapshot shape mismatch");
  handle = seekdb_cuvs_deserialize(argv[1], count, dim);
  require(handle != nullptr, "reload index");
  unsigned loaded[10];
  require(seekdb_cuvs_search(handle, base.data(), 1, 10, loaded, nullptr) == 0 &&
          std::equal(std::begin(loaded), std::end(loaded), expected.begin()),
          "reload preserves results");
  seekdb_cuvs_free(handle);
  require(std::remove(argv[1]) == 0, "remove test snapshot");
  for (long small : {256L, 512L, 1024L}) {
    handle = seekdb_cuvs_build(base.data(), small, dim);
    require(handle != nullptr, "small index build");
    require(seekdb_cuvs_search(handle, base.data(), 1, 10, loaded, nullptr) == 0 && loaded[0] == 0,
            "wide search beam on small index");
    seekdb_cuvs_free(handle);
  }
  std::puts("PASS cached/uncached batches, concurrency, distances, validation, snapshot reload");
}
