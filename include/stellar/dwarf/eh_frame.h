// SPDX-License-Identifier: MIT
// `.eh_frame` FDE table: every function's exact [start, end) range, available
// even when the binary is stripped of DWARF.
//
// This is what makes the dwarfless mode useful. `.eh_frame` is present in
// essentially every C++ shared object because it is needed for unwinding, and
// its FDEs are independent of debug info.
//
// Encoding follows the LSB "Exception Frames" format:
//   record := length:u32 (0 terminates) CIE-pointer/FDE-pc-begin ...
//   CIE    := id:u32 (0) version:u8 augmentation:string
//            code_align:uleb data_align:sleb [ra_reg] [aug-data]
//   FDE    := CIE-pointer:u32 pc_begin:encoded address_range:encoded
//
// `pc_begin` uses the encoding the CIE advertises in its 'R' augmentation byte
// (DW_EH_PE_*); `address_range` is always a plain value.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "stellar/util/bytes.h"

namespace stellar::dwarf {

/// One function's unwind range, as recovered from an FDE.
struct FdeRange {
  std::uint64_t start = 0;
  std::uint64_t size = 0;
};

/// DW_EH_PE_* encoding bits (only the combinations clang emits are supported;
/// anything else makes the scan stop rather than guess).
namespace eh_pe {
inline constexpr std::uint8_t kAbsptr = 0x00;
inline constexpr std::uint8_t kUleb128 = 0x01;
inline constexpr std::uint8_t kUdata2 = 0x02;
inline constexpr std::uint8_t kUdata4 = 0x03;
inline constexpr std::uint8_t kUdata8 = 0x04;
inline constexpr std::uint8_t kSleb128 = 0x09;
inline constexpr std::uint8_t kSdata2 = 0x0a;
inline constexpr std::uint8_t kSdata4 = 0x0b;
inline constexpr std::uint8_t kSdata8 = 0x0c;
inline constexpr std::uint8_t kPcrel = 0x10;
inline constexpr std::uint8_t kTextrel = 0x20;
inline constexpr std::uint8_t kDatarel = 0x40;
inline constexpr std::uint8_t kIndirect = 0x80;
}  // namespace eh_pe

struct EhFrameStats {
  std::uint64_t cies = 0;
  std::uint64_t fdes = 0;
  std::uint64_t skipped_unsupported = 0;
  bool truncated = false;
};

/// Scans `.eh_frame` and returns every FDE range, sorted by start address.
///
/// `section_addr` is the virtual address of the section, needed to turn a
/// pcrel encoding into an absolute address. Parsing stops (and is reported in
/// `stats`) at the first record it cannot interpret, so a malformed section
/// yields a truncated but honest result rather than garbage.
std::vector<FdeRange> parse_eh_frame(util::ByteView section, std::uint64_t section_addr,
                                     EhFrameStats* stats = nullptr);

}  // namespace stellar::dwarf
