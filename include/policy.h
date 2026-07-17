#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

using page_id_t = int;
constexpr page_id_t INVALID_PAGE = -1;

// A policy tracks page_ids ONLY. It never touches frame memory or the disk --
// the BufferPool owns those. The pool tells the policy about hits and inserts,
// and asks it "who do I evict?". That narrow interface is what makes the
// policies swappable at runtime.
//
// Pinning lives in the base class because every policy handles it identically:
// a pinned page is simply not a legal victim. pinned_ is preallocated to the
// whole page-id space, so set_pinned never allocates.
class EvictionPolicy {
 public:
  EvictionPolicy(size_t capacity, size_t num_pages)
      : capacity_(capacity), pinned_(num_pages, 0) {}
  virtual ~EvictionPolicy() = default;

  // Page was found resident in the pool (a cache hit).
  virtual void on_access(page_id_t pid) = 0;

  // Page was just loaded into a frame after a miss. The pool guarantees it
  // called evict() first if the pool was full, so the policy is never over
  // capacity when this returns.
  virtual void on_insert(page_id_t pid) = 0;

  // Choose and REMOVE a victim from the policy's own bookkeeping. Returns
  // INVALID_PAGE if every candidate is pinned (pool is then out of frames).
  virtual page_id_t evict() = 0;

  virtual const char* name() const = 0;

  void set_pinned(page_id_t pid, bool pinned) {
    pinned_[static_cast<size_t>(pid)] = pinned ? 1 : 0;
  }
  bool is_pinned(page_id_t pid) const {
    return pinned_[static_cast<size_t>(pid)] != 0;
  }
  size_t capacity() const { return capacity_; }

 protected:
  size_t capacity_;              // number of frames the policy may fill
  std::vector<uint8_t> pinned_;  // indexed by page_id; 1 = un-evictable
};
