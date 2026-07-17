#include "disk_manager.h"

#include <stdexcept>
#include <vector>

DiskManager::DiskManager(const std::string& path, size_t num_pages)
    : path_(path), num_pages_(num_pages) {
  // Create the file if absent, then reopen read/write and size it.
  { std::ofstream create(path_, std::ios::app | std::ios::binary); }
  file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_.is_open()) throw std::runtime_error("cannot open disk file: " + path_);

  // Grow to the full page count so every read_page hits real bytes.
  file_.seekp(0, std::ios::end);
  std::streamoff have = file_.tellp();
  std::streamoff want = static_cast<std::streamoff>(num_pages_ * PAGE_SIZE);
  if (have < want) {
    std::vector<char> zeros(static_cast<size_t>(want - have), 0);
    file_.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    file_.flush();
  }
}

DiskManager::~DiskManager() {
  if (file_.is_open()) file_.close();
}

void DiskManager::read_page(page_id_t pid, char* dest) {
  ++num_reads_;
  if (!io_enabled_) return;
  file_.seekg(static_cast<std::streamoff>(pid) * PAGE_SIZE, std::ios::beg);
  file_.read(dest, PAGE_SIZE);
  if (file_.fail()) file_.clear();  // short read at EOF: leave dest as-is
}

void DiskManager::write_page(page_id_t pid, const char* src) {
  ++num_writes_;
  if (!io_enabled_) return;
  file_.seekp(static_cast<std::streamoff>(pid) * PAGE_SIZE, std::ios::beg);
  file_.write(src, PAGE_SIZE);
  file_.flush();
}
