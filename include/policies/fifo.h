#pragma once
#include <unordered_map>
#include <vector>

#include "policy.h"

// Plain FIFO: evict whatever was inserted longest ago, ignoring hits entirely.
// Implemented as an index-linked doubly linked list over `capacity_` preallocated
// nodes (no new/delete): `head_` is the newest end (insert point), `tail_` is the
// oldest end (evict point). `loc_` maps a resident page_id to its node index so
// on_access/evict can find it in O(1) instead of scanning.
class FIFOPolicy : public EvictionPolicy {
 public:
  FIFOPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {
    nodes_.resize(capacity);
    free_.reserve(capacity);
    for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) free_.push_back(i);
  }

  // FIFO order is fixed at insertion time; a hit changes nothing.
  void on_access(page_id_t) override {}

  void on_insert(page_id_t pid) override {
    int idx = free_.back();
    free_.pop_back();
    nodes_[idx] = {pid, -1, head_};
    if (head_ != -1) nodes_[head_].prev = idx;
    head_ = idx;
    if (tail_ == -1) tail_ = idx;
    loc_[pid] = idx;
  }

  page_id_t evict() override {
    for (int idx = tail_; idx != -1; idx = nodes_[idx].prev) {
      page_id_t pid = nodes_[idx].page_id;
      if (is_pinned(pid)) continue;
      unlink(idx);
      loc_.erase(pid);
      free_.push_back(idx);
      return pid;
    }
    return INVALID_PAGE;
  }

  const char* name() const override { return "FIFO"; }

 protected:
  struct Node {
    page_id_t page_id = INVALID_PAGE;
    int prev = -1;
    int next = -1;
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
  int tail_ = -1;  // oldest / next to evict
};
