// Phase 1 stub: exercises a real fetch/unpin/eviction cycle through FIFO so the
// full pipeline (disk -> pool -> policy) is proven end to end. Phase 4 turns
// this into the real benchmark runner (workloads x policies x cache sizes -> CSV).
#include <cstdio>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "policies/fifo.h"

int main() {
  constexpr size_t kNumPages = 64;
  constexpr size_t kNumFrames = 4;

  DiskManager disk("results/bench.db", kNumPages);
  FIFOPolicy policy(kNumFrames, kNumPages);
  BufferPool pool(&disk, &policy, kNumFrames);

  // Fill the pool, then request a 5th page to force an eviction.
  for (page_id_t pid = 0; pid < 4; ++pid) {
    pool.fetch_page(pid);
    pool.unpin_page(pid, false);
  }
  pool.fetch_page(4);
  pool.unpin_page(4, false);

  std::printf("sieve-bufferpool phase 1 stub\n");
  std::printf("  page size    : %zu bytes\n", PAGE_SIZE);
  std::printf("  pool frames  : %zu\n", pool.num_frames());
  std::printf("  policy       : %s\n", policy.name());
  std::printf("  hits/misses  : %zu/%zu\n", pool.stats().hits, pool.stats().misses);
  std::printf("  evictions    : %zu\n", pool.stats().evictions);
  std::printf("  page 0 resident (should be evicted): %s\n",
              pool.is_resident(0) ? "yes" : "no");
  std::printf("  page 4 resident: %s\n", pool.is_resident(4) ? "yes" : "no");
  std::printf("toolchain OK.\n");
  return 0;
}
