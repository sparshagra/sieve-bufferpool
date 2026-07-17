#pragma once
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "policy.h"

// S3-FIFO (SOSP'23): three FIFO queues -- small (~10%), main (~90%), and a
// ghost queue holding only the KEYS of pages evicted from small.
//
// The insight is that most cache misses are "one-hit wonders": pages touched
// once and never again. Every new page enters `small`, so those die there fast
// without ever polluting `main`. A page that earns a second access before
// reaching small's tail is promoted to main instead. If a page evicted from
// small is requested again soon, its key is still in ghost -- proof it wasn't a
// one-hit wonder after all -- so it skips small and goes straight to main.
// Main itself uses FIFO-reinsertion (CLOCK-like, counters capped at 3).
class S3FIFOPolicy : public EvictionPolicy {
 public:
  S3FIFOPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {
    s_capacity_ = std::max<size_t>(1, capacity / 10);
    m_capacity_ = capacity > s_capacity_ ? capacity - s_capacity_ : 0;
    ghost_capacity_ = std::max<size_t>(1, m_capacity_);  // ~= main's object count
    nodes_.resize(capacity);                             // small + main never exceed capacity
    ghost_ring_.assign(ghost_capacity_, INVALID_PAGE);
    free_.reserve(capacity);
    for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) free_.push_back(i);
  }

  void on_access(page_id_t pid) override {
    auto it = loc_.find(pid);
    if (it == loc_.end()) return;
    int& f = nodes_[it->second].freq;
    if (f < kMaxFreq) ++f;  // capped at 3: bounds how long main can protect a page
  }

  void on_insert(page_id_t pid) override {
    int idx = free_.back();
    free_.pop_back();
    nodes_[idx] = Node{pid, -1, -1, 0};
    loc_[pid] = idx;
    if (ghost_set_.erase(pid) > 0) {
      link_head(idx, m_head_, m_tail_, m_size_);  // seen before -> straight to main
    } else {
      link_head(idx, s_head_, s_tail_, s_size_);
    }
  }

  page_id_t evict() override {
    // Evict from whichever queue is over its share, falling back to the other
    // so a caller always gets a victim if any unpinned page exists at all.
    if (s_size_ >= s_capacity_) {
      page_id_t v = evict_small();
      return v != INVALID_PAGE ? v : evict_main();
    }
    page_id_t v = evict_main();
    return v != INVALID_PAGE ? v : evict_small();
  }

  const char* name() const override { return "S3-FIFO"; }

 protected:
  static constexpr int kMaxFreq = 3;
  static constexpr int kPromoteThreshold = 1;  // accessed >=1 time => earns main

  struct Node {
    page_id_t page_id = INVALID_PAGE;
    int prev = -1;  // toward head (newer)
    int next = -1;  // toward tail (older)
    int freq = 0;
  };

  void link_head(int idx, int& head, int& tail, size_t& size) {
    nodes_[idx].prev = -1;
    nodes_[idx].next = head;
    if (head != -1) nodes_[head].prev = idx;
    head = idx;
    if (tail == -1) tail = idx;
    ++size;
  }

  void unlink(int idx, int& head, int& tail, size_t& size) {
    Node& n = nodes_[idx];
    if (n.prev != -1) nodes_[n.prev].next = n.next; else head = n.next;
    if (n.next != -1) nodes_[n.next].prev = n.prev; else tail = n.prev;
    --size;
  }

  // Oldest unpinned node in a queue, scanning from tail toward head.
  int first_unpinned(int tail) const {
    while (tail != -1 && is_pinned(nodes_[tail].page_id)) tail = nodes_[tail].prev;
    return tail;
  }

  void release(int idx, page_id_t pid) {
    loc_.erase(pid);
    free_.push_back(idx);
  }

  page_id_t evict_small() {
    // Each pass either promotes a page out of small or evicts one, so small
    // strictly shrinks and the bound is never actually reached.
    for (size_t guard = 0; guard < capacity_ + 2; ++guard) {
      int idx = first_unpinned(s_tail_);
      if (idx == -1) return INVALID_PAGE;
      page_id_t pid = nodes_[idx].page_id;
      if (nodes_[idx].freq >= kPromoteThreshold) {
        unlink(idx, s_head_, s_tail_, s_size_);
        nodes_[idx].freq = 0;  // protection comes from main's head position, not a carried count
        link_head(idx, m_head_, m_tail_, m_size_);
        if (m_size_ > m_capacity_) {
          page_id_t v = evict_main();
          if (v != INVALID_PAGE) return v;
        }
        continue;
      }
      unlink(idx, s_head_, s_tail_, s_size_);  // never re-accessed: a one-hit wonder
      release(idx, pid);
      ghost_push(pid);  // remember the key so a soon re-request can prove us wrong
      return pid;
    }
    return INVALID_PAGE;
  }

  page_id_t evict_main() {
    // Bounded by 4*capacity: every reinsertion decrements a freq capped at 3,
    // so freqs cannot stall the scan forever.
    for (size_t guard = 0; guard < 4 * capacity_ + 4; ++guard) {
      int idx = first_unpinned(m_tail_);
      if (idx == -1) return INVALID_PAGE;
      page_id_t pid = nodes_[idx].page_id;
      if (nodes_[idx].freq > 0) {
        --nodes_[idx].freq;  // FIFO-reinsertion: second chance, back to the head
        unlink(idx, m_head_, m_tail_, m_size_);
        link_head(idx, m_head_, m_tail_, m_size_);
        continue;
      }
      unlink(idx, m_head_, m_tail_, m_size_);
      release(idx, pid);
      return pid;
    }
    return INVALID_PAGE;
  }

  // Ghost is keys only -- no frame, no page data. A key dropped by on_insert
  // leaves a stale ring slot behind; it is harmless (the set is the source of
  // truth for membership) and gets overwritten as the ring wraps.
  void ghost_push(page_id_t pid) {
    if (ghost_set_.count(pid)) return;
    if (ghost_size_ == ghost_capacity_) {
      ghost_set_.erase(ghost_ring_[ghost_head_]);
      ghost_head_ = (ghost_head_ + 1) % ghost_capacity_;
      --ghost_size_;
    }
    ghost_ring_[(ghost_head_ + ghost_size_) % ghost_capacity_] = pid;
    ++ghost_size_;
    ghost_set_.insert(pid);
  }

  std::vector<Node> nodes_;
  std::vector<int> free_;
  std::unordered_map<page_id_t, int> loc_;  // page_id -> node index (small or main)
  int s_head_ = -1, s_tail_ = -1;
  int m_head_ = -1, m_tail_ = -1;
  size_t s_size_ = 0, m_size_ = 0;
  size_t s_capacity_ = 0, m_capacity_ = 0;

  std::vector<page_id_t> ghost_ring_;
  std::unordered_set<page_id_t> ghost_set_;
  size_t ghost_capacity_ = 0, ghost_head_ = 0, ghost_size_ = 0;
};
