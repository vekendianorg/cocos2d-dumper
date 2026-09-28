// SPDX-License-Identifier: MIT
// Bounds-checked cursor over a byte range plus the LEB128 family.
//
// Every DWARF read goes through this type. Two properties matter:
//   1. No read may ever run off the end of the underlying buffer. Malformed or
//      truncated input is a fact of life with real-world binaries, so a failed
//      read latches an error flag instead of trapping.
//   2. Reads are branch-predictable and allocation free, because the DIE walker
//      executes them hundreds of millions of times on a large binary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace c2d::util {

enum class Endian { Little, Big };

namespace detail {
template <typename T>
constexpr T byteswap(T v) noexcept {
  static_assert(std::is_unsigned_v<T>);
  if constexpr (sizeof(T) == 2) {
    return static_cast<T>((v >> 8) | (v << 8));
  } else if constexpr (sizeof(T) == 4) {
    return static_cast<T>(((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) |
                          ((v << 8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u));
  } else if constexpr (sizeof(T) == 8) {
    return static_cast<T>(((v >> 56) & 0x00000000000000FFull) |
                          ((v >> 40) & 0x000000000000FF00ull) |
                          ((v >> 24) & 0x0000000000FF0000ull) |
                          ((v >> 8) & 0x00000000FF000000ull) |
                          ((v << 8) & 0x000000FF00000000ull) |
                          ((v << 24) & 0x0000FF0000000000ull) |
                          ((v << 40) & 0x00FF000000000000ull) |
                          ((v << 56) & 0xFF00000000000000ull));
  } else {
    static_assert(sizeof(T) == 1, "unsupported width");
    return v;
  }
}

// Byte-swap only when the file's endianness differs from the host's. On the
// common little-endian host reading a little-endian file this compiles to a
// single never-taken branch.
template <typename T>
constexpr T to_host(T v, Endian e) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    if (e == Endian::Big) return byteswap(v);
    return v;
  } else {
    if (e == Endian::Little) return byteswap(v);
    return v;
  }
}
}  // namespace detail

/// A non-owning view of a byte range.
class ByteView {
 public:
  constexpr ByteView() = default;
  ByteView(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  explicit ByteView(std::string_view s)
      : data_(reinterpret_cast<const std::uint8_t*>(s.data())), size_(s.size()) {}

  [[nodiscard]] constexpr const std::uint8_t* data() const { return data_; }
  [[nodiscard]] constexpr std::size_t size() const { return size_; }
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }
  [[nodiscard]] ByteView sub(std::size_t off, std::size_t len) const {
    if (off > size_) return {};
    if (len > size_ - off) len = size_ - off;
    return {data_ + off, len};
  }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

/// Sequential reader with a sticky error flag.
///
/// Once a read fails the cursor latches `ok() == false`; callers check once at
/// the end of a record rather than after every field.
class Cursor {
 public:
  Cursor() = default;
  Cursor(const std::uint8_t* data, std::size_t size, Endian endian = Endian::Little)
      : begin_(data), base_(data), pos_(0), end_(size), endian_(endian) {}
  explicit Cursor(ByteView v, Endian e = Endian::Little)
      : Cursor(v.data(), v.size(), e) {}

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] std::size_t pos() const { return pos_; }
  [[nodiscard]] std::size_t size() const { return end_; }
  [[nodiscard]] std::size_t remaining() const { return ok_ ? end_ - pos_ : 0; }
  [[nodiscard]] Endian endian() const { return endian_; }
  [[nodiscard]] const std::uint8_t* ptr() const { return begin_ + pos_; }
  /// Start of the original (unsubranged) buffer, so offsets can be reported
  /// relative to a section even when a cursor covers only part of it.
  [[nodiscard]] const std::uint8_t* base() const { return base_; }
  void set_base(const std::uint8_t* base) { base_ = base; }

  void fail() { ok_ = false; }

  /// True when `n` more bytes are readable.
  [[nodiscard]] bool can(std::size_t n) const { return ok_ && (end_ - pos_) >= n; }

  bool seek(std::size_t p) {
    if (!ok_ || p > end_) { ok_ = false; return false; }
    pos_ = p;
    return true;
  }
  bool skip(std::size_t n) {
    if (!can(n)) { ok_ = false; return false; }
    pos_ += n;
    return true;
  }

