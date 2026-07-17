// Self-contained test runner: no GoogleTest, no Catch2, just a CHECK macro and
// a pass/fail count. Phases 1-3 add per-policy hand-computed eviction tests.
#include <cstdio>
#include <cstring>
#include <string>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "policy.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (cond) {                                                            \
      ++g_pass;                                                            \
    } else {                                                               \
      ++g_fail;                                                            \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto va_ = (a);                                                        \
    auto vb_ = (b);                                                        \
    if (va_ == vb_) {                                                      \
      ++g_pass;                                                            \
    } else {                                                               \
      ++g_fail;                                                            \
      std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                      \
  } while (0)

namespace {

class NoopPolicy : public EvictionPolicy {
 public:
  NoopPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {}
  void on_access(page_id_t) override {}
  void on_insert(page_id_t) override {}
  page_id_t evict() override { return INVALID_PAGE; }
  const char* name() const override { return "noop"; }
};

void test_stats() {
  Stats s;
  CHECK_EQ(s.miss_ratio(), 0.0);  // no ops => defined as 0, not NaN
  s.hits = 3;
  s.misses = 1;
  CHECK_EQ(s.miss_ratio(), 0.25);
}

void test_pinning_base() {
  NoopPolicy p(4, 16);
  EvictionPolicy* base = &p;
  CHECK(!base->is_pinned(5));
  base->set_pinned(5, true);
  CHECK(base->is_pinned(5));
  base->set_pinned(5, false);
  CHECK(!base->is_pinned(5));
  CHECK_EQ(base->capacity(), size_t{4});
  CHECK_EQ(std::string(base->name()), std::string("noop"));
}

void test_disk_roundtrip() {
  DiskManager disk("results/test.db", 8);
  CHECK_EQ(disk.num_pages(), size_t{8});

  char out[PAGE_SIZE];
  std::memset(out, 0xAB, PAGE_SIZE);
  disk.write_page(2, out);

  char in[PAGE_SIZE];
  std::memset(in, 0, PAGE_SIZE);
  disk.read_page(2, in);
  CHECK_EQ(std::memcmp(in, out, PAGE_SIZE), 0);

  // Page 2 must not bleed into its neighbours: offsets are pid * PAGE_SIZE.
  char other[PAGE_SIZE];
  disk.read_page(3, other);
  CHECK(other[0] == 0);

  CHECK_EQ(disk.num_writes(), size_t{1});
  CHECK_EQ(disk.num_reads(), size_t{2});
}

void test_pool_construction() {
  DiskManager disk("results/test.db", 8);
  NoopPolicy policy(4, 8);
  BufferPool pool(&disk, &policy, 4);
  CHECK_EQ(pool.num_frames(), size_t{4});
  CHECK(!pool.is_resident(0));
  CHECK_EQ(pool.stats().hits, size_t{0});
}

}  // namespace

int main() {
  test_stats();
  test_pinning_base();
  test_disk_roundtrip();
  test_pool_construction();

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
