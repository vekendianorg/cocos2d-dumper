// SPDX-License-Identifier: MIT
#include "c2d/dwarf/dwarf_context.h"

#include "c2d/diag/log.h"

namespace c2d::dwarf {

DwarfContext::DwarfContext(const elf::ElfFile& elf)
    : elf_(elf), sections_(elf), abbrev_cache_(16) {}

std::string_view DwarfContext::str_at(std::uint64_t offset) const {
  const util::ByteView s = str();
  if (offset >= s.size()) return {};
  const char* base = reinterpret_cast<const char*>(s.data());
  const std::size_t max = s.size() - offset;
  // .debug_str entries are NUL-terminated; guard against a missing terminator.
  std::size_t len = 0;
  while (len < max && base[offset + len] != '\0') ++len;
  return std::string_view(base + offset, len);
}

std::uint64_t DwarfContext::unit_count() const {
  if (cache_valid_) return cached_count_;
  unit_offsets_.clear();
  const util::ByteView info_v = info();
  std::uint64_t pos = 0;
  while (pos < info_v.size()) {
    UnitHeader h;
    std::string err;
    if (!parse_unit_header(info_v, pos, elf_.endian(), h, &err)) {
      // Stop at the first unparsable unit: everything after it is suspect, and
      // continuing would produce garbage unit boundaries.
      C2D_DEBUG("unit discovery stopped at 0x%llx: %s", static_cast<unsigned long long>(pos),
                err.c_str());
      break;
    }
    unit_offsets_.push_back(pos);
    pos = h.die_end;
  }
  cached_count_ = unit_offsets_.size();
  cache_valid_ = true;
  return cached_count_;
}

bool DwarfContext::unit_header(std::uint64_t index, UnitHeader& out,
                               std::string* error) const {
  (void)unit_count();  // ensure the offset cache is populated
  if (index >= unit_offsets_.size()) {
    if (error) *error = "unit index out of range";
    return false;
  }
  return parse_unit_header(info(), unit_offsets_[index], elf_.endian(), out, error);
}

const AbbrevTable* DwarfContext::abbrev_table(const UnitHeader& unit, std::string* error) {
  return abbrev_cache_.get(abbrev(), unit.abbrev_offset, unit.endian, unit.address_size,
                           unit.offset_size(), error);
}

DwarfContext::UnitIterator::UnitIterator(DwarfContext& ctx, const ScanLimits& limits)
    : ctx_(&ctx), limits_(limits) {
  (void)ctx_->unit_count();  // populate the offset cache
  pos_ = 0;
}

bool DwarfContext::UnitIterator::next(UnitHeader& out, std::string* error) {
  if (done_) return false;
  const std::uint64_t total = ctx_->cached_count_;
  if (limits_.max_units && visited_ >= limits_.max_units) {
    done_ = true;
    return false;
  }
  const std::uint64_t stride = limits_.unit_stride ? limits_.unit_stride : 1;

  while (pos_ < total) {
    const std::uint64_t idx = pos_++;
    if (idx < limits_.first_unit) continue;
    if (stride > 1 && ((idx - limits_.first_unit) % stride) != 0) continue;

    std::string err;
    if (!ctx_->unit_header(idx, out, &err)) {
      ++errors_;
      if (error) *error = err;
      continue;  // skip a bad unit rather than aborting the scan
    }
    index_ = idx;  // index of the unit just returned
    ++visited_;
    return true;
  }
  done_ = true;
  return false;
}

}  // namespace c2d::dwarf
