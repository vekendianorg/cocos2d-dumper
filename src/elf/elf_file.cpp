// SPDX-License-Identifier: MIT
#include "c2d/elf/elf_file.h"

#include <cstring>

#include "c2d/diag/log.h"

namespace c2d::elf {
namespace {

/// Read a NUL-terminated string out of a bounds-checked byte view.
std::string cstr_at(const util::ByteView& tab, std::uint32_t off) {
  if (off >= tab.size()) return {};
  const char* base = reinterpret_cast<const char*>(tab.data());
  const std::size_t max = tab.size() - off;
  const std::size_t len = ::strnlen(base + off, max);
  return std::string(base + off, len);
}

}  // namespace

const char* machine_name(std::uint16_t machine) {
  switch (machine) {
    case kEm386: return "i386";
    case kEmArm: return "ARM";
    case kEmAarch64: return "AArch64";
    case kEmX86_64: return "x86-64";
    case kEmMips: return "MIPS";
    case kEm860: return "i860";
    case kEmNone: return "none";
    default: return "unknown";
  }
}

const char* object_type_name(std::uint16_t type) {
  switch (type) {
    case kEtRel: return "REL";
    case kEtExec: return "EXEC";
    case kEtDyn: return "DYN (shared object)";
    case kEtCore: return "CORE";
    case kEtNone: return "NONE";
    default: return "unknown";
  }
}

const char* section_type_name(std::uint32_t type) {
  switch (type) {
    case kShtNull: return "NULL";
    case kShtProgbits: return "PROGBITS";
    case kShtSymtab: return "SYMTAB";
    case kShtStrtab: return "STRTAB";
    case kShtRela: return "RELA";
    case kShtHash: return "HASH";
    case kShtDynamic: return "DYNAMIC";
    case kShtNote: return "NOTE";
    case kShtNobits: return "NOBITS";
    case kShtRel: return "REL";
    case kShtDynsym: return "DYNSYM";
    case kShtInitArray: return "INIT_ARRAY";
    case kShtFiniArray: return "FINI_ARRAY";
    default: return "other";
  }
}

std::string ElfFile::string_at(std::uint32_t strtab_index, std::uint32_t name_offset) const {
  if (strtab_index >= sections_.size()) return {};
  return cstr_at(section_data(strtab_index), name_offset);
}

bool ElfFile::open(const std::string& path, std::string* error) {
  if (!file_.open(path, error)) return false;

  if (file_.size() < sizeof(Ehdr64)) {
    if (error) *error = "file smaller than an ELF64 header";
    return false;
  }

  // e_ident is byte-order independent; the rest of the header depends on
  // EI_DATA, so it is decoded in two steps.
  std::memcpy(ehdr_.e_ident, file_.data(), 16);

  if (std::memcmp(ehdr_.e_ident, kElfMagic, 4) != 0) {
    if (error) *error = "not an ELF file (bad magic)";
    return false;
  }
  const std::uint8_t klass = ehdr_.e_ident[kEiClass];
  if (klass != 2) {
    if (error) {
      *error = klass == 1 ? "ELF32 is not supported (only ELF64)"
                          : "invalid EI_CLASS in ELF header";
    }
    return false;
  }
  is_64bit_ = true;

  const std::uint8_t data = ehdr_.e_ident[kEiData];
  if (data != 1 && data != 2) {
    if (error) *error = "invalid EI_DATA in ELF header";
    return false;
  }
  little_endian_ = (data == 1);

  util::Cursor c(file_.data() + 16, sizeof(Ehdr64) - 16, endian());
  c.read_u16(ehdr_.e_type);
  c.read_u16(ehdr_.e_machine);
  c.read_u32(ehdr_.e_version);
  c.read_u64(ehdr_.e_entry);
  c.read_u64(ehdr_.e_phoff);
  c.read_u64(ehdr_.e_shoff);
  c.read_u32(ehdr_.e_flags);
  c.read_u16(ehdr_.e_ehsize);
  c.read_u16(ehdr_.e_phentsize);
  c.read_u16(ehdr_.e_phnum);
  c.read_u16(ehdr_.e_shentsize);
  c.read_u16(ehdr_.e_shnum);
  c.read_u16(ehdr_.e_shstrndx);
  if (!c.ok()) {
    if (error) *error = "truncated ELF header";
    return false;
  }

  if (!parse_headers(error)) return false;
  if (!parse_sections(error)) return false;

  // A memory dump has a section table full of dead pointers: the headers parse
  // but describe nothing real. Detect that by sanity-checking the section
  // headers against the file, and fall back to the program headers.
  if (needs_phdr_recovery()) {
    const std::size_t added = recover_sections_from_phdrs();
    C2D_INFO("section table unusable; recovered %zu section(s) from program headers",
             added);
  }
  if (!parse_symbols(error)) return false;
  return true;
}

bool ElfFile::parse_headers(std::string* error) {
  if (ehdr_.e_phnum == 0) return true;
  if (ehdr_.e_phentsize != sizeof(Phdr64)) {
    if (error) *error = "unexpected e_phentsize for ELF64";
    return false;
  }
  phdrs_.reserve(ehdr_.e_phnum);
  for (std::uint16_t i = 0; i < ehdr_.e_phnum; ++i) {
    const std::uint64_t off =
        ehdr_.e_phoff + static_cast<std::uint64_t>(i) * sizeof(Phdr64);
    const util::ByteView v = file_.view_at(off, sizeof(Phdr64));
    if (v.size() != sizeof(Phdr64)) break;  // truncated table; tolerate
    util::Cursor c(v, endian());
    Phdr64 p{};
    c.read_u32(p.p_type);
    c.read_u32(p.p_flags);
    c.read_u64(p.p_offset);
    c.read_u64(p.p_vaddr);
    c.read_u64(p.p_paddr);
    c.read_u64(p.p_filesz);
    c.read_u64(p.p_memsz);
    c.read_u64(p.p_align);
    if (!c.ok()) break;
    phdrs_.push_back(p);
  }
  return true;
}

bool ElfFile::parse_sections(std::string* error) {
  if (ehdr_.e_shoff == 0 || ehdr_.e_shnum == 0) {
    // No section table: a fully stripped object. DWARF cannot be located.
    return true;
  }
  if (ehdr_.e_shentsize != sizeof(Shdr64)) {
    if (error) *error = "unexpected e_shentsize for ELF64";
    return false;
  }

  // Read raw headers first: names need the section-name string table, which is
  // itself one of these sections.
  std::vector<Shdr64> raw;
  raw.reserve(ehdr_.e_shnum);
  for (std::uint16_t i = 0; i < ehdr_.e_shnum; ++i) {
    const std::uint64_t off =
        ehdr_.e_shoff + static_cast<std::uint64_t>(i) * sizeof(Shdr64);
    const util::ByteView v = file_.view_at(off, sizeof(Shdr64));
    if (v.size() != sizeof(Shdr64)) break;  // truncated
    util::Cursor c(v, endian());
    Shdr64 s{};
    c.read_u32(s.sh_name);
    c.read_u32(s.sh_type);
    c.read_u64(s.sh_flags);
    c.read_u64(s.sh_addr);
    c.read_u64(s.sh_offset);
    c.read_u64(s.sh_size);
    c.read_u32(s.sh_link);
    c.read_u32(s.sh_info);
    c.read_u64(s.sh_addralign);
    c.read_u64(s.sh_entsize);
    if (!c.ok()) break;
    raw.push_back(s);
  }
  if (raw.size() != ehdr_.e_shnum) {
    if (error) *error = "section header table extends past end of file";
    return false;
  }

  // Resolve the section-name string table straight from the raw headers:
  // section_data() reads `sections_`, which is still being built here.
  util::ByteView shstr;
  if (ehdr_.e_shstrndx < raw.size()) {
    const Shdr64& s = raw[ehdr_.e_shstrndx];
    if (s.sh_type != kShtNobits) shstr = file_.view_at(s.sh_offset, s.sh_size);
  }

  sections_.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const Shdr64& s = raw[i];
    Section out;
    out.index = static_cast<std::uint32_t>(i);
    out.name = shstr.empty() ? std::string() : cstr_at(shstr, s.sh_name);
    out.type = s.sh_type;
    out.flags = s.sh_flags;
    out.addr = s.sh_addr;
    out.offset = s.sh_offset;
    out.size = s.sh_size;
    out.link = s.sh_link;
    out.info = s.sh_info;
    out.addralign = s.sh_addralign;
    out.entsize = s.sh_entsize;
    sections_.push_back(std::move(out));
  }
  return true;
}

