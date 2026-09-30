// SPDX-License-Identifier: MIT
// Abbreviation tables (.debug_abbrev).
//
// Each compilation unit points at one table by offset; a large binary typically
// has one table per translation unit (1183 tables in the target). Tables are
// parsed lazily and cached, because most units are processed exactly once, so
// an unbounded cache would be pure overhead. The cache is therefore keyed by
// offset with LRU eviction.
#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "stellar/util/bytes.h"

namespace stellar::dwarf {

/// One attribute specification inside an abbreviation.
struct AbbrevAttr {
  std::uint32_t attr = 0;
  std::uint64_t form = 0;
  std::int64_t implicit_const = 0;  ///< only for DW_FORM_implicit_const
};

/// One abbreviation: a DIE code -> (tag, children, attributes) mapping.
struct Abbrev {
  std::uint64_t code = 0;
  std::uint32_t tag = 0;
  bool has_children = false;
  std::vector<AbbrevAttr> attrs;
  /// Byte size of the DIE's attribute values, or -1 when a form makes the size
  /// depend on the unit (DW_FORM_addr / offset-size forms) or is unknown.
  /// Computed once per unit by finalise().
  std::int32_t fixed_size = -1;
  bool size_known = false;
};

/// A parsed abbreviation table.
class AbbrevTable {
 public:
  /// Parses the table starting at `offset` inside .debug_abbrev. Returns false
  /// on malformed input; `error` receives a description.
  bool parse(util::ByteView abbrev_section, std::uint64_t offset, util::Endian endian,
             std::string* error);

  /// The table ends with a zero code. Everything before that is a valid code.
  [[nodiscard]] bool valid(std::uint64_t code) const {
    return code != 0 && code < decls_.size() && present_[code];
  }
  [[nodiscard]] const Abbrev& get(std::uint64_t code) const { return decls_[code]; }
  [[nodiscard]] std::size_t size() const { return count_; }
  [[nodiscard]] std::uint64_t offset() const { return offset_; }
  /// Byte offset just past this table in .debug_abbrev.
  [[nodiscard]] std::uint64_t end_offset() const { return end_offset_; }
  /// Sets up per-unit fixed sizes once address/offset size are known.
  void finalise(unsigned address_size, unsigned offset_size);

 private:
  std::uint64_t offset_ = 0;
  std::uint64_t end_offset_ = 0;
  std::size_t count_ = 0;
  std::vector<Abbrev> decls_;   ///< indexed by code
  std::vector<bool> present_;
};

/// Bounded LRU cache of parsed tables, keyed by .debug_abbrev offset.
class AbbrevCache {
 public:
  explicit AbbrevCache(std::size_t capacity = 16) : capacity_(capacity) {}

  /// Returns the table at `offset`, parsing it on first use. On parse failure
  /// returns nullptr and sets `error` (only the first failure is reported).
  const AbbrevTable* get(util::ByteView abbrev_section, std::uint64_t offset,
                         util::Endian endian, unsigned address_size,
                         unsigned offset_size, std::string* error);

  void clear() {
    order_.clear();
    index_.clear();
  }
  [[nodiscard]] std::size_t hits() const { return hits_; }
  [[nodiscard]] std::size_t misses() const { return misses_; }
  [[nodiscard]] std::size_t entries() const { return order_.size(); }

 private:
  // Classic LRU: `order_` holds live entries most-recent-first, `index_` maps a
  // .debug_abbrev offset to its position in that list.
  struct Entry {
    AbbrevTable table;
    std::list<std::uint64_t>::iterator pos;
  };

  std::size_t capacity_;
  std::list<Entry> order_;
  std::unordered_map<std::uint64_t, std::list<Entry>::iterator> index_;
  std::size_t hits_ = 0;
  std::size_t misses_ = 0;
};

}  // namespace stellar::dwarf
