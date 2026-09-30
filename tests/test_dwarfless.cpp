// SPDX-License-Identifier: MIT
// Tests for the dwarfless path: a binary with no .debug_info at all.
//
// The .eh_frame rules are worth pinning down here because they are easy to get
// wrong and quiet when wrong: the CIE pointer is the distance from the pointer
// *field* back to the CIE, and the 'R' augmentation byte is data index 0 (not 1)
// for a "zR" augmentation. Both were wrong in the first implementation and
// produced zero functions, so they are asserted directly.
#include <cstdio>
#include <string>
#include <vector>

#include "c2d/dwarf/dwarf_context.h"
#include "c2d/dwarf/eh_frame.h"
#include "c2d/ir/build.h"
#include "c2d/output/emit.h"
#include "fixture_builder.h"
#include "test_framework.h"

using namespace c2d;

namespace {

constexpr std::uint64_t kEhAddr = 0x1000;

/// A stripped ELF: an .eh_frame with three functions and no .debug_info.
template <typename Fn>
void with_stripped_binary(Fn&& fn) {
  c2d::test::DebugSections dbg;
  dbg.eh_frame_addr = kEhAddr;
  dbg.eh_frame = c2d::test::build_eh_frame(
      {{0x2000, 0x40}, {0x2040, 0x10}, {0x2100, 0x80}}, kEhAddr);
  const std::string path = c2d::test::temp_path("c2d_stripped.so");
  c2d::test::write_file(path, c2d::test::build_elf(dbg));
  fn(path);
}

std::string slurp(const std::string& path) {
  std::string text;
  if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) text.append(buf, n);
    std::fclose(in);
  }
  return text;
}

}  // namespace

C2D_TEST(Dwarfless, EhFrameRecoversExactFunctionRanges) {
  c2d::test::DebugSections dbg;
  dbg.eh_frame_addr = kEhAddr;
  dbg.eh_frame = c2d::test::build_eh_frame(
      {{0x2000, 0x40}, {0x2040, 0x10}, {0x2100, 0x80}}, kEhAddr);

  dwarf::EhFrameStats st;
  const auto ranges = dwarf::parse_eh_frame(
      util::ByteView(dbg.eh_frame.data(), dbg.eh_frame.size()), kEhAddr, &st);
  ASSERT_EQ_SIZE(ranges, 3);
  EXPECT_EQ(st.cies, std::uint64_t{1});
  EXPECT_EQ(st.fdes, std::uint64_t{3});
  EXPECT_FALSE(st.truncated);
  EXPECT_EQ(ranges[0].start, std::uint64_t{0x2000});
  EXPECT_EQ(ranges[0].size, std::uint64_t{0x40});
  EXPECT_EQ(ranges[1].start, std::uint64_t{0x2040});
  EXPECT_EQ(ranges[1].size, std::uint64_t{0x10});
  EXPECT_EQ(ranges[2].start, std::uint64_t{0x2100});
  EXPECT_EQ(ranges[2].size, std::uint64_t{0x80});
}

C2D_TEST(Dwarfless, EhFrameStopsCleanlyOnTruncatedInput) {
  c2d::test::DebugSections dbg;
  dbg.eh_frame_addr = kEhAddr;
  dbg.eh_frame = c2d::test::build_eh_frame({{0x2000, 0x40}}, kEhAddr);
  dbg.eh_frame.resize(dbg.eh_frame.size() - 3);  // chop the last FDE
  dwarf::EhFrameStats st;
  const auto ranges = dwarf::parse_eh_frame(
      util::ByteView(dbg.eh_frame.data(), dbg.eh_frame.size()), kEhAddr, &st);
  // Never invents a range from a partial record.
  EXPECT_EQ(ranges.size(), std::size_t{0});
}

C2D_TEST(Dwarfless, StrippedBinaryIsDetectedAndFunctionsAreNamedSubAddr) {
  with_stripped_binary([](const std::string& path) {
    std::string err;
    elf::ElfFile f;
    EXPECT_TRUE(f.open(path, &err));
    dwarf::DwarfContext ctx(f);
    EXPECT_FALSE(ctx.sections().has_info());
    EXPECT_TRUE(ctx.sections().has(dwarf::Sec::kEhFrame));

    ir::Model m;
    ir::DwarflessStats st;
    EXPECT_TRUE(ir::build_dwarfless_model(ctx, m, &st));
    EXPECT_TRUE(m.inferred);
    EXPECT_EQ(st.fdes, std::uint64_t{3});
    // No symbol table in the fixture, so every function is named by address.
    EXPECT_EQ(st.functions_sub_, std::uint64_t{3});
    EXPECT_EQ(st.functions_named, std::uint64_t{0});
    ASSERT_EQ_SIZE(m.functions, 3);
    EXPECT_EQ(m.arena(m.functions[0].name_off), std::string_view("sub_2000"));
    EXPECT_EQ(m.functions[0].addr, std::uint64_t{0x2000});
    EXPECT_EQ(m.functions[0].size, std::uint64_t{0x40});
    EXPECT_EQ(m.functions[0].named, std::uint8_t{0});
  });
}

C2D_TEST(Dwarfless, DumpCarriesTheWarningBannerAndTierTags) {
  with_stripped_binary([](const std::string& path) {
    elf::ElfFile f;
    std::string err;
    EXPECT_TRUE(f.open(path, &err));
    dwarf::DwarfContext ctx(f);
    ir::Model m;
    ir::DwarflessStats st;
    EXPECT_TRUE(ir::build_dwarfless_model(ctx, m, &st));

    output::EmitOptions o;
    o.inferred = true;
    o.named_functions = st.functions_named;
    o.unnamed_functions = st.functions_sub_;
    const std::string out_path = c2d::test::temp_path("c2d_dwarfless.cs");
    std::FILE* out = std::fopen(out_path.c_str(), "wb");
    EXPECT_TRUE(out != nullptr);
    output::EmitStats es;
    output::emit_il2cpp(m, out, o, &es);
    std::fclose(out);
    const std::string text = slurp(out_path);

    // The warning must be unmissable and must say the output is not truth.
    EXPECT_TRUE(text.find("WARNING: LOW-ACCURACY OUTPUT") != std::string::npos);
    EXPECT_TRUE(text.find("NOT GROUND TRUTH") != std::string::npos);
    EXPECT_TRUE(text.find("field offsets recoverable     : 0") != std::string::npos);
    // Every record states its provenance so it can be grepped.
    EXPECT_TRUE(text.find("| TIER:infer") != std::string::npos);
    EXPECT_TRUE(text.find("public static IntPtr sub_2000; // RVA: 0x2000") != std::string::npos);
    // And the DWARF-mode emitter must NOT emit the warning.
    EXPECT_TRUE(text.find("// VEKENDIAN Cocos2d Dumper Il2cpp-style") != std::string::npos);
  });
}

C2D_TEST(Dwarfless, DwarfModeDumpHasNoWarningBanner) {
  // The same model, emitted without the inferred flag, must stay clean: this
  // guards against the warning leaking into authoritative dumps.
  ir::Model m;
  m.inferred = false;
  output::EmitOptions o;  // inferred defaults to false
  const std::string out_path = c2d::test::temp_path("c2d_dwarf_mode.cs");
  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  EXPECT_TRUE(out != nullptr);
  output::EmitStats es;
  output::emit_il2cpp(m, out, o, &es);
  std::fclose(out);
  const std::string text = slurp(out_path);
  EXPECT_TRUE(text.find("WARNING") == std::string::npos);
  EXPECT_TRUE(text.find("TIER:") == std::string::npos);
}
