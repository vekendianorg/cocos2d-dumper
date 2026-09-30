// SPDX-License-Identifier: MIT
#include "fixture_builder.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>  // MSVC provides stat here, but not the S_ISREG macro.

#include "c2d/elf/elf_types.h"

namespace c2d::test {
namespace {

/// S_ISREG is a POSIX macro with no MSVC equivalent, so spell the test out for
/// the two platforms rather than assuming one spelling of <sys/stat.h>.
bool is_regular_file(const char* path) {
  struct stat st {};
  if (::stat(path, &st) != 0) return false;
#if defined(_WIN32)
  return (st.st_mode & _S_IFMT) == _S_IFREG;
#else
  return S_ISREG(st.st_mode) != 0;
#endif
}

}  // namespace

std::vector<std::uint8_t> build_abbrev(const std::vector<AbbrevSpec>& specs) {
  Bytes out;
  for (const AbbrevSpec& s : specs) {
    out.uleb(s.code);
    out.uleb(s.tag);
    out.u8(s.has_children ? 1 : 0);
    for (const auto& a : s.attrs) {
      out.uleb(a.attr);
      out.uleb(a.form);
      if (a.form == dwarf::form::kImplicitConst) out.sleb(a.implicit_const);
    }
    out.uleb(0);  // end of attribute list
    out.uleb(0);
  }
  out.uleb(0);  // end of table
  return out.bytes();
}

namespace {

/// Shared tail: append the DIE stream to a header that has been written with a
/// placeholder length, then patch the length in.
std::vector<std::uint8_t> finish_unit(Bytes& out, std::size_t value_pos, unsigned initial_len,
                                      const std::vector<RawDie>& dies) {
  for (const RawDie& d : dies) {
    out.uleb(d.code);
    if (d.code != 0) out.raw(d.payload.data(), d.payload.size());
  }
  // unit_length counts everything after the initial length field.
  const std::uint64_t body = out.size() - initial_len;
  if (initial_len == 4) {
    out.patch_u32(value_pos, static_cast<std::uint32_t>(body));
  } else {
    out.patch_u64(value_pos, body);  // 8-byte length after a 4-byte marker
  }
  return out.bytes();
}

}  // namespace

std::vector<std::uint8_t> build_unit_v4(std::uint64_t abbrev_offset, std::uint8_t address_size,
                                        const std::vector<RawDie>& dies) {
  Bytes out;
  const std::size_t len_pos = 0;
  out.u32(0);  // placeholder unit_length
  out.u16(4);           // version
  out.u32(static_cast<std::uint32_t>(abbrev_offset));
  out.u8(address_size);
  return finish_unit(out, len_pos, 4, dies);
}

std::vector<std::uint8_t> build_unit_v5(std::uint8_t unit_type, std::uint8_t address_size,
                                        std::uint64_t abbrev_offset,
                                        const std::vector<RawDie>& dies) {
  Bytes out;
  const std::size_t len_pos = 0;
  out.u32(0);  // placeholder
  out.u16(5);  // version
  out.u8(unit_type);
  out.u8(address_size);
  out.u32(static_cast<std::uint32_t>(abbrev_offset));
  return finish_unit(out, len_pos, 4, dies);
}

std::vector<std::uint8_t> build_unit_dwarf64(std::uint16_t version, std::uint8_t address_size,
                                             std::uint64_t abbrev_offset,
                                             const std::vector<RawDie>& dies) {
  Bytes out;
  const std::size_t len_pos = 4;  // 8-byte length follows the 4-byte marker
  out.u32(0xffffffffu);          // 64-bit DWARF format marker
  out.u64(0);                    // placeholder unit_length
  out.u16(version);
  out.u64(abbrev_offset);
  out.u8(address_size);
  return finish_unit(out, len_pos, 12, dies);  // 4 marker + 8 length
}

std::vector<std::uint8_t> build_elf(const DebugSections& debug) {
  // Section order: 0 null, 1 .shstrtab, 2 .debug_info, 3 .debug_abbrev,
  // 4 .debug_str, 5 .eh_frame (only when non-empty).
  std::vector<std::string> names = {"",       ".shstrtab",     ".debug_info",
                                    ".debug_abbrev", ".debug_str"};
  std::vector<std::uint32_t> types = {elf::kShtNull, elf::kShtStrtab, elf::kShtProgbits,
                                      elf::kShtProgbits, elf::kShtProgbits};
  std::vector<const std::vector<std::uint8_t>*> datas = {nullptr, nullptr, &debug.info,
                                                        &debug.abbrev, &debug.str};
  std::vector<std::uint64_t> addrs = {0, 0, 0, 0, 0};
  if (!debug.eh_frame.empty()) {
    names.emplace_back(".eh_frame");
    types.push_back(elf::kShtProgbits);
    datas.push_back(&debug.eh_frame);
    addrs.push_back(debug.eh_frame_addr);
  }
  const std::size_t nsec = names.size();

  Bytes shstr;
  std::vector<std::uint32_t> name_off(nsec, 0);
  for (std::size_t i = 0; i < nsec; ++i) {
    name_off[i] = static_cast<std::uint32_t>(shstr.size());
    shstr.cstr(names[i]);
  }

  std::vector<std::size_t> content_off(nsec, 0);
  std::vector<std::vector<std::uint8_t>> contents(nsec);
  std::size_t cursor = sizeof(elf::Ehdr64);
  for (std::size_t i = 1; i < nsec; ++i) {
    contents[i] = (datas[i] == nullptr) ? shstr.bytes() : *datas[i];
    content_off[i] = cursor;
    cursor += contents[i].size();
    cursor = (cursor + 7) & ~static_cast<std::size_t>(7);
  }
  const std::size_t shoff = cursor;

  Bytes file;
  file.raw(elf::kElfMagic, 4);
  file.u8(2);  // ELFCLASS64
  file.u8(1);  // ELFDATA2LSB
  file.u8(1);  // EV_CURRENT
  file.u8(0);  // ELFOSABI_NONE
  for (int i = 0; i < 8; ++i) file.u8(0);
  file.u16(elf::kEtDyn);
  file.u16(elf::kEmAarch64);
  file.u32(1);
  file.u64(0);   // e_entry
  file.u64(0);   // e_phoff
  file.u64(shoff);
  file.u32(0);   // e_flags
  file.u16(static_cast<std::uint16_t>(sizeof(elf::Ehdr64)));
  file.u16(0);   // e_phentsize
  file.u16(0);   // e_phnum
  file.u16(static_cast<std::uint16_t>(sizeof(elf::Shdr64)));
  file.u16(static_cast<std::uint16_t>(nsec));
  file.u16(1);   // e_shstrndx

  for (std::size_t i = 1; i < nsec; ++i) {
    while (file.size() < content_off[i]) file.u8(0);
    file.raw(contents[i].data(), contents[i].size());
  }
  while (file.size() < shoff) file.u8(0);

  for (std::size_t i = 0; i < nsec; ++i) {
    file.u32(name_off[i]);
    file.u32(types[i]);
    file.u64(0);  // sh_flags
    file.u64(addrs[i]);
    file.u64(i == 0 ? 0 : content_off[i]);
    file.u64(i == 0 ? 0 : contents[i].size());
    file.u32(0);  // sh_link
    file.u32(0);  // sh_info
    file.u64(8);  // sh_addralign
    file.u64(0);  // sh_entsize
  }
  return file.bytes();
}

std::vector<std::uint8_t> build_eh_frame(const std::vector<EhFunction>& fns,
                                         std::uint64_t section_addr) {
  // A single "zR" CIE, as clang emits for AArch64: code_align 1, data_align -4,
  // return-address register 30, and an FDE pointer encoding of
  // DW_EH_PE_pcrel|DW_EH_PE_sdata4 (0x1b). The trailing bytes stand in for the
  // CIE's CFA program, which this builder does not care about but which keeps
  // the record the same length a real producer emits.
  Bytes cie;
  cie.u32(0);      // placeholder length
  cie.u32(0);      // CIE id (0 marks a CIE in .eh_frame)
  cie.u8(1);       // version
  cie.cstr("zR");  // augmentation
  cie.uleb(1);     // code alignment factor
  cie.sleb(-4);    // data alignment factor
  cie.u8(30);      // return address register (AArch64 x30)
  cie.uleb(1);     // augmentation data length
  cie.u8(0x1b);    // DW_EH_PE_pcrel | DW_EH_PE_sdata4
  cie.u8(0x0c);    // DW_CFA_def_cfa
  cie.u8(0x1f);
  cie.u8(0x00);
  cie.patch_u32(0, static_cast<std::uint32_t>(cie.size() - 4));

  Bytes out;
  out.raw(cie.bytes().data(), cie.bytes().size());

  for (const EhFunction& f : fns) {
    const std::uint64_t record_off = out.size();
    Bytes fde;
    fde.u32(0);  // placeholder length
    // The CIE pointer is the distance from this field back to the CIE, so it
    // equals the offset of the field itself (record_off + 4) minus the CIE's.
    fde.u32(static_cast<std::uint32_t>(record_off + 4));
    // pc_begin is pcrel relative to the address of the value being read, which
    // sits just after length and CIE pointer.
    const std::uint64_t pc_field_addr = section_addr + record_off + 8;
    fde.u32(static_cast<std::uint32_t>(static_cast<std::int64_t>(f.start) -
                                       static_cast<std::int64_t>(pc_field_addr)));
    fde.uleb(f.size);
    fde.patch_u32(0, static_cast<std::uint32_t>(fde.size() - 4));
    out.raw(fde.bytes().data(), fde.bytes().size());
  }
  return out.bytes();
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = bytes.empty() ||
                  std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  return ok;
}

/// Location of the large real-world binary, for the opt-in real-binary tests.
///
/// There is deliberately no default: a path baked into the repository would
/// only ever be valid on one machine, and would silently skip everywhere else
/// for the wrong reason. Set C2D_REAL_BINARY to a file (or a directory holding
/// `libcocos2dcpp_1.74.2.so`) to enable these tests; they are skipped otherwise.
std::string real_binary_path() {
  const char* env = std::getenv("C2D_REAL_BINARY");
  if (env == nullptr || env[0] == '\0') return {};
  if (!is_regular_file(env)) {
    // Allow pointing at a directory that contains the sample.
    const std::string dir = std::string(env) + "/libcocos2dcpp_1.74.2.so";
    if (is_regular_file(dir.c_str())) return dir;
    return {};
  }
  return env;
}

}  // namespace c2d::test