  bool read_u8(std::uint8_t& out) {
    if (!can(1)) return fail_read();
    out = begin_[pos_++];
    return true;
  }
  bool read_u16(std::uint16_t& out) {
    if (!can(2)) return fail_read();
    std::uint16_t v;
    std::memcpy(&v, begin_ + pos_, 2);
    pos_ += 2;
    out = detail::to_host(v, endian_);
    return true;
  }
  bool read_u24(std::uint32_t& out) {
    if (!can(3)) return fail_read();
    const std::uint8_t* p = begin_ + pos_;
    out = (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
          static_cast<std::uint32_t>(p[2]);
    pos_ += 3;
    return true;
  }
  bool read_u32(std::uint32_t& out) {
    if (!can(4)) return fail_read();
    std::uint32_t v;
    std::memcpy(&v, begin_ + pos_, 4);
    pos_ += 4;
    out = detail::to_host(v, endian_);
    return true;
  }
  bool read_u64(std::uint64_t& out) {
    if (!can(8)) return fail_read();
    std::uint64_t v;
    std::memcpy(&v, begin_ + pos_, 8);
    pos_ += 8;
    out = detail::to_host(v, endian_);
    return true;
  }
  /// Read an unsigned integer of `bytes` width (1, 2, 4 or 8).
  bool read_uint(unsigned bytes, std::uint64_t& out) {
    switch (bytes) {
      case 1: { std::uint8_t v; if (!read_u8(v)) return false; out = v; return true; }
      case 2: { std::uint16_t v; if (!read_u16(v)) return false; out = v; return true; }
      case 4: { std::uint32_t v; if (!read_u32(v)) return false; out = v; return true; }
      case 8: { std::uint64_t v; if (!read_u64(v)) return false; out = v; return true; }
      default: return fail_read();
    }
  }


  /// Copy raw bytes out (used for e_ident, which is byte-order independent).
  bool read_bytes(std::uint8_t* out, std::size_t n) {
    if (!can(n)) return fail_read();
    std::memcpy(out, begin_ + pos_, n);
    pos_ += n;
    return true;
  }

  /// LEB128 unsigned. Longer than 10 bytes is treated as malformed.
  bool read_uleb128(std::uint64_t& out) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
      if (!can(1)) return fail_read();
      const std::uint8_t byte = begin_[pos_++];
      if (shift < 64) result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
      shift += 7;
      if ((byte & 0x80u) == 0) break;
      if (shift > 70) { ok_ = false; return false; }
    }
    out = result;
    return true;
  }

  /// LEB128 signed.
  bool read_sleb128(std::int64_t& out) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    while (true) {
      if (!can(1)) return fail_read();
      byte = begin_[pos_++];
      if (shift < 64) result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
      shift += 7;
      if ((byte & 0x80u) == 0) break;
      if (shift > 70) { ok_ = false; return false; }
    }
    if (shift < 64 && (byte & 0x40u) != 0) result |= ~std::uint64_t{0} << shift;
    out = static_cast<std::int64_t>(result);
    return true;
  }

  /// Advance past a LEB128 without decoding it.
  bool skip_uleb128() {
    while (true) {
      if (!can(1)) return fail_read();
      if ((begin_[pos_++] & 0x80u) == 0) return true;
    }
  }

  /// NUL-terminated string without the terminator.
  [[nodiscard]] std::string_view read_cstr() {
    const std::uint8_t* start = begin_ + pos_;
    const std::uint8_t* stop = begin_ + end_;
    const std::uint8_t* p = start;
    while (p < stop && *p != 0) ++p;
    if (p == stop) {  // no terminator within bounds
      ok_ = false;
      return {};
    }
    pos_ = static_cast<std::size_t>(p - begin_) + 1;
    return std::string_view(reinterpret_cast<const char*>(start),
                            static_cast<std::size_t>(p - start));
  }

  /// Read a length-prefixed block as a sub-view (DW_FORM_exprloc, block*).
  bool read_block(ByteView& out) {
    std::uint64_t len = 0;
    if (!read_uleb128(len)) return false;
    if (len > remaining()) return fail_read();
    out = ByteView(begin_ + pos_, static_cast<std::size_t>(len));
    pos_ += static_cast<std::size_t>(len);
    return true;
  }

  [[nodiscard]] ByteView whole() const { return ByteView(begin_, end_); }

 private:
  bool fail_read() {
    ok_ = false;
    return false;
  }

  const std::uint8_t* begin_ = nullptr;
  const std::uint8_t* base_ = nullptr;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  Endian endian_ = Endian::Little;
  bool ok_ = true;
};

/// Decode a LEB128 from a raw pointer; returns bytes consumed or 0 if invalid.
[[nodiscard]] inline std::size_t decode_uleb128(const std::uint8_t* p,
                                               const std::uint8_t* end,
                                               std::uint64_t& out,
                                               std::size_t max_bytes = 10) noexcept {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (std::size_t i = 0; i < max_bytes; ++i) {
    if (p >= end) return 0;
    const std::uint8_t byte = *p++;
    if (shift < 64) result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
    shift += 7;
    if ((byte & 0x80u) == 0) {
      out = result;
      return i + 1;
    }
  }
  return 0;
}

[[nodiscard]] inline std::size_t decode_sleb128(const std::uint8_t* p,
                                               const std::uint8_t* end,
                                               std::int64_t& out,
                                               std::size_t max_bytes = 10) noexcept {
  std::uint64_t result = 0;
  unsigned shift = 0;
  std::uint8_t byte = 0;
  for (std::size_t i = 0; i < max_bytes; ++i) {
    if (p >= end) return 0;
    byte = *p++;
    if (shift < 64) result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
    shift += 7;
    if ((byte & 0x80u) == 0) {
      if (shift < 64 && (byte & 0x40u) != 0) result |= ~std::uint64_t{0} << shift;
      out = static_cast<std::int64_t>(result);
      return i + 1;
    }
  }
  return 0;
}

/// Human-readable size (e.g. "194.4 MB") for the reporting layer.
[[nodiscard]] std::string human_size(std::uint64_t bytes);

}  // namespace c2d::util

