#pragma once
#include <unordered_map>
#include <vector>

#include "policy.h"

// CLOCK (second-chance): an LRU approximation via a circular buffer of frames
// and one reference bit each -- what Postgres actually uses. A hit just sets
// the bit. To evict, the hand sweeps forward one slot at a time: a set bit is
// cleared and skipped (a "second chance"); the first slot the hand finds with
// a clear bit is the victim. New pages start with the bit set (one free pass).
class ClockPolicy : public EvictionPolicy {
 public:
  ClockPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {
    slots_.resize(capacity);
    free_.reserve(capacity);
    for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) free_.push_back(i);
  }

  void on_access(page_id_t pid) override {
    auto it = loc_.find(pid);
    if (it != loc_.end()) slots_[it->second].ref_bit = true;
  }

  void on_insert(page_id_t pid) override {
    int idx = free_.back();
    free_.pop_back();
    slots_[idx] = {pid, true, true};
    loc_[pid] = idx;
  }

  page_id_t evict() override {
    // Bounded to two full sweeps: one to clear every reference bit, one more
    // to find the (now-clear) victim. If everything is pinned we exhaust the
    // bound and correctly report failure instead of looping forever.
    for (size_t iter = 0; iter < 2 * capacity_ + 1; ++iter) {
      Slot& s = slots_[hand_];
      int cur = hand_;
      hand_ = (hand_ + 1) % capacity_;
      if (!s.occupied) continue;
      if (is_pinned(s.page_id)) continue;
      if (s.ref_bit) {
        s.ref_bit = false;
        continue;
      }
      page_id_t pid = s.page_id;
      s.occupied = false;
      loc_.erase(pid);
      free_.push_back(cur);
      return pid;
    }
    return INVALID_PAGE;
  }

  const char* name() const override { return "CLOCK"; }

 protected:
  struct Slot {
    page_id_t page_id = INVALID_PAGE;
    bool ref_bit = false;
    bool occupied = false;
  };

  std::vector<Slot> slots_;
  std::vector<int> free_;
  std::unordered_map<page_id_t, int> loc_;
  size_t hand_ = 0;
};
