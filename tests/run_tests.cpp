// Self-contained test runner: no GoogleTest, no Catch2, just a CHECK macro and
// a pass/fail count. Phases 2-3 add CLOCK/SIEVE/S3-FIFO tests the same way.
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "policies/clock.h"
#include "policies/fifo.h"
#include "policies/lru.h"
#include "policies/s3fifo.h"
#include "policies/sieve.h"
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

void test_stats() {
  Stats s;
  CHECK_EQ(s.miss_ratio(), 0.0);  // no ops => defined as 0, not NaN
  s.hits = 3;
  s.misses = 1;
  CHECK_EQ(s.miss_ratio(), 0.25);
}

void test_pinning_base() {
  FIFOPolicy p(4, 16);
  EvictionPolicy* base = &p;
  CHECK(!base->is_pinned(5));
  base->set_pinned(5, true);
  CHECK(base->is_pinned(5));
  base->set_pinned(5, false);
  CHECK(!base->is_pinned(5));
  CHECK_EQ(base->capacity(), size_t{4});
  CHECK_EQ(std::string(base->name()), std::string("FIFO"));
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
  FIFOPolicy policy(4, 8);
  BufferPool pool(&disk, &policy, 4);
  CHECK_EQ(pool.num_frames(), size_t{4});
  CHECK(!pool.is_resident(0));
  CHECK_EQ(pool.stats().hits, size_t{0});
}

// Drives a bare policy (no BufferPool/disk involved) through a trace, mirroring
// exactly the protocol BufferPool uses: hit -> on_access, miss when full ->
// evict() then on_insert, miss with room -> on_insert only. Returns the
// sequence of evicted page_ids, in order.
std::vector<page_id_t> simulate(EvictionPolicy& policy, const std::vector<page_id_t>& trace) {
  std::unordered_set<page_id_t> resident;
  std::vector<page_id_t> evictions;
  for (page_id_t pid : trace) {
    if (resident.count(pid)) {
      policy.on_access(pid);
      continue;
    }
    if (resident.size() >= policy.capacity()) {
      page_id_t victim = policy.evict();
      resident.erase(victim);
      evictions.push_back(victim);
    }
    policy.on_insert(pid);
    resident.insert(pid);
  }
  return evictions;
}

// Hand-computed on paper: FIFO never reorders on a hit, so eviction order is
// purely insertion order regardless of the two hits at positions 8 and 9.
void test_fifo_hand_computed() {
  FIFOPolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 2, 3, 4, 1, 2, 5, 1, 2, 6};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {1, 2, 3, 4, 1};
  CHECK_EQ(evictions, expected);
}

// Hand-computed on paper: the hit on page 1 at position 4 promotes it, so it
// survives one extra eviction round compared to plain FIFO on a similar trace.
void test_lru_hand_computed() {
  LRUPolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 2, 3, 1, 4, 2, 5, 3, 1, 2};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {2, 3, 1, 4, 2, 5};
  CHECK_EQ(evictions, expected);
}

// Hand-computed on paper: pages 1,2,3 fill the 3 slots with ref_bit=true (set
// on insert). Requesting page 4 forces a full sweep that clears all three
// bits without evicting anything, then a second pass evicts page 1 (the
// first slot revisited). Page 2's later hit refreshes its bit before the next
// eviction, so page 3 -- never touched again -- is evicted instead of it.
void test_clock_hand_computed() {
  ClockPolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 2, 3, 4, 2, 5};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {1, 3};
  CHECK_EQ(evictions, expected);
}

// Hand-computed on paper (see PROJECT_TRACKER.md Phase 2 notes for the full
// step-by-step derivation). Demonstrates lazy promotion (the two hits at
// positions 4-5 never move pages 1/2 in the list, only set their visited
// bit), the hand persisting mid-sweep across separate evict() calls (request
// 8 evicts immediately using the hand position left over from request 7,
// with no extra skipping), and quick demotion (page 4, inserted unprotected
// at request 6, is evicted three requests later at request 11 having never
// been re-accessed).
void test_sieve_hand_computed() {
  SievePolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 2, 3, 1, 2, 4, 5, 6, 5, 6, 7};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {3, 1, 2, 4};
  CHECK_EQ(evictions, expected);
}

