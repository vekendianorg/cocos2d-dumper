// SPDX-License-Identifier: MIT
// Rebuilding ELF sections from the program-header table.
//
// A shared object dumped from a running process has a perfectly good program
// header table (the loader filled it in) but a section header table full of
// long-dead pointers -- on a real sample, `readelf` aborts with "bad shstrndx"
// and section 4 claims a size of 6.8 exabytes. Sections cannot be trusted, so
// the dynamic segment and the unwind segment are located the only reliable way:
// through PT_DYNAMIC and PT_GNU_EH_FRAME.
//
// Recovered here:
//   .dynsym, .dynstr  from DT_SYMTAB / DT_STRTAB / DT_SYMENT, with the symbol
//                     count from DT_HASH (SysV, nchain) or DT_GNU_HASH
//   .eh_frame         from PT_GNU_EH_FRAME, whose header stores an encoded
//                     pointer to the unwind section
//   .data.rel.ro     the RELRO PT_LOAD range, which holds vtables and RTTI
#include "stellar/elf/elf_file.h"

#include "stellar/diag/log.h"
#include "stellar/elf/elf_types.h"

namespace stellar::elf {
namespace {

// Values already declared in elf_types.h, referenced with a trailing underscore
// here only to avoid shadowing them inside this anonymous namespace.
constexpr std::uint32_t kPtLoad_ = 1;
constexpr std::uint32_t kPtDynamic_ = 2;
constexpr std::uint32_t kPtGnuEhFrame_ = 0x6474e550;
constexpr std::uint32_t kPtGnuRelro_ = 0x6474e552;

// Dynamic array tags.
constexpr std::uint64_t kDtHash = 4;
constexpr std::uint64_t kDtStrTab = 5;
constexpr std::uint64_t kDtSymTab = 6;
constexpr std::uint64_t kDtStrSz = 10;
constexpr std::uint64_t kDtSymEnt = 11;
constexpr std::uint64_t kDtGnuHash = 0x6ffffef5;

/// Decodes a DW_EH_PE-encoded value inside `.eh_frame_hdr`.
///
/// Only the forms a linker emits for the header's own fields are handled:
/// pcrel|sdata4 (0x1b), pcrel|sdata8 (0x1c), udata4 (0x03) and udata8 (0x04).
/// The pcrel base is the address of the field itself, matching the .eh_frame
/// convention verified elsewhere in this project.
bool read_eh_encoded(util::Cursor& c, std::uint8_t enc, std::uint64_t vaddr_base,
                     std::uint64_t& out) {
  const std::uint64_t here = vaddr_base + c.pos();
  std::uint64_t v = 0;
  switch (enc & 0x0f) {
    case 0x03: { std::uint32_t t = 0; if (!c.read_u32(t)) return false; v = t; break; }
    case 0x04: { if (!c.read_u64(v)) return false; break; }
    case 0x0b: { std::uint32_t t = 0; if (!c.read_u32(t)) return false; v = static_cast<std::uint32_t>(t); break; }
    case 0x0c: { if (!c.read_u64(v)) return false; break; }
    case 0x01: { if (!c.read_uleb128(v)) return false; break; }
    case 0x09: { std::int64_t t = 0; if (!c.read_sleb128(t)) return false; v = static_cast<std::uint64_t>(t); break; }
    default: return false;
  }
  if (enc & 0x10) v += here;  // DW_EH_PE_pcrel
  out = v;
  return true;
}

/// Number of symbols described by a GNU hash table.
///
/// The chain is shared and terminated by a set low bit, so the count is found by
/// locating the highest non-empty bucket and walking its chain to the end.
std::uint64_t gnu_hash_symbol_count(util::ByteView hash, std::uint64_t symtab_offset) {
  util::Cursor c(hash.data(), hash.size());
  std::uint32_t nbuckets = 0, symoffset = 0, bloom_size = 0;
  if (!c.read_u32(nbuckets) || !c.read_u32(symoffset) || !c.read_u32(bloom_size)) return 0;
  if (nbuckets == 0 || bloom_size == 0) return 0;
  // Skip the bloom filter (8 bytes per word on 64-bit), then the bucket array.
  std::uint64_t bloom_bytes = static_cast<std::uint64_t>(bloom_size) * 8;
  if (c.pos() + bloom_bytes > hash.size()) return 0;
  (void)c.skip(static_cast<std::size_t>(bloom_bytes));
  if (c.pos() + static_cast<std::uint64_t>(nbuckets) * 4 > hash.size()) return 0;

  std::uint32_t max_index = 0;
  bool any = false;
  for (std::uint32_t i = 0; i < nbuckets; ++i) {
    std::uint32_t b = 0;
    if (!c.read_u32(b)) return 0;
    if (b > max_index) { max_index = b; any = true; }
  }
  if (!any) return 0;

  // Walk the chain from the highest bucket to its terminator.
  const std::uint64_t chain_start = c.pos() + (max_index - symoffset) * 4;
  if (chain_start >= hash.size()) return 0;
  std::uint64_t n = symoffset;
  std::uint64_t off = chain_start;
  while (off + 4 <= hash.size()) {
    std::uint32_t e = 0;
    util::Cursor cc(hash.data() + off, 4);
    cc.read_u32(e);
    ++n;
    off += 4;
    if ((e & 1u) != 0) break;  // chain terminator
  }
  (void)symtab_offset;
  return n;
}

}  // namespace

bool ElfFile::vaddr_to_offset(std::uint64_t vaddr, std::uint64_t& offset) const {
  for (const Phdr64& p : phdrs_) {
    if (p.p_type != kPtLoad_) continue;
    if (vaddr >= p.p_vaddr && vaddr < p.p_vaddr + p.p_filesz) {
      offset = p.p_offset + (vaddr - p.p_vaddr);
      return true;
    }
  }
  return false;
}

std::size_t ElfFile::recover_sections_from_phdrs() {
  std::size_t added = 0;
  const Phdr64* dyn_phdr = nullptr;
  const Phdr64* eh_phdr = nullptr;
  const Phdr64* relro = nullptr;
  for (const Phdr64& p : phdrs_) {
    if (p.p_type == kPtDynamic_) dyn_phdr = &p;
    else if (p.p_type == kPtGnuEhFrame_) eh_phdr = &p;
    else if (p.p_type == kPtGnuRelro_) relro = &p;
  }

  // ---- .dynsym / .dynstr --------------------------------------------------
  if (dyn_phdr != nullptr) {
    std::uint64_t symtab = 0, strtab = 0, strsz = 0, syment = sizeof(Sym64);
    std::uint64_t hash_vaddr = 0, gnu_hash_vaddr = 0;
    bool have_symtab = false, have_strtab = false;
    util::Cursor d(file_.data() + dyn_phdr->p_offset,
                   static_cast<std::size_t>(dyn_phdr->p_filesz), endian());
    while (d.can(16)) {
      std::uint64_t tag = 0, val = 0;
      if (!d.read_u64(tag) || !d.read_u64(val)) break;
      if (tag == 0) break;  // DT_NULL
      switch (tag) {
        case kDtSymTab: symtab = val; have_symtab = true; break;
        case kDtStrTab: strtab = val; have_strtab = true; break;
        case kDtStrSz: strsz = val; break;
        case kDtSymEnt: if (val != 0) syment = val; break;
        case kDtHash: hash_vaddr = val; break;
        case kDtGnuHash: gnu_hash_vaddr = val; break;
        default: break;
      }
    }

    if (have_symtab && have_strtab && syment >= sizeof(Sym64)) {
      // Symbol count: SysV hash states it exactly, so prefer it.
      std::uint64_t count = 0;
      if (hash_vaddr != 0) {
        std::uint64_t off = 0;
        if (vaddr_to_offset(hash_vaddr, off)) {
          const util::ByteView hv = file_.view_at(off, 8);
          if (hv.size() == 8) {
            util::Cursor hc(hv.data(), 8, endian());
            std::uint32_t nbucket = 0, nchain = 0;
            hc.read_u32(nbucket);
            hc.read_u32(nchain);
            count = nchain;
          }
        }
      }
      if (count == 0 && gnu_hash_vaddr != 0) {
        std::uint64_t off = 0;
        if (vaddr_to_offset(gnu_hash_vaddr, off)) {
          // The hash table is bounded by the next mapped byte range; cap the
          // read so a bad pointer cannot produce a huge allocation.
          std::uint64_t limit = file_.size() - off;
          count = gnu_hash_symbol_count(file_.view_at(off, limit), symtab);
        }
      }
      if (count != 0) {
        std::uint64_t sym_off = 0, str_off = 0;
        if (vaddr_to_offset(symtab, sym_off) && vaddr_to_offset(strtab, str_off)) {
          const std::uint64_t sym_bytes = count * syment;
          if (strsz == 0 || strsz > file_.size()) strsz = file_.size() - str_off;
          Section s;
          s.name = ".dynsym";
          s.type = kShtDynsym;
          s.offset = sym_off;
          s.size = sym_bytes;
          s.addr = symtab;
          s.link = static_cast<std::uint32_t>(sections_.size() + 1);
          s.entsize = syment;
          s.index = static_cast<std::uint32_t>(sections_.size());
          add_synthetic_section(s);
          ++added;

          Section t;
          t.name = ".dynstr";
          t.type = kShtStrtab;
          t.offset = str_off;
          t.size = strsz;
          t.addr = strtab;
          t.index = static_cast<std::uint32_t>(sections_.size());
          add_synthetic_section(t);
          ++added;
          STELLAR_INFO("recovered .dynsym (%llu symbols) and .dynstr from PT_DYNAMIC",
                   static_cast<unsigned long long>(count));
        }
      }
    }
  }

  // ---- .eh_frame ----------------------------------------------------------
  // The header stores an encoded pointer to the unwind section, so the base is
  // known exactly; the records themselves are walked to their terminator.
  if (eh_phdr != nullptr) {
    const util::ByteView hdr = file_.view_at(eh_phdr->p_offset,
                                             static_cast<std::uint64_t>(eh_phdr->p_filesz));
    if (hdr.size() >= 8) {
      util::Cursor c(hdr.data(), hdr.size(), endian());
      std::uint8_t version = 0, ptr_enc = 0, cnt_enc = 0, table_enc = 0;
      c.read_u8(version);
      c.read_u8(ptr_enc);
      c.read_u8(cnt_enc);
      c.read_u8(table_enc);
      std::uint64_t base_vaddr = 0;
      if (version == 1 &&
          read_eh_encoded(c, ptr_enc, eh_phdr->p_vaddr, base_vaddr)) {
        std::uint64_t base_off = 0;
        if (vaddr_to_offset(base_vaddr, base_off)) {
          // Extend to the end of the segment that contains the base; the
          // record walk stops at the length-0 terminator regardless.
          std::uint64_t end = file_.size();
          for (const Phdr64& p : phdrs_) {
            if (p.p_type != kPtLoad_) continue;
            if (base_vaddr >= p.p_vaddr && base_vaddr < p.p_vaddr + p.p_filesz) {
              const std::uint64_t seg_end = p.p_offset + p.p_filesz;
              if (seg_end > base_off && seg_end < end) end = seg_end;
            }
          }
          Section s;
          s.name = ".eh_frame";
          s.type = kShtProgbits;
          s.offset = base_off;
          s.size = end > base_off ? end - base_off : 0;
          s.addr = base_vaddr;
          s.index = static_cast<std::uint32_t>(sections_.size());
          add_synthetic_section(s);
          ++added;
          STELLAR_INFO("recovered .eh_frame at vaddr 0x%llx from PT_GNU_EH_FRAME",
                   static_cast<unsigned long long>(base_vaddr));
        }
      }
    }
  }

  // ---- .data.rel.ro -------------------------------------------------------
  if (relro != nullptr) {
    Section s;
    s.name = ".data.rel.ro";
    s.type = kShtProgbits;
    s.offset = relro->p_offset;
    s.size = relro->p_filesz;
    s.addr = relro->p_vaddr;
    s.flags = kShfAlloc | kShfWrite;
    s.index = static_cast<std::uint32_t>(sections_.size());
    add_synthetic_section(s);
    ++added;
  }

  return added;
}

}  // namespace stellar::elf
