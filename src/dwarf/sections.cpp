// SPDX-License-Identifier: MIT
#include "stellar/dwarf/sections.h"

#include <array>
#include <sstream>

#include "stellar/util/bytes.h"

namespace stellar::dwarf {

const char* section_name(Sec s) noexcept {
  switch (s) {
    case Sec::kInfo: return ".debug_info";
    case Sec::kAbbrev: return ".debug_abbrev";
    case Sec::kStr: return ".debug_str";
    case Sec::kLineStr: return ".debug_line_str";
    case Sec::kStrOffsets: return ".debug_str_offsets";
    case Sec::kAddr: return ".debug_addr";
    case Sec::kRanges: return ".debug_ranges";
    case Sec::kRnglists: return ".debug_rnglists";
    case Sec::kLoc: return ".debug_loc";
    case Sec::kLoclists: return ".debug_loclists";
    case Sec::kLine: return ".debug_line";
    case Sec::kTypes: return ".debug_types";
    case Sec::kAranges: return ".debug_aranges";
    case Sec::kFrame: return ".debug_frame";
    case Sec::kMacro: return ".debug_macro";
    case Sec::kPubnames: return ".debug_pubnames";
    case Sec::kEhFrame: return ".eh_frame";
    case Sec::kDataRelRo: return ".data.rel.ro";
    case Sec::kInitArray: return ".init_array";
    case Sec::kCount: break;
  }
  return "?";
}

Sections::Sections(const elf::ElfFile& elf) : elf_(elf) {
  for (std::size_t i = 0; i < static_cast<std::size_t>(Sec::kCount); ++i) {
    const Sec s = static_cast<Sec>(i);
    util::ByteView v = elf.section_data(section_name(s));
    present_[i] = !v.empty();
    views_[i] = v;
    // The address is needed to resolve pcrel encodings in .eh_frame, and to
    // map a symbol value back to a section.
    const elf::Section* sec = elf.find_section(section_name(s));
    addrs_[i] = sec != nullptr ? sec->addr : 0;
  }
  // Record the complete on-disk debug section inventory (including any section
  // this build does not know about) for the capability report.
  for (const elf::Section& s : elf.sections()) {
    if (!s.is_debug() || s.is_nobits()) continue;
    present_sections_.emplace_back(s.name, s.size);
    total_bytes_ += s.size;
  }
}

std::uint64_t Sections::section_addr(Sec s) const {
  const auto i = static_cast<std::size_t>(s);
  if (i >= static_cast<std::size_t>(Sec::kCount)) return 0;
  return addrs_[i];
}

util::ByteView Sections::view(Sec s) const {
  return views_[static_cast<std::size_t>(s)];
}

std::string Sections::capability_report() const {
  std::ostringstream os;
  os << (has_info() ? "DWARF info present" : "no DWARF info (.debug_info/.debug_abbrev missing)");
  if (has(Sec::kStr)) os << ", .debug_str";
  if (has(Sec::kStrOffsets)) os << ", .debug_str_offsets (indexed strings)";
  if (has(Sec::kAddr)) os << ", .debug_addr";
  if (has(Sec::kLine)) os << ", .debug_line";
  if (has(Sec::kLineStr)) os << ", .debug_line_str";
  if (has(Sec::kRanges)) os << ", .debug_ranges";
  if (has(Sec::kRnglists)) os << ", .debug_rnglists";
  if (has(Sec::kLoc)) os << ", .debug_loc";
  if (has(Sec::kLoclists)) os << ", .debug_loclists";
  if (has(Sec::kTypes)) os << ", .debug_types (separate type units)";
  if (has(Sec::kEhFrame)) os << ", .eh_frame (function ranges)";
  if (has(Sec::kDataRelRo)) os << ", .data.rel.ro (vtables + RTTI)";
  if (has(Sec::kAranges)) os << ", .debug_aranges";
  return os.str();
}

}  // namespace stellar::dwarf
