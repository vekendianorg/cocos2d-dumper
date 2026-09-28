// SPDX-License-Identifier: MIT
#include "c2d/dwarf/eh_frame.h"

#include <algorithm>

namespace c2d::dwarf {
namespace {

/// What a CIE tells us about the FDEs that reference it.
struct Cie {
  std::uint8_t fde_encoding = eh_pe::kUdata4;
  bool valid = false;
};

/// Decodes a value in the given DW_EH_PE encoding at the cursor position.
/// Returns false for encodings we do not implement, so the scan stops rather
/// than guessing.
bool read_encoded(util::Cursor& c, std::uint8_t enc, std::uint64_t section_addr,
                  std::uint64_t& out) {
  const std::uint8_t format = enc & 0x0f;
  const std::uint64_t here = section_addr + c.pos();
  std::uint64_t raw = 0;

  switch (format) {
    case eh_pe::kAbsptr: {
      std::uint64_t v = 0;
      if (!c.read_uint(8, v)) return false;
      out = v;
      return true;
    }
    case eh_pe::kUleb128: if (!c.read_uleb128(raw)) return false; break;
    case eh_pe::kUdata2: { std::uint16_t v = 0; if (!c.read_u16(v)) return false; raw = v; break; }
    case eh_pe::kUdata4: { std::uint32_t v = 0; if (!c.read_u32(v)) return false; raw = v; break; }
    case eh_pe::kUdata8: { std::uint64_t v = 0; if (!c.read_u64(v)) return false; raw = v; break; }
    case eh_pe::kSleb128: { std::int64_t v = 0; if (!c.read_sleb128(v)) return false; raw = static_cast<std::uint64_t>(v); break; }
    case eh_pe::kSdata2: { std::uint16_t v = 0; if (!c.read_u16(v)) return false; raw = static_cast<std::uint64_t>(static_cast<std::int16_t>(v)); break; }
    case eh_pe::kSdata4: { std::uint32_t v = 0; if (!c.read_u32(v)) return false; raw = static_cast<std::uint64_t>(static_cast<std::int32_t>(v)); break; }
    case eh_pe::kSdata8: { std::uint64_t v = 0; if (!c.read_u64(v)) return false; raw = v; break; }
    default: return false;
  }
  out = raw;
  // pcrel is the modifier clang emits for AArch64.
  if (enc & eh_pe::kPcrel) out += here;
  return true;
}

}  // namespace

std::vector<FdeRange> parse_eh_frame(util::ByteView section, std::uint64_t section_addr,
                                     EhFrameStats* stats) {
  std::vector<FdeRange> out;
  EhFrameStats local;
  if (stats == nullptr) stats = &local;
  if (section.empty()) return out;

  util::Cursor c(section.data(), section.size());
  // CIEs are referenced by their byte offset within the section.
  std::vector<Cie> cies(16);

  while (c.can(4)) {
    const std::size_t record_off = c.pos();
    std::uint32_t length = 0;
    if (!c.read_u32(length)) break;
    if (length == 0) break;  // terminator
    if (length == 0xffffffffu || length < 4) {  // 64-bit escape, or too small
      stats->truncated = true;
      break;
    }
    const std::size_t body = record_off + 4;
    const std::size_t body_end = body + length;
    if (body_end > section.size()) {
      stats->truncated = true;
      break;
    }

    std::uint32_t id = 0;
    c.read_u32(id);

    if (id == 0) {
      // ---------------------------------------------------------------- CIE --
      Cie cie;
      std::uint8_t version = 0;
      if (!c.read_u8(version)) break;
      const std::string_view aug = c.read_cstr();
      std::uint64_t tmp = 0;
      std::int64_t data_align = 0;
      if (!c.read_uleb128(tmp)) break;   // code alignment factor
      if (!c.read_sleb128(data_align)) break;
      if (version == 1) {
        if (!c.skip(1)) break;          // return address register
      } else {
        if (!c.read_uleb128(tmp)) break;
      }
      if (!aug.empty() && aug[0] == 'z') {
        std::uint64_t aug_len = 0;
        if (!c.read_uleb128(aug_len)) break;
        const std::size_t aug_start = c.pos();
        if (aug_start + aug_len > body_end) break;
        // 'R' carries the FDE pointer encoding; for the single-letter
        // augmentations clang emits ('zR', 'zPLR') it sits at the same index.
        for (std::size_t i = 1; i < aug.size(); ++i) {
          if (aug[i] == 'R' && (i - 1) < aug_len) {
            cie.fde_encoding = section.data()[aug_start + (i - 1)];
          }
        }
        (void)c.seek(aug_start + aug_len);
      }
      cie.valid = true;
      if (record_off >= cies.size()) cies.resize(record_off + 16, Cie{});
      cies[record_off] = cie;
      ++stats->cies;
      if (!c.seek(body_end)) break;
      continue;
    }

    // ---------------------------------------------------------------- FDE --
    // The CIE pointer is the byte distance back to the CIE measured from the
    // pointer field itself, which sits at record_off + 4. Verified against
    // `objdump -Wf` on the real binary: the FDE at 0x14 stores 0x18 and refers
    // to the CIE at 0, and 0x14 + 4 - 0x18 == 0. Producers that store an
    // absolute section offset instead are accepted as a fallback.
    const std::uint64_t field_off = record_off + 4;
    const Cie* cie = nullptr;
    if (id <= field_off && cies[field_off - id].valid) {
      cie = &cies[field_off - id];
    } else if (id < cies.size() && cies[id].valid) {
      cie = &cies[id];
    }
    if (cie == nullptr) {
      if (!c.seek(body_end)) break;  // unknown CIE: skip the record
      continue;
    }

    std::uint64_t pc_begin = 0, pc_range = 0;
    if (!read_encoded(c, cie->fde_encoding, section_addr, pc_begin)) {
      ++stats->skipped_unsupported;
      if (!c.seek(body_end)) break;
      continue;
    }
    if (!c.read_uleb128(pc_range)) {
      stats->truncated = true;
      break;
    }
    if (pc_range != 0) {
      out.push_back(FdeRange{pc_begin, pc_range});
      ++stats->fdes;
    }
    if (!c.seek(body_end)) break;
  }

  std::sort(out.begin(), out.end(),
            [](const FdeRange& a, const FdeRange& b) { return a.start < b.start; });
  return out;
}

}  // namespace c2d::dwarf
