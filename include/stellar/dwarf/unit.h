// SPDX-License-Identifier: MIT
// Compilation-unit headers and streaming DIE traversal.
//
// Layout rules that this implementation depends on (all verified against the
// target binary, which is 1183 DWARF 4 units):
//   * `unit_length` does NOT include the initial length field itself, so the
//     distance to the next unit is `length_size + unit_length`. Getting this
//     wrong shifts every subsequent unit by 4 bytes and desynchronises the
//     whole section.
//   * 0xffffffff as the initial length marks a 64-bit DWARF format unit.
//   * DWARF 5 reorders the header (version, unit_type, address_size,
//     abbrev_offset) relative to DWARF <= 4 (version, abbrev_offset,
//     address_size), and appends type_signature + type_offset for type units.
//
// Nothing here materialises a DIE tree: DIEs are visited in document order with
// a cursor, which keeps memory flat regardless of unit size.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "stellar/dwarf/abbrev.h"
#include "stellar/dwarf/constants.h"
#include "stellar/dwarf/sections.h"
#include "stellar/util/bytes.h"

namespace stellar::dwarf {

/// A parsed .debug_info unit header.
struct UnitHeader {
  std::uint64_t offset = 0;      ///< byte offset within .debug_info
  std::uint64_t length = 0;      ///< unit_length as stored (excludes length field)
  unsigned length_size = 4;      ///< 4 (DWARF32) or 8 (DWARF64)
  std::uint16_t version = 0;
  std::uint8_t unit_type = ::stellar::dwarf::utype::kUnknown;
  std::uint8_t address_size = 0;
  util::Endian endian = util::Endian::Little;  ///< taken from the ELF container
  std::uint64_t abbrev_offset = 0;
  std::uint64_t type_signature = 0;  ///< DWARF5 type units only
  std::uint64_t type_offset = 0;     ///< DWARF5 type units only
  std::uint64_t die_start = 0;       ///< first byte of the root DIE
  std::uint64_t die_end = 0;         ///< one past the last byte of the unit

  [[nodiscard]] bool is_dwarf64() const { return length_size == 8; }
  [[nodiscard]] unsigned offset_size() const { return length_size; }
  /// True for units whose DIEs describe types (.debug_types / DWARF5 type units).
  [[nodiscard]] bool is_type_unit() const {
    return unit_type == ::stellar::dwarf::utype::kType ||
           unit_type == ::stellar::dwarf::utype::kSplitType;
  }
  [[nodiscard]] std::uint64_t body_size() const { return die_end - die_start; }
};

/// Parses one unit header at `offset`. Returns false with `error` set when the
/// header is truncated or self-inconsistent.
bool parse_unit_header(util::ByteView info, std::uint64_t offset, util::Endian endian,
                       UnitHeader& out, std::string* error);

/// A decoded attribute value.
///
/// Only the numeric interpretation is materialised; string payloads stay as
/// section-relative offsets, because resolving them eagerly would dominate
/// runtime on a 20M-DIE binary.
struct AttrValue {
  std::uint32_t attr = 0;
  std::uint64_t form = 0;
  /// Numeric payload: unsigned forms as-is, signed forms sign-extended into
  /// int64_value, blocks/strings as their offset or length.
  std::uint64_t u64 = 0;
  std::int64_t i64 = 0;
  util::ByteView block;    ///< block/exprloc/string payload, when applicable
  bool has_block = false;

  [[nodiscard]] std::uint64_t as_u64() const { return u64; }
  [[nodiscard]] std::int64_t as_i64() const { return i64; }
  [[nodiscard]] bool as_bool() const { return u64 != 0; }
};

/// A single DIE, positioned at `offset` within its unit.
class Die {
 public:
  Die() = default;

