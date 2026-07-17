// Phase 0 stub: proves the toolchain builds + links all three translation units
// and that virtual dispatch through EvictionPolicy works. Phase 4 turns this
// into the real benchmark runner (workloads x policies x cache sizes -> CSV).
#include <cstdio>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "policy.h"

namespace {

// A placeholder policy. It is NOT one of the five -- it exists only so Phase 0
// has something concrete to dispatch through. Phase 1 deletes it.
class NoopPolicy : public EvictionPolicy {
 public:
  NoopPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {}
  void on_access(page_id_t) override {}
  void on_insert(page_id_t) override {}
  page_id_t evict() override { return INVALID_PAGE; }
  const char* name() const override { return "noop"; }
};

}  // namespace

int main() {
  constexpr size_t kNumPages = 64;
  constexpr size_t kNumFrames = 8;

  DiskManager disk("results/bench.db", kNumPages);
  NoopPolicy policy(kNumFrames, kNumPages);
  BufferPool pool(&disk, &policy, kNumFrames);

  EvictionPolicy* p = &policy;  // dispatch through the base class
  p->set_pinned(3, true);

  std::printf("sieve-bufferpool phase 0 stub\n");
  std::printf("  page size    : %zu bytes\n", PAGE_SIZE);
  std::printf("  disk pages   : %zu (%zu KB file)\n", disk.num_pages(),
              disk.num_pages() * PAGE_SIZE / 1024);
  std::printf("  pool frames  : %zu\n", pool.num_frames());
  std::printf("  policy       : %s\n", p->name());
  std::printf("  page 3 pinned: %s\n", p->is_pinned(3) ? "yes" : "no");
  std::printf("  miss ratio   : %.2f (no ops yet)\n", pool.stats().miss_ratio());
  std::printf("toolchain OK.\n");
  return 0;
}