util::ByteView ElfFile::section_data(std::uint32_t index) const {
  if (index >= sections_.size()) return {};
  const Section& s = sections_[index];
  if (s.is_nobits()) return {};
  return file_.view_at(s.offset, s.size);
}

util::ByteView ElfFile::section_data(std::string_view name) const {
  const Section* s = find_section(name);
  return s ? section_data(s->index) : util::ByteView{};
}

const Section* ElfFile::find_section(std::string_view name) const {
  for (const Section& s : sections_) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

std::uint64_t ElfFile::debug_bytes() const {
  std::uint64_t total = 0;
  for (const Section& s : sections_) {
    if (s.is_debug() && !s.is_nobits()) total += s.size;
  }
  return total;
}

std::vector<std::string> ElfFile::debug_section_names() const {
  std::vector<std::string> names;
  for (const Section& s : sections_) {
    if (s.is_debug()) names.push_back(s.name);
  }
  return names;
}

bool ElfFile::needs_phdr_recovery() const {
  // No section table at all.
  if (sections_.empty()) return true;
  // Any section claiming to extend past the end of the file is a stale pointer
  // from the loader, not a real header.
  for (const Section& s : sections_) {
    if (s.is_nobits()) continue;
    if (s.offset > file_.size() || s.size > file_.size()) return true;
    if (s.offset + s.size > file_.size()) return true;
  }
  // A .dynamic section we can actually read means the table is likely sound.
  return false;
}

std::string ElfFile::describe() const {
  std::string s = is_64bit_ ? "ELF64 " : "ELF32 ";
  s += object_type_name(ehdr_.e_type);
  s += " ";
  s += machine_name(ehdr_.e_machine);
  s += little_endian_ ? " little-endian" : " big-endian";
  return s;
}

bool ElfFile::parse_symbols(std::string* error) {
  (void)error;
  // Prefer .symtab (the full symbol table, present in the unstripped target);
  // fall back to .dynsym for stripped shared objects.
  std::uint32_t which = 0;
  for (const Section& s : sections_) {
    if (s.type == kShtSymtab) { symtab_shndx_ = s.index; break; }
  }
  for (const Section& s : sections_) {
    if (s.type == kShtDynsym) { dynsym_shndx_ = s.index; break; }
  }
  has_symtab_ = symtab_shndx_ != 0;
  has_dynsym_ = dynsym_shndx_ != 0;
  // Prefer the full symbol table; fall back to .dynsym for stripped objects.
  which = has_symtab_ ? symtab_shndx_ : dynsym_shndx_;
  if (which == 0) return true;

  const Section& symsec = sections_[which];
  const util::ByteView strtab = section_data(symsec.link);
  const util::ByteView symdata = section_data(which);
  if (symdata.empty()) return true;

  const std::uint64_t entsize = symsec.entsize ? symsec.entsize : sizeof(Sym64);
  if (entsize < sizeof(Sym64)) return true;  // implausible; skip
  const std::uint64_t count = symdata.size() / entsize;
  // Reserve, but cap: a corrupt entsize must not trigger a huge allocation.
  symbols_.reserve(count < (4u << 20) ? count : (4u << 20));

  const util::Endian e = endian();
  std::uint32_t index = 0;
  for (std::uint64_t off = 0; off + sizeof(Sym64) <= symdata.size(); off += entsize, ++index) {
    util::Cursor c(symdata.data() + off, sizeof(Sym64), e);
    std::uint32_t st_name = 0;
    std::uint8_t st_info = 0;
    std::uint8_t st_other = 0;
    std::uint16_t st_shndx = 0;
    std::uint64_t st_value = 0;
    std::uint64_t st_size = 0;
    c.read_u32(st_name);
    c.read_u8(st_info);
    c.read_u8(st_other);
    c.read_u16(st_shndx);
    c.read_u64(st_value);
    c.read_u64(st_size);
    if (!c.ok()) break;
    if (st_name == 0 && st_value == 0) continue;  // null symbol
    Symbol sym;
    sym.name = strtab.empty() ? std::string() : cstr_at(strtab, st_name);
    sym.value = st_value;
    sym.size = st_size;
    sym.info = st_info;
    sym.shndx = st_shndx;
    sym.index = index;
    symbols_.push_back(std::move(sym));
  }
  return true;
}

}  // namespace c2d::elf

