#pragma once
#include <cstddef>
#include <fstream>
#include <string>

#include "policy.h"

constexpr size_t PAGE_SIZE = 4096;

// A simulated disk: one real file, addressed as fixed 4 KB pages. Page N lives
// at byte offset N * PAGE_SIZE. Every read/write is counted, because "disk I/Os
// avoided" is the number this whole project exists to measure.
class DiskManager {
 public:
  // Creates/opens `path` and grows it to num_pages * PAGE_SIZE bytes.
  DiskManager(const std::string& path, size_t num_pages);
  ~DiskManager();

  // dest/src must point to at least PAGE_SIZE bytes.
  void read_page(page_id_t pid, char* dest);
  void write_page(page_id_t pid, const char* src);

  // When true, read/write become no-ops that still count. Used by the ops/sec
  // micro-benchmark to isolate policy overhead from file I/O (Phase 4).
  void set_io_enabled(bool enabled) { io_enabled_ = enabled; }

  size_t num_reads() const { return num_reads_; }
  size_t num_writes() const { return num_writes_; }
  size_t num_pages() const { return num_pages_; }

 private:
  std::fstream file_;
  std::string path_;
  size_t num_pages_;
  size_t num_reads_ = 0;
  size_t num_writes_ = 0;
  bool io_enabled_ = true;
};
