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

int BufferPool::find_victim_frame() {
  if (!free_list_.empty()) {
    int frame = free_list_.back();
    free_list_.pop_back();
    return frame;
  }
  page_id_t victim = policy_->evict();
  if (victim == INVALID_PAGE) return -1;  // every resident page is pinned

  int frame = page_table_.at(victim);
  if (meta_[frame].dirty) disk_->write_page(victim, frame_data(frame));
  page_table_.erase(victim);
  ++stats_.evictions;
  return frame;
}

char* BufferPool::fetch_page(page_id_t pid) {
  auto it = page_table_.find(pid);
  if (it != page_table_.end()) {
    int frame = it->second;
    ++meta_[frame].pin_count;
    policy_->set_pinned(pid, true);
    policy_->on_access(pid);
    ++stats_.hits;
    return frame_data(frame);
  }

  ++stats_.misses;
  int frame = find_victim_frame();
  if (frame == -1) return nullptr;  // pool is full and every page is pinned

  disk_->read_page(pid, frame_data(frame));
  meta_[frame] = FrameMeta{pid, 1, false};
  page_table_[pid] = frame;
  policy_->set_pinned(pid, true);
  policy_->on_insert(pid);
  return frame_data(frame);
}

bool BufferPool::unpin_page(page_id_t pid, bool is_dirty) {
  auto it = page_table_.find(pid);
  if (it == page_table_.end()) return false;

  FrameMeta& fm = meta_[it->second];
  if (fm.pin_count <= 0) return false;
  --fm.pin_count;
  if (is_dirty) fm.dirty = true;
  policy_->set_pinned(pid, fm.pin_count > 0);
  return true;
}

void BufferPool::flush_all() {
  for (auto& [pid, frame] : page_table_) {
    if (meta_[frame].dirty) {
      disk_->write_page(pid, frame_data(frame));
      meta_[frame].dirty = false;
    }
  }
}
