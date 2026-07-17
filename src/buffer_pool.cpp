#include "buffer_pool.h"

BufferPool::BufferPool(DiskManager* disk, EvictionPolicy* policy, size_t num_frames)
    : disk_(disk),
      policy_(policy),
      num_frames_(num_frames),
      frames_(num_frames * PAGE_SIZE, 0),
      meta_(num_frames) {
  // Every frame starts free. Reverse order so frame 0 is handed out first.
  free_list_.reserve(num_frames);
  for (int i = static_cast<int>(num_frames) - 1; i >= 0; --i) free_list_.push_back(i);
  page_table_.reserve(num_frames * 2);
}

// --- Phase 1 implements the bodies below. Stubs keep the toolchain honest. ---

int BufferPool::find_victim_frame() {
  return -1;  // TODO(Phase 1)
}

char* BufferPool::fetch_page(page_id_t /*pid*/) {
  return nullptr;  // TODO(Phase 1)
}

bool BufferPool::unpin_page(page_id_t /*pid*/, bool /*is_dirty*/) {
  return false;  // TODO(Phase 1)
}

void BufferPool::flush_all() {
  // TODO(Phase 1)
}