// Hand-computed, capacity 3 => small=1, main=2, ghost=2. Exercises the two
// paths that only exist in main: pages 1,2,3 each earn a second access, so the
// eviction at request 7 promotes all three out of small, overflowing main and
// forcing evict_main (which evicts 1, freq 0). Later, page 2's hit at request 8
// gives it freq 1, so when main overflows again at request 11 page 2 is
// FIFO-reinserted (freq 1->0, back to main's head) and page 3 is evicted in its
// place. See PROJECT_TRACKER.md Phase 3 for the full step-by-step state.
void test_s3fifo_hand_computed() {
  S3FIFOPolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 1, 2, 2, 3, 3, 4, 2, 5, 5, 6};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {1, 4, 3};
  CHECK_EQ(evictions, expected);
}

// Hand-computed, capacity 3 => small=1, main=2, ghost=2. Page 1 is evicted from
// small as a one-hit wonder at request 4 (its key lands in ghost), then
// re-requested at request 5 -- the ghost hit sends it straight to main. The
// four requests that follow churn small completely, and page 1 is never evicted
// again, which is what proves the promotion took effect: if the ghost lookup
// were broken and page 1 had re-entered small, it would reach small's tail and
// be evicted a second time, giving {1,2,3,4,1} instead.
void test_s3fifo_ghost_promotion() {
  S3FIFOPolicy policy(3, 16);
  std::vector<page_id_t> trace = {1, 2, 3, 4, 1, 5, 6, 7};
  auto evictions = simulate(policy, trace);
  std::vector<page_id_t> expected = {1, 2, 3, 4, 5};
  CHECK_EQ(evictions, expected);
}

// Invariant: a pinned page is never returned by evict(), even when it sits at
// the natural eviction end of the list.
void test_pin_invariant(EvictionPolicy& policy, const char* label) {
  policy.on_insert(10);
  policy.on_insert(20);
  policy.on_insert(30);  // fills capacity 3, insertion order: 10, 20, 30
  policy.set_pinned(10, true);

  page_id_t victim = policy.evict();
  CHECK(victim != 10);
  CHECK_EQ(victim, page_id_t{20});  // 10 is pinned, so 20 is next in line

  std::printf("  (pin invariant OK for %s)\n", label);
}

// Invariant: if every resident page is pinned, evict() must report failure
// (INVALID_PAGE) rather than evicting a pinned page or a phantom one.
void test_all_pinned_invariant(EvictionPolicy& policy, const char* label) {
  policy.on_insert(1);
  policy.on_insert(2);
  policy.set_pinned(1, true);
  policy.set_pinned(2, true);
  CHECK_EQ(policy.evict(), INVALID_PAGE);
  std::printf("  (all-pinned invariant OK for %s)\n", label);
}

}  // namespace

int main() {
  test_stats();
  test_pinning_base();
  test_disk_roundtrip();
  test_pool_construction();
  test_fifo_hand_computed();
  test_lru_hand_computed();
  test_clock_hand_computed();
  test_sieve_hand_computed();
  test_s3fifo_hand_computed();
  test_s3fifo_ghost_promotion();

  {
    FIFOPolicy p(3, 64);
    test_pin_invariant(p, "FIFO");
  }
  {
    LRUPolicy p(3, 64);
    test_pin_invariant(p, "LRU");
  }
  {
    ClockPolicy p(3, 64);
    test_pin_invariant(p, "CLOCK");
  }
  {
    SievePolicy p(3, 64);
    test_pin_invariant(p, "SIEVE");
  }
  {
    S3FIFOPolicy p(3, 64);
    test_pin_invariant(p, "S3-FIFO");
  }
  {
    FIFOPolicy p(2, 64);
    test_all_pinned_invariant(p, "FIFO");
  }
  {
    LRUPolicy p(2, 64);
    test_all_pinned_invariant(p, "LRU");
  }
  {
    ClockPolicy p(2, 64);
    test_all_pinned_invariant(p, "CLOCK");
  }
  {
    SievePolicy p(2, 64);
    test_all_pinned_invariant(p, "SIEVE");
  }
  {
    S3FIFOPolicy p(2, 64);
    test_all_pinned_invariant(p, "S3-FIFO");
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
