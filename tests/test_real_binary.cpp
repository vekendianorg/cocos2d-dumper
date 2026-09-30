// SPDX-License-Identifier: MIT
// Tests against the real target binary.
//
// These are opt-in (ctest target `stellar_real_binary`) because the input is 583 MB
// and a full DIE traversal must not run on every edit. The expected values
// below were measured from that binary; see docs/MILESTONES.md for how.
#include <chrono>
#include <cstdio>
#include <string>

#include "stellar/diag/log.h"
#include "stellar/diag/metrics.h"
#include "stellar/dwarf/dwarf_context.h"
#include "stellar/elf/elf_file.h"
#include "fixture_builder.h"
#include "test_framework.h"

using namespace stellar;

namespace {

/// Opens the real binary, or returns false (tests then report a skip).
bool open_real(elf::ElfFile& f, std::string& path) {
  path = stellar::test::real_binary_path();
  if (path.empty()) {
    std::fprintf(stderr, "  (skipped: real binary not present)\n");
    return false;
  }
  std::string err;
  if (!f.open(path, &err)) {
    std::fprintf(stderr, "  (skipped: %s)\n", err.c_str());
    return false;
  }
  return true;
}

}  // namespace

STELLAR_TEST(RealBinary, ElfHeaderAndSectionsMatchTheMeasuredTarget) {
  elf::ElfFile f;
  std::string path;
  if (!open_real(f, path)) return;

  EXPECT_TRUE(f.is_64bit());
  EXPECT_TRUE(f.is_little_endian());
  EXPECT_EQ(static_cast<int>(f.header().e_machine), static_cast<int>(elf::kEmAarch64));
  EXPECT_EQ(static_cast<int>(f.header().e_type), static_cast<int>(elf::kEtDyn));
  // readelf -S reports 35 sections for this object.
  EXPECT_EQ(f.sections().size(), std::size_t{35});
  // The binary is unstripped: it carries both .symtab and .dynsym.
  EXPECT_TRUE(f.has_symtab());
  EXPECT_TRUE(f.has_dynsym());
  EXPECT_TRUE(f.find_section(".debug_info") != nullptr);
  EXPECT_TRUE(f.find_section(".debug_abbrev") != nullptr);
  EXPECT_TRUE(f.find_section(".debug_str") != nullptr);
  // DWARF 4 only: no .debug_types / .debug_str_offsets / .debug_addr /
  // .debug_rnglists / .debug_loclists / .debug_line_str are present.
  EXPECT_TRUE(f.find_section(".debug_types") == nullptr);
  EXPECT_TRUE(f.find_section(".debug_str_offsets") == nullptr);
  EXPECT_TRUE(f.find_section(".debug_addr") == nullptr);
  EXPECT_TRUE(f.find_section(".debug_rnglists") == nullptr);
  EXPECT_TRUE(f.find_section(".debug_loclists") == nullptr);
  EXPECT_TRUE(f.find_section(".debug_line_str") == nullptr);
  // ...while the DWARF 2-4 companions are.
  EXPECT_TRUE(f.find_section(".debug_ranges") != nullptr);
  EXPECT_TRUE(f.find_section(".debug_loc") != nullptr);
}

STELLAR_TEST(RealBinary, UnitCountAndVersionsMatchTheMeasuredTarget) {
  elf::ElfFile f;
  std::string path;
  if (!open_real(f, path)) return;
  dwarf::DwarfContext ctx(f);

  // Measured: 1183 compilation units, all DWARF version 4, all 32-bit offsets.
  EXPECT_EQ(ctx.unit_count(), std::uint64_t{1183});

  std::uint64_t v4 = 0, other = 0, dwarf64 = 0, bad = 0;
  dwarf::DwarfContext::UnitIterator it(ctx);
  dwarf::UnitHeader h;
  std::string err;
  while (it.next(h, &err)) {
    if (h.version == 4) ++v4; else ++other;
    if (h.is_dwarf64()) ++dwarf64;
    if (h.address_size != 8) ++bad;
  }
  EXPECT_EQ(v4, std::uint64_t{1183});
  EXPECT_EQ(other, std::uint64_t{0});
  EXPECT_EQ(dwarf64, std::uint64_t{0});
  EXPECT_EQ(bad, std::uint64_t{0});
  EXPECT_EQ(it.skipped_errors(), std::uint64_t{0});
}

