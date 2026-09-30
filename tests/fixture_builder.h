// SPDX-License-Identifier: MIT
// Builds synthetic ELF files containing hand-written DWARF, in memory.
//
// This exists so the parser can be tested against constructs the real target
// binary does not contain (DWARF64, DWARF 5 headers, every form, truncated
// input) without depending on a compiler toolchain at test time.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "stellar/dwarf/constants.h"

namespace stellar::test {

/// Minimal LE byte assembler with DWARF's LEB128 encodings.
class Bytes {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void uleb(std::uint64_t v) {
    do {
      std::uint8_t byte = v & 0x7f;
      v >>= 7;
      if (v != 0) byte |= 0x80;
      buf_.push_back(byte);
    } while (v != 0);
  }
  void sleb(std::int64_t v) {
    bool more = true;
    while (more) {
      std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7f);
      v >>= 7;
      const bool sign = (byte & 0x40) != 0;
      if ((v == 0 && !sign) || (v == -1 && sign)) {
        more = false;
      } else {
        byte |= 0x80;
      }
      buf_.push_back(byte);
    }
  }
  void cstr(const std::string& s) {
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
  }
  void raw(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  void patch_u32(std::size_t pos, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_[pos + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  void patch_u64(std::size_t pos, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_[pos + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
  [[nodiscard]] std::size_t size() const { return buf_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return buf_; }
  [[nodiscard]] std::vector<uint8_t>& mutable_bytes() { return buf_; }

 private:
  std::vector<std::uint8_t> buf_;
};

/// One (tag, attributes, children) entry in a synthetic abbreviation table.
struct AbbrevSpec {
  std::uint64_t code;
  std::uint32_t tag;
  bool has_children = false;
  /// (attribute, form, implicit_const) triples. The third element is only
  /// written to the table for DW_FORM_implicit_const; pass 0 otherwise.
  struct AttrSpec {
    std::uint32_t attr;
    std::uint64_t form;
    std::int64_t implicit_const = 0;
  };
  std::vector<AttrSpec> attrs;
};

/// Assembles .debug_abbrev from a list of specs.
std::vector<std::uint8_t> build_abbrev(const std::vector<AbbrevSpec>& specs);

/// Assembles a complete unit (.debug_info contribution) for a set of DIEs.
///
/// `dies` is encoded as (abbrev_code, encoded_attribute_bytes) pairs, in
/// document order; a code of 0 emits a null entry (closing the current level).
struct RawDie {
  std::uint64_t code;
  std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> build_unit_v4(std::uint64_t abbrev_offset, std::uint8_t address_size,
                                        const std::vector<RawDie>& dies);
std::vector<std::uint8_t> build_unit_v5(std::uint8_t unit_type, std::uint8_t address_size,
                                        std::uint64_t abbrev_offset,
                                        const std::vector<RawDie>& dies);
/// 64-bit DWARF format unit (initial length 0xffffffff).
std::vector<std::uint8_t> build_unit_dwarf64(std::uint16_t version, std::uint8_t address_size,
                                             std::uint64_t abbrev_offset,
                                             const std::vector<RawDie>& dies);

/// Assembles a minimal but valid ELF64 shared object carrying the given debug
/// sections. Returns the complete file image.
struct DebugSections {
  std::vector<std::uint8_t> info;
  std::vector<std::uint8_t> abbrev;
  std::vector<std::uint8_t> str;
  /// Optional non-DWARF sections, used by the dwarfless-mode tests.
  std::vector<std::uint8_t> eh_frame;
  std::uint64_t eh_frame_addr = 0;
};

/// One function range to encode into a synthetic .eh_frame.
struct EhFunction {
  std::uint64_t start = 0;
  std::uint64_t size = 0;
};

/// Builds a minimal .eh_frame with a single "zR" CIE (encoding
/// DW_EH_PE_pcrel|sdata4, as clang emits for AArch64) followed by one FDE per
/// function. `section_addr` is the address the section will be mapped at, since
/// the FDE pointers are pcrel.
std::vector<std::uint8_t> build_eh_frame(const std::vector<EhFunction>& fns,
                                         std::uint64_t section_addr);

std::vector<std::uint8_t> build_elf(const DebugSections& debug);

/// Writes `bytes` to `path`; returns false on I/O error.
bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes);

/// Absolute path to `name` inside the platform's temporary directory.
///
/// Tests must not hardcode "/tmp": that directory does not exist on Windows, so
/// every fixture write failed there and the ELF could not be reopened.
std::string temp_path(const char* name);

/// Path of the real target binary, or empty when it is not present.
/// Tests that need it are skipped rather than failed, so the suite stays green
/// on machines without the 583 MB input.
std::string real_binary_path();

}  // namespace stellar::test
