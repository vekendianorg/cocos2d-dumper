// SPDX-License-Identifier: MIT
#include "c2d/elf/mapped_file.h"

#include <cerrno>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace c2d::elf {

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
#if defined(_WIN32)
    : handle_(other.handle_),
      mapping_(other.mapping_),
#else
    : fd_(other.fd_),
#endif
      data_(other.data_),
      size_(other.size_),
      mapped_(other.mapped_),
      path_(std::move(other.path_)) {
#if defined(_WIN32)
  other.handle_ = nullptr;
  other.mapping_ = nullptr;
#else
  other.fd_ = -1;
#endif
  other.data_ = nullptr;
  other.size_ = 0;
  other.mapped_ = false;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    reset();
#if defined(_WIN32)
    handle_ = other.handle_;
    mapping_ = other.mapping_;
#else
    fd_ = other.fd_;
#endif
    data_ = other.data_;
    size_ = other.size_;
    mapped_ = other.mapped_;
    path_ = std::move(other.path_);
#if defined(_WIN32)
    other.handle_ = nullptr;
    other.mapping_ = nullptr;
#else
    other.fd_ = -1;
#endif
    other.data_ = nullptr;
    other.size_ = 0;
    other.mapped_ = false;
  }
  return *this;
}

void MappedFile::reset() noexcept {
#if defined(_WIN32)
  if (data_ != nullptr) ::UnmapViewOfFile(data_);
  if (mapping_ != nullptr) ::CloseHandle(mapping_);
  if (handle_ != nullptr) ::CloseHandle(handle_);
  handle_ = nullptr;
  mapping_ = nullptr;
  data_ = nullptr;
#else
  if (mapped_ && data_ != nullptr && size_ > 0) {
    ::munmap(const_cast<std::uint8_t*>(data_), size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = -1;
  data_ = nullptr;
#endif
  size_ = 0;
  mapped_ = false;
}

#if defined(_WIN32)

bool MappedFile::open(const std::string& path, std::string* error) {
  reset();
  path_ = path;

  // UTF-8 paths are the norm on the command line, so widen explicitly.
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wpath;
  if (wlen > 0) {
    wpath.resize(static_cast<std::size_t>(wlen));
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
  }
  HANDLE h = wpath.empty()
                 ? INVALID_HANDLE_VALUE
                 : ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "CreateFile failed: " + std::to_string(::GetLastError());
    return false;
  }
  LARGE_INTEGER li {};
  if (!::GetFileSizeEx(h, &li)) {
    ::CloseHandle(h);
    if (error) *error = "GetFileSizeEx failed";
    return false;
  }
  size_ = static_cast<std::size_t>(li.QuadPart);
  if (size_ < 64) {
    ::CloseHandle(h);
    if (error) *error = "file too small to be an ELF object";
    return false;
  }
  handle_ = h;
  // SEC_IMAGE is not used: the input may be a raw memory dump whose headers
  // no longer describe the file, and we only need read-only addressability.
  mapping_ = ::CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping_ == nullptr) {
    ::CloseHandle(h);
    handle_ = nullptr;
    if (error) *error = "CreateFileMapping failed";
    return false;
  }
  data_ = static_cast<const std::uint8_t*>(::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
  if (data_ == nullptr) {
    ::CloseHandle(mapping_);
    ::CloseHandle(h);
    mapping_ = nullptr;
    handle_ = nullptr;
    if (error) *error = "MapViewOfFile failed";
    return false;
  }
  mapped_ = true;
  return true;
}

#else  // POSIX

bool MappedFile::open(const std::string& path, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = std::string(what) + ": " + std::strerror(errno);
    reset();
    return false;
  };
  reset();
  path_ = path;

  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) return fail("open");

  struct stat st {};
  if (::fstat(fd_, &st) != 0) return fail("fstat");
  if (!S_ISREG(st.st_mode)) {
    if (error) *error = "not a regular file";
    reset();
    return false;
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ < 64) {
    if (error) *error = "file too small to be an ELF object";
    reset();
    return false;
  }

  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (p == MAP_FAILED) {
    return fail("mmap");
  }
  data_ = static_cast<const std::uint8_t*>(p);
  mapped_ = true;
  return true;
}

#endif

util::ByteView MappedFile::view_at(std::uint64_t offset, std::uint64_t length) const {
  if (offset > size_ || length > size_ - offset) return {};
  return util::ByteView(data_ + offset, static_cast<std::size_t>(length));
}

#if !defined(_WIN32)
void MappedFile::advise_sequential() const {
  if (mapped_ && data_) ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_SEQUENTIAL);
}

void MappedFile::advise_random() const {
  if (mapped_ && data_) ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_RANDOM);
}
#else
// Windows has no madvise; the paging behaviour is managed by the OS and the
// sequential-access hint is only useful on the platforms that expose it.
void MappedFile::advise_sequential() const {}
void MappedFile::advise_random() const {}
#endif

}  // namespace c2d::elf
