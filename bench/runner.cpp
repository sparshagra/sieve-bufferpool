// Sweeps every workload x policy x cache size, writing results/results.csv.
// Each combination is measured twice: once with real disk I/O (for miss
// ratio) and once with I/O disabled (for the ops/sec policy-overhead figure).
// Build with -O2 -- the ops/sec numbers are meaningless otherwise.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "policies/clock.h"
#include "policies/fifo.h"
#include "policies/lru.h"
#include "policies/s3fifo.h"
#include "policies/sieve.h"
#include "traces.h"

namespace {

constexpr size_t kKeySpace = 1000;
constexpr size_t kTraceLength = 20000;
const std::vector<size_t> kCacheSizes = {20, 50, 100, 200, 400};

struct Result {
  std::string workload;
  std::string policy;
  size_t cache_size;
  size_t hits;
  size_t misses;
  double miss_ratio;
  double ops_per_sec;
};

void drive_trace(BufferPool& pool, const Trace& trace) {
  for (page_id_t pid : trace.requests) {
    pool.fetch_page(pid);
    pool.unpin_page(pid, false);
  }
}

// Real disk I/O, for the miss-ratio numbers that go in results.csv / the PNGs.
template <typename PolicyT>
Result measure_miss_ratio(const Trace& trace, size_t cache_size) {
  DiskManager disk("results/bench.db", trace.key_space);
  PolicyT policy(cache_size, trace.key_space);
  BufferPool pool(&disk, &policy, cache_size);
  drive_trace(pool, trace);

  Result r;
  r.workload = trace.name;
  r.policy = policy.name();
  r.cache_size = cache_size;
  r.hits = pool.stats().hits;
  r.misses = pool.stats().misses;
  r.miss_ratio = pool.stats().miss_ratio();
  return r;
}

// I/O disabled: isolates policy bookkeeping cost (hash lookups, list splices)
// from actual file access, so this is comparable across policies.
template <typename PolicyT>
double measure_ops_per_sec(const Trace& trace, size_t cache_size) {
  DiskManager disk("results/bench.db", trace.key_space);
  disk.set_io_enabled(false);
  PolicyT policy(cache_size, trace.key_space);
  BufferPool pool(&disk, &policy, cache_size);

  auto start = std::chrono::steady_clock::now();
  drive_trace(pool, trace);
  auto end = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(end - start).count();
  return secs > 0.0 ? static_cast<double>(trace.requests.size()) / secs : 0.0;
}

template <typename PolicyT>
void run_policy(const std::vector<Trace>& workloads, std::vector<Result>& out) {
  for (const auto& trace : workloads) {
    for (size_t cache_size : kCacheSizes) {
      Result r = measure_miss_ratio<PolicyT>(trace, cache_size);
      r.ops_per_sec = measure_ops_per_sec<PolicyT>(trace, cache_size);
      std::printf("  %-16s %-8s cache=%4zu miss_ratio=%.4f ops/sec=%.0f\n", r.workload.c_str(),
                  r.policy.c_str(), cache_size, r.miss_ratio, r.ops_per_sec);
      out.push_back(r);
    }
  }
}

void write_csv(const std::vector<Result>& results, const std::string& path) {
  std::ofstream out(path);
  out << "workload,policy,cache_size,hits,misses,miss_ratio,ops_per_sec\n";
  for (const auto& r : results) {
    out << r.workload << ',' << r.policy << ',' << r.cache_size << ',' << r.hits << ','
        << r.misses << ',' << r.miss_ratio << ',' << r.ops_per_sec << '\n';
  }
}

}  // namespace

int main() {
  std::vector<Trace> workloads;
  workloads.push_back(make_zipfian_trace(0.6, kKeySpace, kTraceLength, 1));
  workloads.push_back(make_zipfian_trace(0.8, kKeySpace, kTraceLength, 2));
  workloads.push_back(make_zipfian_trace(1.0, kKeySpace, kTraceLength, 3));
  workloads.push_back(make_sequential_scan_trace(kKeySpace, kTraceLength));
  workloads.push_back(make_hot_set_shift_trace(kKeySpace, kTraceLength, 4));

  std::printf("running benchmarks (-O2, %zu requests/workload, %zu workloads x %zu policies x %zu cache sizes)...\n",
              kTraceLength, workloads.size(), size_t{5}, kCacheSizes.size());

  std::vector<Result> results;
  run_policy<FIFOPolicy>(workloads, results);
  run_policy<LRUPolicy>(workloads, results);
  run_policy<ClockPolicy>(workloads, results);
  run_policy<SievePolicy>(workloads, results);
  run_policy<S3FIFOPolicy>(workloads, results);

  write_csv(results, "results/results.csv");
  std::printf("wrote results/results.csv (%zu rows)\n", results.size());
  return 0;
}
