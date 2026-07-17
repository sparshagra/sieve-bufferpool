#pragma once
#include <unordered_map>
#include <vector>

#include "policy.h"

// Classic LRU: on a hit, splice the node to the newest end; evict from the
// oldest end. Same index-linked doubly linked list shape as FIFOPolicy --
// the only behavioral difference is that on_access moves the node, which is
// exactly the O(1) list-splice cost LRU pays on every hit that FIFO doesn't.
class LRUPolicy : public EvictionPolicy {
 public:
  LRUPolicy(size_t capacity, size_t num_pages) : EvictionPolicy(capacity, num_pages) {
    nodes_.resize(capacity);
    free_.reserve(capacity);
    for (int i = static_cast<int>(capacity) - 1; i >= 0; --i) free_.push_back(i);
  }

  void on_access(page_id_t pid) override {
    auto it = loc_.find(pid);
    if (it == loc_.end()) return;
    move_to_head(it->second);
  }

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

  const char* name() const override { return "LRU"; }

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

  void move_to_head(int idx) {
    if (idx == head_) return;
    unlink(idx);
    nodes_[idx].prev = -1;
    nodes_[idx].next = head_;
    if (head_ != -1) nodes_[head_].prev = idx;
    head_ = idx;
    if (tail_ == -1) tail_ = idx;
  }

  std::vector<Node> nodes_;
  std::vector<int> free_;
  std::unordered_map<page_id_t, int> loc_;
  int head_ = -1;  // most recently used
  int tail_ = -1;  // least recently used / next to evict
};
