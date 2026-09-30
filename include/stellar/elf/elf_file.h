// SPDX-License-Identifier: MIT
// ELF container parsing. Knows nothing about DWARF: it exposes sections and
// symbols, and the DWARF layer decides what to make of them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stellar/elf/elf_types.h"
#include "stellar/elf/mapped_file.h"
#include "stellar/util/bytes.h"

namespace stellar::elf {

/// A parsed section header together with its (resolved) name.
struct Section {
  std::uint32_t index = 0;
  std::string name;
  std::uint32_t type = 0;
  std::uint64_t flags = 0;
  std::uint64_t addr = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t addralign = 0;
  std::uint64_t entsize = 0;

  [[nodiscard]] bool is_alloc() const { return (flags & kShfAlloc) != 0; }
  [[nodiscard]] bool is_nobits() const { return type == kShtNobits; }
  /// Bytes this section actually occupies in the file (0 for SHT_NOBITS).
  [[nodiscard]] std::uint64_t file_size() const { return is_nobits() ? 0 : size; }
  [[nodiscard]] bool is_debug() const { return name.rfind(".debug", 0) == 0; }
};

/// One entry of the symbol table (already resolved to a name).
struct Symbol {
  std::string name;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
  std::uint8_t info = 0;
  std::uint16_t shndx = 0;
  std::uint32_t index = 0;

  [[nodiscard]] std::uint8_t type() const { return static_cast<std::uint8_t>(info & 0xf); }
  [[nodiscard]] std::uint8_t bind() const { return static_cast<std::uint8_t>(info >> 4); }
};

enum class StT : std::uint8_t {
  kNotype = 0, kObject = 1, kFunc = 2, kSection = 3, kFile = 4,
  kCommon = 5, kTls = 6, kGnuIfunc = 10,
};

enum class StB : std::uint8_t {
  kLocal = 0, kGlobal = 1, kWeak = 2, kGnuUnique = 10,
};

class ElfFile {
 public:
  /// Opens, maps and validates the ELF container. Returns false with a
  /// human-readable reason in `error`.
  bool open(const std::string& path, std::string* error);

  [[nodiscard]] const MappedFile& mapping() const { return file_; }
  [[nodiscard]] const Ehdr64& header() const { return ehdr_; }
  [[nodiscard]] bool is_64bit() const { return is_64bit_; }
  [[nodiscard]] bool is_little_endian() const { return little_endian_; }
  [[nodiscard]] util::Endian endian() const {
    return little_endian_ ? util::Endian::Little : util::Endian::Big;
  }
  [[nodiscard]] const std::string& path() const { return file_.path(); }
  [[nodiscard]] std::uint64_t file_size() const { return file_.size(); }

  [[nodiscard]] const std::vector<Section>& sections() const { return sections_; }
  [[nodiscard]] const std::vector<Phdr64>& program_headers() const { return phdrs_; }

  /// When a memory dump has a stale, zeroed or absent section table, rebuild
  /// the sections the program headers still describe. This is the normal case
  /// for a library dumped from a running process: the loader fills in segment
  /// addresses, but the section header table holds long-dead pointers.
  ///
  /// Recovers `.dynsym`/`.dynstr` (from PT_DYNAMIC: DT_SYMTAB, DT_STRTAB,
  /// DT_SYMENT, plus DT_HASH or DT_GNU_HASH for the symbol count) and
  /// `.eh_frame` (from PT_GNU_EH_FRAME, whose header carries an encoded
  /// pointer to the unwind section). Returns how many were synthesised.
  std::size_t recover_sections_from_phdrs();

  /// Maps a virtual address to a file offset using the PT_LOAD table, or
  /// returns false when the address is not backed by file contents.
  [[nodiscard]] bool vaddr_to_offset(std::uint64_t vaddr, std::uint64_t& offset) const;

  /// True when the section header table is absent or describes ranges outside
  /// the file, which is what a library dumped from memory looks like.
  [[nodiscard]] bool needs_phdr_recovery() const;

  /// Appends a synthesised section (recovery path only).
  void add_synthetic_section(Section s) { sections_.push_back(std::move(s)); }

  /// Section lookup by exact name; nullptr when absent.
  [[nodiscard]] const Section* find_section(std::string_view name) const;
  /// Byte view of a named section; empty when absent or SHT_NOBITS.
  [[nodiscard]] util::ByteView section_data(std::string_view name) const;
  /// Byte view of a section by index; empty when out of range or SHT_NOBITS.
  [[nodiscard]] util::ByteView section_data(std::uint32_t index) const;

  /// Total size of all SHT_NOBITS-free .debug_* sections.
  [[nodiscard]] std::uint64_t debug_bytes() const;
  /// Names of all .debug_* sections, in section-table order.
  [[nodiscard]] std::vector<std::string> debug_section_names() const;

  /// Symbols from .symtab (or .dynsym when .symtab is absent/stripped).
  [[nodiscard]] const std::vector<Symbol>& symbols() const { return symbols_; }
  [[nodiscard]] bool has_symtab() const { return has_symtab_; }
  [[nodiscard]] bool has_dynsym() const { return has_dynsym_; }
  /// Section index that .symtab/.dynsym string table lives in, or 0.
  [[nodiscard]] std::uint32_t symtab_shndx() const { return symtab_shndx_; }
  [[nodiscard]] std::uint32_t dynsym_shndx() const { return dynsym_shndx_; }

  /// A one-line identity for reports, e.g. "ELF64 DYN aarch64".
  [[nodiscard]] std::string describe() const;

 private:
  bool parse_headers(std::string* error);
  bool parse_sections(std::string* error);
  bool parse_symbols(std::string* error);
  [[nodiscard]] std::string string_at(std::uint32_t strtab_index,
                                      std::uint32_t name_offset) const;

  MappedFile file_;
  Ehdr64 ehdr_{};
  std::vector<Phdr64> phdrs_;
  std::vector<Section> sections_;
  std::vector<Symbol> symbols_;
  std::vector<std::string> shstrtab_;
  bool is_64bit_ = false;
  bool little_endian_ = true;
  bool has_symtab_ = false;
  bool has_dynsym_ = false;
  std::uint32_t symtab_shndx_ = 0;
  std::uint32_t dynsym_shndx_ = 0;
};

}  // namespace stellar::elf
