// SPDX-License-Identifier: MIT
// ELF64 on-disk structures, plus the small subset of constants the dumper
// needs. Declared locally rather than pulling in <elf.h> so the parser is
// self-contained and testable on any host.
#pragma once

#include <cstdint>

namespace c2d::elf {

constexpr std::uint8_t kElfMagic[4] = {0x7f, 'E', 'L', 'F'};

enum : std::uint8_t {
  kEiClass = 4,       // 1 = ELF32, 2 = ELF64
  kEiData = 5,        // 1 = little endian, 2 = big endian
  kEiVersion = 6,
  kEiOsabi = 7,
  kEiAbiversion = 8,
};

enum : std::uint16_t {
  kEtNone = 0,
  kEtRel = 1,
  kEtExec = 2,
  kEtDyn = 3,
  kEtCore = 4,
};

enum : std::uint16_t {
  kEmNone = 0,
  kEm32 = 1,
  kEmSparc = 2,
  kEm386 = 3,
  kEm68k = 4,
  kEm88k = 5,
  kEm860 = 7,
  kEmMips = 8,
  kEmArm = 40,
  kEmX86_64 = 62,
  kEmAarch64 = 183,
};

enum : std::uint32_t {
  kShtNull = 0,
  kShtProgbits = 1,
  kShtSymtab = 2,
  kShtStrtab = 3,
  kShtRela = 4,
  kShtHash = 5,
  kShtDynamic = 6,
  kShtNote = 7,
  kShtNobits = 8,
  kShtRel = 9,
  kShtShlib = 10,
  kShtDynsym = 11,
  kShtInitArray = 14,
  kShtFiniArray = 15,
  kShtPreinitArray = 16,
  kShtGroup = 17,
  kShtSymtabShndx = 18,
};

/// Section header flags we care about.
enum : std::uint64_t {
  kShfWrite = 0x1,
  kShfAlloc = 0x2,
  kShfExecinstr = 0x4,
  kShfCompress = 0x800,
};

/// Program header types.
enum : std::uint32_t {
  kPtNull = 0,
  kPtLoad = 1,
  kPtDynamic = 2,
  kPtInterp = 3,
  kPtNote = 4,
  kPtPhdr = 6,
  kPtGnuEhFrame = 0x6474e550,
};

#pragma pack(push, 1)

struct Ehdr64 {
  std::uint8_t e_ident[16];
  std::uint16_t e_type;
  std::uint16_t e_machine;
  std::uint32_t e_version;
  std::uint64_t e_entry;
  std::uint64_t e_phoff;
  std::uint64_t e_shoff;
  std::uint32_t e_flags;
  std::uint16_t e_ehsize;
  std::uint16_t e_phentsize;
  std::uint16_t e_phnum;
  std::uint16_t e_shentsize;
  std::uint16_t e_shnum;
  std::uint16_t e_shstrndx;
};

struct Shdr64 {
  std::uint32_t sh_name;
  std::uint32_t sh_type;
  std::uint64_t sh_flags;
  std::uint64_t sh_addr;
  std::uint64_t sh_offset;
  std::uint64_t sh_size;
  std::uint32_t sh_link;
  std::uint32_t sh_info;
  std::uint64_t sh_addralign;
  std::uint64_t sh_entsize;
};

struct Phdr64 {
  std::uint32_t p_type;
  std::uint32_t p_flags;
  std::uint64_t p_offset;
  std::uint64_t p_vaddr;
  std::uint64_t p_paddr;
  std::uint64_t p_filesz;
  std::uint64_t p_memsz;
  std::uint64_t p_align;
};

struct Sym64 {
  std::uint32_t st_name;
  std::uint8_t st_info;
  std::uint8_t st_other;
  std::uint16_t st_shndx;
  std::uint64_t st_value;
  std::uint64_t st_size;
};

static_assert(sizeof(Ehdr64) == 64, "Ehdr64 layout");
static_assert(sizeof(Shdr64) == 64, "Shdr64 layout");
static_assert(sizeof(Phdr64) == 56, "Phdr64 layout");
static_assert(sizeof(Sym64) == 24, "Sym64 layout");

#pragma pack(pop)

[[nodiscard]] const char* machine_name(std::uint16_t machine);
[[nodiscard]] const char* object_type_name(std::uint16_t type);
[[nodiscard]] const char* section_type_name(std::uint32_t type);

}  // namespace c2d::elf
