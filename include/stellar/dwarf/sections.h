// SPDX-License-Identifier: MIT
// Discovery of the DWARF section set within an ELF file, plus the capability
// flags derived from it.
//
// Design note: the target binary is DWARF 4 and lacks .debug_str_offsets,
// .debug_addr, .debug_line_str, .debug_rnglists and .debug_loclists. Rather
// than assuming any particular version, the reader detects what is present and
// only enables the resolution paths that the data can actually support.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stellar/elf/elf_file.h"
#include "stellar/util/bytes.h"

namespace stellar::dwarf {

/// Canonical DWARF section names, in the order they are probed.
enum class Sec : std::uint8_t {
  kInfo, kAbbrev, kStr, kLineStr, kStrOffsets, kAddr, kRanges, kRnglists,
  kLoc, kLoclists, kLine, kTypes, kAranges, kFrame, kMacro, kPubnames,
  // Non-DWARF sections the dwarfless mode uses.
  kEhFrame, kDataRelRo, kInitArray,
  kCount
};

[[nodiscard]] const char* section_name(Sec s) noexcept;

class Sections {
 public:
  /// Probes the ELF file for DWARF sections. Always succeeds; use `has_info()`
  /// to test whether debug info is present at all.
  explicit Sections(const elf::ElfFile& elf);

  [[nodiscard]] bool has(Sec s) const { return present_[static_cast<std::size_t>(s)]; }
  [[nodiscard]] util::ByteView view(Sec s) const;
  [[nodiscard]] std::uint64_t size(Sec s) const {
    return view(s).size();
  }
  [[nodiscard]] const elf::ElfFile& elf() const { return elf_; }

  /// True when .debug_info and .debug_abbrev are both present: the minimum
  /// needed to walk DIEs.
  [[nodiscard]] bool has_info() const { return has(Sec::kInfo) && has(Sec::kAbbrev); }
  /// True when a full line-table program is available.
  [[nodiscard]] bool has_line() const { return has(Sec::kLine); }

  /// Virtual address of a section, needed to resolve pcrel encodings in
  /// .eh_frame. Returns 0 when the section is absent.
  [[nodiscard]] std::uint64_t section_addr(Sec s) const;

  /// True when enough survives to produce a useful (if inferred) dump:
  /// function ranges from .eh_frame, or at least an export table.
  [[nodiscard]] bool has_dwarfless_sources() const {
    return has(Sec::kEhFrame) || has(Sec::kDataRelRo) || elf_.has_symtab() ||
           elf_.has_dynsym();
  }

  /// Every .debug_* section actually present, with sizes (for reports).
  [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>& present_debug_sections() const {
    return present_sections_;
  }
  [[nodiscard]] std::uint64_t total_debug_bytes() const { return total_bytes_; }
  [[nodiscard]] std::string capability_report() const;

 private:
  const elf::ElfFile& elf_;
  util::ByteView views_[static_cast<std::size_t>(Sec::kCount)];
  bool present_[static_cast<std::size_t>(Sec::kCount)] = {};
  std::uint64_t addrs_[static_cast<std::size_t>(Sec::kCount)] = {};
  std::vector<std::pair<std::string, std::uint64_t>> present_sections_;
  std::uint64_t total_bytes_ = 0;
};

}  // namespace stellar::dwarf
