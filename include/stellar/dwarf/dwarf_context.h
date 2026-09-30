// SPDX-License-Identifier: MIT
// The entry point for DWARF work: owns the ELF view, discovers sections,
// enumerates units lazily, and hands out abbreviation tables.
//
// Everything downstream (indexing, type reconstruction, output) depends only on
// this interface, which keeps the DWARF container concerns out of the type
// logic.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "stellar/dwarf/abbrev.h"
#include "stellar/dwarf/sections.h"
#include "stellar/dwarf/unit.h"
#include "stellar/elf/elf_file.h"

namespace stellar::dwarf {

/// Bounds and filters for a unit scan. Defaults mean "everything"; the CLI and
/// the test-suite use these to keep iteration bounded.
struct ScanLimits {
  std::uint64_t max_units = 0;    ///< 0 = no limit
  std::uint64_t max_dies = 0;     ///< 0 = no limit, applied across all units
  std::uint64_t first_unit = 0;   ///< index of the first unit to visit
  std::uint64_t unit_stride = 1;  ///< visit every Nth unit (bounded sampling)
};

class DwarfContext {
 public:
  explicit DwarfContext(const elf::ElfFile& elf);

  [[nodiscard]] const Sections& sections() const { return sections_; }
  [[nodiscard]] const elf::ElfFile& elf() const { return elf_; }
  [[nodiscard]] util::ByteView info() const { return sections_.view(Sec::kInfo); }
  [[nodiscard]] util::ByteView abbrev() const { return sections_.view(Sec::kAbbrev); }
  [[nodiscard]] util::ByteView str() const { return sections_.view(Sec::kStr); }
  /// Resolves a .debug_str offset to a NUL-terminated string view. Returns an
  /// empty view for out-of-range offsets.
  [[nodiscard]] std::string_view str_at(std::uint64_t offset) const;

  /// Total units in .debug_info, discovered by a full header pass. This is a
  /// cheap operation (11 bytes read per unit) and is cached.
  [[nodiscard]] std::uint64_t unit_count() const;

  /// Header of unit `index`, without walking DIEs.
  bool unit_header(std::uint64_t index, UnitHeader& out, std::string* error = nullptr) const;

  /// Iteration over unit headers with lazy, sequential access.
  class UnitIterator {
   public:
    explicit UnitIterator(DwarfContext& ctx, const ScanLimits& limits = {});
    /// Advances to the next matching unit. Returns false when exhausted.
    bool next(UnitHeader& out, std::string* error = nullptr);
    [[nodiscard]] std::uint64_t index() const { return index_; }
    [[nodiscard]] std::uint64_t visited() const { return visited_; }
    [[nodiscard]] std::uint64_t skipped_errors() const { return errors_; }

   private:
    DwarfContext* ctx_;
    ScanLimits limits_;
    std::uint64_t index_ = 0;
    std::uint64_t visited_ = 0;
    std::uint64_t errors_ = 0;
    std::uint64_t pos_ = 0;
    bool done_ = false;
  };

  /// Abbreviation table for a unit, from the bounded LRU cache.
  const AbbrevTable* abbrev_table(const UnitHeader& unit, std::string* error = nullptr);

  /// Walks every DIE of `unit`, invoking `fn(Die&)`. Returning false from `fn`
  /// stops the walk. Returns false only on a structural failure.
  template <typename Fn>
  bool walk_unit(const UnitHeader& unit, Fn&& fn, std::uint64_t max_dies = 0);

  /// Resolves a unit-relative reference (DW_FORM_ref4 etc.) to an absolute
  /// .debug_info offset: unit.offset() + value.
  [[nodiscard]] static std::uint64_t resolve_unit_ref(const UnitHeader& unit,
                                                      std::uint64_t value) {
    return unit.offset + value;
  }

  [[nodiscard]] AbbrevCache& abbrev_cache() { return abbrev_cache_; }
  [[nodiscard]] const AbbrevCache& abbrev_cache() const { return abbrev_cache_; }

 private:
  const elf::ElfFile& elf_;
  Sections sections_;
  AbbrevCache abbrev_cache_;
  // Memoised unit start offsets. Mutable because filling the cache is a
  // logically-const operation shared by the const accessors above.
  mutable std::vector<std::uint64_t> unit_offsets_;
  mutable std::uint64_t cached_count_ = 0;
  mutable bool cache_valid_ = false;
};

template <typename Fn>
bool DwarfContext::walk_unit(const UnitHeader& unit, Fn&& fn, std::uint64_t max_dies) {
  const AbbrevTable* ab = abbrev_table(unit, nullptr);
  if (ab == nullptr || ab->size() == 0) return false;
  UnitWalker w(info(), unit, ab);
  w.reset();
  Die die;
  std::uint64_t count = 0;
  while (w.next(die)) {
    if (!fn(die)) return true;
    if (max_dies && ++count >= max_dies) return true;
  }
  return true;
}

}  // namespace stellar::dwarf
