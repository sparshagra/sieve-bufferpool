#pragma once
#include <unordered_map>
#include <vector>

#include "policy.h"

// SIEVE (NSDI'24): one FIFO queue + one visited bit per entry + a single hand
// that PERSISTS across evict() calls instead of resetting to the tail each
// time. A hit only sets the visited bit -- the object is never moved
// ("lazy promotion"). New objects always insert at the head, unprotected
// (visited=false), so an object that's never re-accessed ages out fast
// ("quick demotion"). To evict, the hand walks from wherever it stopped last
// time, toward the head: a set bit is cleared and skipped; the first clear
// bit found is the victim, and the hand is left just past it for next time.
class SievePolicy : public EvictionPolicy {
 public:
  SievePolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {
    nodes_.resize(capacity);
    free_.reserve(capacity);
    for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) free_.push_back(i);
  }

  void on_access(page_id_t pid) override {
    auto it = loc_.find(pid);
    if (it != loc_.end()) nodes_[it->second].visited = true;
  }

  void on_insert(page_id_t pid) override {
    int idx = free_.back();
    free_.pop_back();
    nodes_[idx] = {pid, -1, head_, false};
    if (head_ != -1) nodes_[head_].prev = idx;
    head_ = idx;
    if (tail_ == -1) tail_ = idx;
    loc_[pid] = idx;
  }

  page_id_t evict() override {
    if (tail_ == -1) return INVALID_PAGE;  // nothing resident
    // Bounded the same way as CLOCK: two full sweeps are always enough.
    for (size_t iter = 0; iter < 2 * capacity_ + 1; ++iter) {
      if (hand_ == -1) hand_ = tail_;  // wrap: resume sweep at the oldest end
      int idx = hand_;
      page_id_t pid = nodes_[idx].page_id;
      int prev_idx = nodes_[idx].prev;  // toward the head; captured before any mutation

      if (is_pinned(pid)) {
        hand_ = prev_idx;
        continue;
      }
      if (nodes_[idx].visited) {
        nodes_[idx].visited = false;
        hand_ = prev_idx;
        continue;
      }
      hand_ = prev_idx;
      unlink(idx);
      loc_.erase(pid);
      free_.push_back(idx);
      return pid;
    }
    return INVALID_PAGE;
  }

  const char* name() const override { return "SIEVE"; }

 protected:
  struct Node {
    page_id_t page_id = INVALID_PAGE;
    int prev = -1;
    int next = -1;
    bool visited = false;
  };

  void unlink(int idx) {
    Node& n = nodes_[idx];
    if (n.prev != -1) nodes_[n.prev].next = n.next; else head_ = n.next;
    if (n.next != -1) nodes_[n.next].prev = n.prev; else tail_ = n.prev;
  }

  std::vector<Node> nodes_;
  std::vector<int> free_;
  std::unordered_map<page_id_t, int> loc_;
  int head_ = -1;  // newest / most recently inserted
  int tail_ = -1;  // oldest / sweep starts here
  int hand_ = -1;  // -1 means "wrap to tail_ on next evict()"
};
