// SPDX-License-Identifier: MIT
// Read-only memory-mapped file access.
//
// The real target is ~583 MB, so the file is never read into a heap buffer.
// mmap gives us zero-copy access for the multi-hundred-MB .debug_* sections and
// lets the kernel manage the page cache, which matters when only ~1 GB of RAM
// is available. Verified on the FUSE-backed Android storage used for the
// target: ~1 GB/s sequential access.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "c2d/util/bytes.h"

namespace c2d::elf {

class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  /// Opens `path` read-only. Returns false and fills `error` on failure.
  bool open(const std::string& path, std::string* error);

  [[nodiscard]] const std::uint8_t* data() const { return data_; }
  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool is_mapped() const { return mapped_; }
#if !defined(_WIN32)
  /// Raw descriptor, for callers that want to use it directly.
  [[nodiscard]] int fd() const { return fd_; }
#endif
  [[nodiscard]] const std::string& path() const { return path_; }

  [[nodiscard]] util::ByteView view() const { return util::ByteView(data_, size_); }
  /// Bounds-checked sub-view; returns an empty view if out of range.
  [[nodiscard]] util::ByteView view_at(std::uint64_t offset, std::uint64_t length) const;

  /// Advise the kernel about the intended access pattern. Best effort.
  void advise_sequential() const;
  void advise_random() const;

 private:
  void reset() noexcept;

#if defined(_WIN32)
  // The Windows implementation maps through the Win32 file-mapping API.
  void* handle_ = nullptr;  ///< HANDLE from CreateFileW
  void* mapping_ = nullptr; ///< HANDLE from CreateFileMappingW
#else
  int fd_ = -1;
#endif
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;
  std::string path_;
};

}  // namespace c2d::elf
