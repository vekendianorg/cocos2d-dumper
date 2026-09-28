// SPDX-License-Identifier: MIT
#include "c2d/dwarf/abbrev.h"

#include <cstdio>

#include "c2d/dwarf/constants.h"

namespace c2d::dwarf {
namespace {
std::string at(std::uint64_t off) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), " (abbrev offset 0x%llx)",
                static_cast<unsigned long long>(off));
  return buf;
}
}  // namespace

bool AbbrevTable::parse(util::ByteView abbrev, std::uint64_t offset, util::Endian endian,
                        std::string* error) {
  offset_ = offset;
  end_offset_ = offset;
  count_ = 0;
  decls_.clear();
  present_.clear();

  if (offset >= abbrev.size()) {
    if (error) *error = "abbreviation offset past end of .debug_abbrev" + at(offset);
    return false;
  }
  util::Cursor c(abbrev.data() + offset, abbrev.size() - offset, endian);

  // Codes are dense and ascending in practice; grow lazily to tolerate gaps.
  while (true) {
    std::uint64_t code = 0;
    if (!c.read_uleb128(code)) {
      if (error) *error = "truncated abbreviation code" + at(offset);
      return false;
    }
    if (code == 0) break;  // end-of-table marker

    if (code > (1u << 24)) {
      if (error) {
        *error = "implausible abbreviation code " + std::to_string(code) + at(offset);
      }
      return false;
    }
    if (code >= decls_.size()) {
      decls_.resize(static_cast<std::size_t>(code) + 1);
      present_.resize(static_cast<std::size_t>(code) + 1, false);
    }

    Abbrev& ab = decls_[static_cast<std::size_t>(code)];
    ab.code = code;
    std::uint64_t tag_value = 0;
    if (!c.read_uleb128(tag_value)) {
      if (error) *error = "truncated tag for abbreviation code " + std::to_string(code) + at(offset);
      return false;
    }
    ab.tag = static_cast<std::uint32_t>(tag_value);
    std::uint8_t children = 0;
    if (!c.read_u8(children)) {
      if (error) *error = "truncated DW_CHILDREN for abbreviation code " + std::to_string(code) + at(offset);
      return false;
    }
    ab.has_children = children != 0;
    ab.attrs.clear();

    while (true) {
      AbbrevAttr a;
      std::uint64_t attr_v = 0;
      std::uint64_t form_v = 0;
      if (!c.read_uleb128(attr_v) || !c.read_uleb128(form_v)) {
        if (error) *error = "truncated attribute spec for abbreviation code " + std::to_string(code) + at(offset);
        return false;
      }
      a.attr = static_cast<std::uint32_t>(attr_v);
      a.form = form_v;
      if (a.attr == 0 && a.form == 0) break;  // end-of-attribute-list
      // DW_FORM_implicit_const carries its value in the abbreviation, not the
      // DIE, so it must be consumed here and consumes nothing during a walk.
      if (a.form == form::kImplicitConst) {
        if (!c.read_sleb128(a.implicit_const)) {
          if (error) *error = "truncated DW_FORM_implicit_const value" + at(offset);
          return false;
        }
      }
      ab.attrs.push_back(a);
    }
    present_[static_cast<std::size_t>(code)] = true;
    ++count_;
  }

  end_offset_ = offset + c.pos();
  if (end_offset_ > abbrev.size()) {
    if (error) *error = "abbreviation table runs past end of .debug_abbrev" + at(offset);
    return false;
  }
  return true;
}

void AbbrevTable::finalise(unsigned address_size, unsigned offset_size) {
  for (Abbrev& ab : decls_) {
    if (!present_[static_cast<std::size_t>(ab.code)]) continue;
    std::uint64_t total = 0;
    bool fixed = true;
    for (const AbbrevAttr& a : ab.attrs) {
      std::uint64_t sz = 0;
      if (a.form == form::kImplicitConst) {
        sz = 0;
      } else if (!form_size(a.form, address_size, offset_size, sz)) {
        fixed = false;  // unknown or variable-length: must be walked
        break;
      }
      total += sz;
    }
    ab.size_known = fixed;
    ab.fixed_size = fixed ? static_cast<std::int32_t>(total) : -1;
  }
}

const AbbrevTable* AbbrevCache::get(util::ByteView abbrev, std::uint64_t offset,
                                    util::Endian endian, unsigned address_size,
                                    unsigned offset_size, std::string* error) {
  auto it = index_.find(offset);
  if (it != index_.end()) {
    ++hits_;
    order_.splice(order_.begin(), order_, it->second);
    return &it->second->table;
  }
  ++misses_;

  order_.emplace_front();
  Entry& e = order_.front();
  e.table.parse(abbrev, offset, endian, error);
  e.table.finalise(address_size, offset_size);
  // Insert at front; evict the least recently used if over capacity.
  index_[offset] = order_.begin();
  if (order_.size() > capacity_) {
    index_.erase(order_.back().table.offset());
    order_.pop_back();
  }
  return &e.table;
}

}  // namespace c2d::dwarf