STELLAR_TEST(RealBinary, BoundedUnitScanMatchesAnIndependentWalk) {
  elf::ElfFile f;
  std::string path;
  if (!open_real(f, path)) return;
  dwarf::DwarfContext ctx(f);

  // A bounded scan of the first N units: unit 0 is the smallest and parses to
  // 5713 DIEs (verified against a reference walker).
  dwarf::UnitHeader h0{};
  std::string err;
  EXPECT_TRUE(ctx.unit_header(0, h0, &err));
  std::uint64_t dies = 0;
  const dwarf::AbbrevTable* ab = ctx.abbrev_table(h0, &err);
  EXPECT_TRUE(ab != nullptr);
  dwarf::UnitWalker w(ctx.info(), h0, ab);
  w.reset();
  dwarf::Die d;
  while (w.next(d)) ++dies;
  EXPECT_EQ(dies, std::uint64_t{5713});
}

STELLAR_TEST(RealBinary, FullDieTraversalIsConsistentAndBoundedInMemory) {
  elf::ElfFile f;
  std::string path;
  if (!open_real(f, path)) return;
  dwarf::DwarfContext ctx(f);

  const auto t0 = diag::Clock::now();
  std::uint64_t dies = 0, units = 0, max_depth = 0, failed = 0;
  std::vector<std::uint64_t> tag_counts(0x500, 0);
  dwarf::DwarfContext::UnitIterator it(ctx);
  dwarf::UnitHeader h;
  std::string err;
  while (it.next(h, &err)) {
    const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
    if (ab == nullptr) { ++failed; continue; }
    dwarf::UnitWalker w(ctx.info(), h, ab);
    w.reset();
    dwarf::Die d;
    while (w.next(d)) {
      ++dies;
      if (d.tag() < tag_counts.size()) ++tag_counts[d.tag()];
      if (d.depth() > max_depth) max_depth = d.depth();
    }
    ++units;
  }
  const double secs = diag::seconds_since(t0);

  std::fprintf(stderr, "  measured: units=%llu dies=%llu max_depth=%llu failed=%llu "
                       "elapsed=%.2fs peak_rss=%s\n",
               static_cast<unsigned long long>(units), static_cast<unsigned long long>(dies),
               static_cast<unsigned long long>(max_depth),
               static_cast<unsigned long long>(failed), secs,
               util::human_size(diag::peak_rss_bytes()).c_str());

  EXPECT_EQ(units, std::uint64_t{1183});
  EXPECT_EQ(failed, std::uint64_t{0});
  // 20,454,580 DIEs measured by an independent reference walker.
  EXPECT_EQ(dies, std::uint64_t{20454580});
  EXPECT_EQ(max_depth, std::uint64_t{27});
  // Spot-check the tag mix, which is what drives type reconstruction.
  EXPECT_EQ(tag_counts[dwarf::tag::kCompileUnit], std::uint64_t{1183});
  EXPECT_EQ(tag_counts[dwarf::tag::kStructureType], std::uint64_t{238843});
  EXPECT_EQ(tag_counts[dwarf::tag::kClass], std::uint64_t{217510});
  EXPECT_EQ(tag_counts[dwarf::tag::kEnumeration], std::uint64_t{10293});
  EXPECT_EQ(tag_counts[dwarf::tag::kUnion], std::uint64_t{1621});
  EXPECT_EQ(tag_counts[dwarf::tag::kMember], std::uint64_t{369148});
  EXPECT_EQ(tag_counts[dwarf::tag::kInheritance], std::uint64_t{140098});

  // Streaming traversal must not scale memory with DIE count.
  const std::uint64_t rss = diag::peak_rss_bytes();
  EXPECT_TRUE(rss > 0);
  EXPECT_TRUE(rss < 700ull * 1024 * 1024);  // well under the binary's own size
}
