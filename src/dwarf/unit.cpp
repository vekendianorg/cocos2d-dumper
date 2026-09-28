// SPDX-License-Identifier: MIT
#include "c2d/dwarf/unit.h"

#include <cstdio>

#include "c2d/dwarf/constants.h"

namespace c2d::dwarf {
namespace {
/// Small printf-to-std::string helper for diagnostics; formatting args are
/// passed through exactly as given to snprintf.
template <typename... Args>
std::string fmt(const char* f, Args... args) {
  char buf[256];
  const int n = std::snprintf(buf, sizeof(buf), f, args...);
  if (n < 0) return {};
  if (static_cast<std::size_t>(n) < sizeof(buf)) return std::string(buf, static_cast<std::size_t>(n));
  std::string big(static_cast<std::size_t>(n) + 1, '\0');
  std::snprintf(big.data(), big.size(), f, args...);
  big.resize(static_cast<std::size_t>(n));
  return big;
}
}  // namespace

bool parse_unit_header(util::ByteView info, std::uint64_t offset, util::Endian endian,
                       UnitHeader& out, std::string* error) {
  if (offset >= info.size()) {
    if (error) *error = "unit offset past end of .debug_info";
    return false;
  }
  util::Cursor c(info.data() + offset, info.size() - offset, endian);

  std::uint32_t len32 = 0;
  if (!c.read_u32(len32)) {
    if (error) *error = fmt("truncated unit_length at 0x%llx", (unsigned long long)offset);
    return false;
  }

  // Width of the "initial length field": 4 for DWARF32, or 12 for DWARF64
  // (the 0xffffffff marker plus the 8-byte length). unit_length counts the
  // bytes that follow this field, so the unit's total extent is
  // initial_length_size + unit_length.
  unsigned initial_length_size = 4;
  unsigned length_size = 4;  // width of the offset fields (abbrev_offset etc.)
  std::uint64_t length = len32;
  if (len32 == 0xffffffffu) {
    // 64-bit DWARF format: the length is a full 64-bit field.
    if (!c.read_u64(length)) {
      if (error) *error = fmt("truncated 64-bit unit_length at 0x%llx", (unsigned long long)offset);
      return false;
    }
    initial_length_size = 12;
    length_size = 8;
  } else if (len32 >= 0xfffffff0u) {
    if (error) {
      *error = fmt("reserved unit_length 0x%llx at 0x%llx",
                   (unsigned long long)len32, (unsigned long long)offset);
    }
    return false;
  }

  if (length == 0) {
    if (error) *error = fmt("zero-length unit at 0x%llx", (unsigned long long)offset);
    return false;
  }
  // unit_length covers everything after the initial length field, including
  // the rest of the header, so the total extent is initial_length_size+length.
  const std::uint64_t total = static_cast<std::uint64_t>(initial_length_size) + length;
  if (offset + total > info.size()) {
    if (error) {
      *error = fmt("unit at 0x%llx claims %llu bytes, past end of .debug_info (%llu)",
                   (unsigned long long)offset, (unsigned long long)total,
                   (unsigned long long)info.size());
    }
    return false;
  }

  std::uint16_t version = 0;
  if (!c.read_u16(version)) {
    if (error) *error = fmt("truncated version at 0x%llx", (unsigned long long)offset);
    return false;
  }
  if (version < 2 || version > 5) {
    if (error) *error = fmt("unsupported DWARF version %u at 0x%llx", (unsigned)version, (unsigned long long)offset);
    return false;
  }

  out.offset = offset;
  out.length = length;
  out.length_size = length_size;
  out.version = version;
  out.endian = endian;
  out.die_end = offset + total;

  if (version >= 5) {
    // DWARF5: version, unit_type, address_size, abbrev_offset
    std::uint8_t ut = 0, asz = 0;
    if (!c.read_u8(ut) || !c.read_u8(asz)) {
      if (error) *error = fmt("truncated DWARF5 unit header at 0x%llx", (unsigned long long)offset);
      return false;
    }
    out.unit_type = ut;
    out.address_size = asz;
    if (!c.read_uint(length_size, out.abbrev_offset)) {
      if (error) *error = fmt("truncated abbrev_offset at 0x%llx", (unsigned long long)offset);
      return false;
    }
    if (out.unit_type == utype::kType || out.unit_type == utype::kSplitType) {
      if (!c.read_u64(out.type_signature) || !c.read_uint(length_size, out.type_offset)) {
        if (error) *error = fmt("truncated type unit header at 0x%llx", (unsigned long long)offset);
        return false;
      }
    }
  } else {
    // DWARF <= 4: version, abbrev_offset, address_size
    if (!c.read_uint(length_size, out.abbrev_offset) || !c.read_u8(out.address_size)) {
      if (error) *error = fmt("truncated unit header at 0x%llx", (unsigned long long)offset);
      return false;
    }
    out.unit_type = utype::kUnknown;
  }

  if (out.address_size == 0 || out.address_size > 8) {
    if (error) {
      *error = fmt("implausible address_size %u at 0x%llx", (unsigned)out.address_size, (unsigned long long)offset);
    }
    return false;
  }
  out.die_start = c.pos() + offset;
  if (out.die_start >= out.die_end) {
    if (error) *error = fmt("unit at 0x%llx has no DIE data", (unsigned long long)offset);
    return false;
  }
  return true;
}

bool Die::read_attr(util::Cursor& c, unsigned address_size, unsigned offset_size,
                    const AbbrevAttr& spec, AttrValue& out) {
  out.attr = spec.attr;
  out.form = spec.form;
  out.has_block = false;
  out.u64 = 0;
  out.i64 = 0;

  switch (spec.form) {
    case form::kFlagPresent:
      out.u64 = 1;
      out.i64 = 1;
      return true;
    case form::kImplicitConst:
      out.u64 = static_cast<std::uint64_t>(spec.implicit_const);
      out.i64 = spec.implicit_const;
      return true;

    // --- fixed-width unsigned ---
    case form::kAddr:
    case form::kData1: case form::kData2: case form::kData4: case form::kData8:
    case form::kRef1: case form::kRef2: case form::kRef4: case form::kRef8:
    case form::kRefAddr: case form::kSecOffset: case form::kStrp:
    case form::kStrpSup: case form::kLineStrp: case form::kRefSup4:
    case form::kRefSig8: case form::kStrx1: case form::kStrx2:
    case form::kStrx3: case form::kStrx4: case form::kAddrx1:
    case form::kAddrx2: case form::kAddrx3: case form::kAddrx4: {
      unsigned width = 0;
      switch (spec.form) {
        case form::kData1: case form::kRef1: case form::kStrx1: case form::kAddrx1:
          width = 1; break;
        case form::kData2: case form::kRef2: case form::kStrx2: case form::kAddrx2:
          width = 2; break;
        case form::kStrx3: case form::kAddrx3:
          width = 3; break;
        case form::kData4: case form::kRef4: case form::kSecOffset: case form::kStrp:
        case form::kStrpSup: case form::kLineStrp: case form::kRefSup4:
        case form::kStrx4: case form::kAddrx4:
          width = 4; break;
        case form::kData8: case form::kRef8: case form::kRefAddr: case form::kRefSig8:
          width = 8; break;
        case form::kAddr:
          width = address_size; break;
        default: break;
      }
      std::uint64_t v = 0;
      if (!c.read_uint(width, v)) return false;
      out.u64 = v;
      out.i64 = static_cast<std::int64_t>(v);
      return true;
    }

    case form::kData16: {
      // Two 64-bit halves; only the first is surfaced numerically.
      std::uint64_t lo = 0, hi = 0;
      if (!c.read_u64(lo) || !c.read_u64(hi)) return false;
      out.u64 = lo;
      out.i64 = static_cast<std::int64_t>(lo);
      return true;
    }

    case form::kFlag:
    case form::kUdata:
    case form::kRefUdata:
    case form::kStrx:
    case form::kAddrx:
    case form::kGnuAddrIndex:
    case form::kGnuStrIndex:
    case form::kLoclistx:
    case form::kRnglistx: {
      if (spec.form == form::kFlag) {
        std::uint8_t v = 0;
        if (!c.read_u8(v)) return false;
        out.u64 = v;
      } else {
        std::uint64_t v = 0;
        if (!c.read_uleb128(v)) return false;
        out.u64 = v;
      }
      out.i64 = static_cast<std::int64_t>(out.u64);
      return true;
    }

    case form::kSdata: {
      std::int64_t v = 0;
      if (!c.read_sleb128(v)) return false;
      out.i64 = v;
      out.u64 = static_cast<std::uint64_t>(v);
      return true;
    }

    // --- variable-length payloads ---
    case form::kString: {
      // Inline string in the DIE. Expose the characters (without the NUL) as a
      // view; note this is NOT NUL-terminated storage, so callers must respect
      // view.size().
      std::string_view sv = c.read_cstr();
      if (!c.ok()) return false;
      out.has_block = true;
      out.block = util::ByteView(reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size());
      out.u64 = sv.size();
      out.i64 = static_cast<std::int64_t>(sv.size());
      return true;
    }
    case form::kBlock: case form::kBlock1: case form::kBlock2: case form::kBlock4:
    case form::kExprloc: {
      util::ByteView v;
      if (!c.read_block(v)) return false;
      out.has_block = true;
      out.block = v;
      out.u64 = v.size();
      out.i64 = static_cast<std::int64_t>(v.size());
      return true;
    }

    case form::kIndirect: {
      // The DIE stores the real form as a ULEB128; read it and recurse once.
      std::uint64_t actual = 0;
      if (!c.read_uleb128(actual)) return false;
      AbbrevAttr tmp = spec;
      tmp.form = actual;
      // An indirect form may itself be indirect; guard against a cycle.
      if (actual == form::kIndirect) {
        out.form = form::kIndirect;
        out.u64 = 0;
        return true;  // cannot represent; treated as zero-width
      }
      return read_attr(c, address_size, offset_size, tmp, out);
    }

    default:
      // Unknown form: the walker cannot safely continue past it.
      return false;
  }
}

bool Die::attr(std::uint32_t code, AttrValue& out) const {
  if (null_entry_ || abbrev_ == nullptr) return false;
  // Linear scan: abbreviations carry a handful of attributes (max observed in
  // the target's tables is 11), so this is cheaper than any index.
  util::Cursor c(data_, size_, unit_->endian);
  if (!c.seek(start_)) return false;
  for (const AbbrevAttr& spec : abbrev_->attrs) {
    AttrValue v;
    if (!Die::read_attr(c, unit_->address_size, unit_->offset_size(), spec, v)) {
          return false;
        }
    if (spec.attr == code) {
      out = v;
      return true;
    }
  }
  return false;
}

util::Cursor Die::cursor_at_attr_values() const {
  util::Cursor c(data_, size_, unit_->endian);
  (void)c.seek(start_);
  return c;
}

void UnitWalker::reset() {
  pos_ = unit_ ? unit_->die_start : 0;
  dies_seen_ = 0;
  depth_ = 0;
  started_ = false;
}

bool UnitWalker::next(Die& out) {
  if (unit_ == nullptr || abbrev_ == nullptr) return false;
  const std::uint64_t end = unit_->die_end;
  util::Cursor c(info_.data(), info_.size(), unit_->endian);

  if (!started_) {
    started_ = true;
    pos_ = unit_->die_start;
  }

  while (pos_ < end) {
    if (!c.seek(pos_)) return false;
    std::uint64_t code = 0;
    if (!c.read_uleb128(code)) return false;
    const std::uint64_t die_offset = pos_;
    pos_ = c.pos();

    if (code == 0) {
      // Null entry: closes the current sibling list.
      if (depth_ == 0) return false;  // finished the root DIE's children
      --depth_;
      continue;
    }

    if (!abbrev_->valid(code)) {
      // Unknown abbreviation code: the stream is unrecoverable from here.
      return false;
    }
    const Abbrev& ab = abbrev_->get(code);

    out.unit_ = unit_;
    out.abbrev_ = &ab;
    out.data_ = info_.data();
    out.start_ = pos_;
    out.size_ = info_.size();
    out.offset_ = die_offset;
    out.tag_ = ab.tag;
    out.depth_ = depth_;
    out.has_children_ = ab.has_children;
    out.null_entry_ = false;

    // Advance past the attribute values. The fast path uses the precomputed
    // fixed size; only abbreviations containing a variable-length form take the
    // decoding path.
    if (ab.size_known) {
      if (pos_ + static_cast<std::uint64_t>(ab.fixed_size) > end) return false;
      pos_ += static_cast<std::uint64_t>(ab.fixed_size);
      out.end_ = pos_;
    } else {
      if (!c.seek(pos_)) return false;
      for (const AbbrevAttr& spec : ab.attrs) {
        AttrValue v;
        if (!Die::read_attr(c, unit_->address_size, unit_->offset_size(), spec, v)) {
          return false;
        }
      }
      pos_ = c.pos();
      if (pos_ > end) return false;
      out.end_ = pos_;
    }

    ++dies_seen_;
    if (ab.has_children) ++depth_;
    return true;
  }
  return false;
}
}