  [[nodiscard]] std::uint64_t offset() const { return offset_; }
  [[nodiscard]] std::uint32_t tag() const { return tag_; }
  [[nodiscard]] bool has_children() const { return has_children_; }
  [[nodiscard]] bool null_entry() const { return null_entry_; }
  [[nodiscard]] const Abbrev* abbrev() const { return abbrev_; }
  /// Nesting depth within the unit (root DIE = 0).
  [[nodiscard]] unsigned depth() const { return depth_; }
  /// Offset one past this DIE's attribute values.
  [[nodiscard]] std::uint64_t next_offset() const { return end_; }
  /// Unit this DIE belongs to.
  [[nodiscard]] const UnitHeader& unit() const { return *unit_; }

  /// Iterates attributes. Uses a callback to avoid materialising a vector per
  /// DIE; the walker's whole purpose is to stay allocation free. The callback
  /// may return void, or bool to stop the iteration early.
  template <typename Fn>
  bool for_each_attr(Fn&& fn) const {
    if (null_entry_ || abbrev_ == nullptr) return true;
    util::Cursor c(data_, size_, unit_->endian);
    if (!c.seek(start_)) return false;
    for (const AbbrevAttr& spec : abbrev_->attrs) {
      AttrValue v;
      if (!read_attr(c, unit_->address_size, unit_->offset_size(), spec, v)) return false;
      if constexpr (std::is_same_v<std::invoke_result_t<Fn, const AttrValue&>, bool>) {
        if (!fn(v)) return true;  // callback asked to stop early
      } else {
        fn(v);
      }
    }
    return true;
  }

  /// Reads one attribute by DWARF attribute code. Linear scan over the
  /// abbreviation; the attribute counts involved are small (<= ~20).
  [[nodiscard]] bool attr(std::uint32_t code, AttrValue& out) const;

  [[nodiscard]] util::Cursor cursor_at_attr_values() const;

 private:
  friend class UnitWalker;
  friend class DieReader;

  static bool read_attr(util::Cursor& c, unsigned address_size, unsigned offset_size,
                        const AbbrevAttr& spec, AttrValue& out);

  const UnitHeader* unit_ = nullptr;
  const Abbrev* abbrev_ = nullptr;
  const std::uint8_t* data_ = nullptr;  ///< .debug_info base
  std::uint64_t start_ = 0;              ///< offset of the first attribute value
  std::size_t size_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t end_ = 0;
  std::uint32_t tag_ = 0;
  unsigned depth_ = 0;
  bool has_children_ = false;
  bool null_entry_ = false;
};

/// Streaming walker over the DIEs of a single unit.
///
/// The DWARF DIE tree is a pre-order stream with explicit null terminators, so a
/// depth counter is all the state that is required: no tree is built and no
/// memory is allocated per DIE. This is what allows a 20-million-DIE binary to
/// be scanned in bounded memory.
class UnitWalker {
 public:
  UnitWalker() = default;
  UnitWalker(util::ByteView info, const UnitHeader& unit, const AbbrevTable* abbrev)
      : info_(info), unit_(&unit), abbrev_(abbrev) {}

  /// Positions at the unit's root DIE.
  void reset();

  /// Advances to the next DIE (skipping null terminators internally, but
  /// tracking depth). Returns false at the end of the unit.
  bool next(Die& out);

  [[nodiscard]] std::uint64_t dies_seen() const { return dies_seen_; }
  [[nodiscard]] unsigned current_depth() const { return depth_; }

 private:
  util::ByteView info_;
  const UnitHeader* unit_ = nullptr;
  const AbbrevTable* abbrev_ = nullptr;
  std::uint64_t pos_ = 0;
  std::uint64_t dies_seen_ = 0;
  unsigned depth_ = 0;
  bool started_ = false;
};

/// Counters filled in by a traversal, for reporting and regression tests.
struct WalkStats {
  std::uint64_t units = 0;
  std::uint64_t dies = 0;
  std::uint64_t bytes_scanned = 0;
  std::uint64_t max_depth = 0;
  std::uint64_t failed_units = 0;
  /// Tag -> count, indexed by raw tag value (sparse; use a map when reporting).
  std::vector<std::uint64_t> tag_counts = std::vector<std::uint64_t>(0x500, 0);
};

}  // namespace stellar::dwarf
