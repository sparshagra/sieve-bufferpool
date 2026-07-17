#pragma once
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "policy.h"

struct Stats {
  size_t hits = 0;
  size_t misses = 0;
  size_t evictions = 0;
  double miss_ratio() const {
    size_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(misses) / total;
  }
};

// The buffer pool caches fixed-size disk pages in a fixed number of frames.
//
// Layout: `frames_` is ONE allocation of num_frames * PAGE_SIZE bytes; frame i
// is the slice at i * PAGE_SIZE. `page_table_` maps page_id -> frame index.
// Which page gets thrown out is entirely the policy's decision -- the pool only
// enforces that a pinned page is never a victim.
class BufferPool {
 public:
  // Does not take ownership of disk or policy; both must outlive the pool.
  BufferPool(DiskManager* disk, EvictionPolicy* policy, size_t num_frames);

  // Returns a pointer to the page's frame, pinning it. Loads from disk on a
  // miss, evicting if needed. Returns nullptr only if every frame is pinned.
  char* fetch_page(page_id_t pid);

  // Drops one pin. is_dirty=true marks the frame for write-back on eviction.
  // Returns false if the page was not resident or was not pinned.
  bool unpin_page(page_id_t pid, bool is_dirty);

  void flush_all();

  const Stats& stats() const { return stats_; }
  size_t num_frames() const { return num_frames_; }
  bool is_resident(page_id_t pid) const { return page_table_.count(pid) > 0; }

 private:
  // Per-frame bookkeeping, parallel to the frames_ byte array.
  struct FrameMeta {
    page_id_t page_id = INVALID_PAGE;
    int pin_count = 0;
    bool dirty = false;
  };

  // Frees a frame by asking the policy for a victim. Returns the frame index,
  // or -1 if no victim is available.
  int find_victim_frame();
  char* frame_data(int frame_id) {
    return frames_.data() + static_cast<size_t>(frame_id) * PAGE_SIZE;
  }

  DiskManager* disk_;
  EvictionPolicy* policy_;
  size_t num_frames_;
  std::vector<char> frames_;   // num_frames_ * PAGE_SIZE, allocated once
  std::vector<FrameMeta> meta_;
  std::vector<int> free_list_;  // frame indices not yet holding a page
  std::unordered_map<page_id_t, int> page_table_;
  Stats stats_;
};
