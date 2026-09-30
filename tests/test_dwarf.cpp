// SPDX-License-Identifier: MIT
// Unit-level tests for the byte cursor, LEB128, unit headers, abbreviation
// tables and DIE walking. All of them run against hand-built fixtures so the
// suite needs neither a compiler nor the 583 MB target binary.
#include <string>
#include <vector>

#include "c2d/diag/log.h"
#include "c2d/dwarf/abbrev.h"
#include "c2d/dwarf/dwarf_context.h"
#include "c2d/dwarf/unit.h"
#include "c2d/elf/elf_file.h"
#include "c2d/util/bytes.h"
#include "fixture_builder.h"
#include "test_framework.h"

using namespace c2d;
using c2d::test::AbbrevSpec;
using c2d::test::Bytes;
using c2d::test::build_abbrev;
using c2d::test::build_unit_dwarf64;
using c2d::test::build_unit_v4;
using c2d::test::build_unit_v5;
using c2d::test::RawDie;

// The abbreviation tests spell constants without the `dwarf::` prefix.
using namespace c2d::dwarf;

/// Convenience: a ByteView over a byte vector owned by the caller.
inline util::ByteView bv(const std::vector<std::uint8_t>& v) {
  return util::ByteView(v.data(), v.size());
}

// ---------------------------------------------------------------------------
// util::Cursor
// ---------------------------------------------------------------------------

C2D_TEST(Cursor, ReadsFixedWidthInBothEndiannesses) {
  const std::uint8_t buf[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  util::Cursor le(buf, sizeof(buf), util::Endian::Little);
  std::uint32_t v32 = 0;
  EXPECT_TRUE(le.read_u32(v32));
  EXPECT_EQ(v32, 0x04030201u);
  util::Cursor be(buf, sizeof(buf), util::Endian::Big);
  EXPECT_TRUE(be.read_u32(v32));
  EXPECT_EQ(v32, 0x01020304u);
}

C2D_TEST(Cursor, Uleb128RoundTrip) {
  const std::vector<std::uint64_t> values = {0, 1, 127, 128, 255, 624485, 0x7fffffff,
                                             0xffffffffull, 0x1ffffffffull};
  for (std::uint64_t want : values) {
    Bytes b;
    b.uleb(want);
    const auto& raw = b.bytes();
    util::Cursor c(raw.data(), raw.size());
    std::uint64_t got = 0;
    EXPECT_TRUE(c.read_uleb128(got));
    EXPECT_EQ(got, want);
  }
}

C2D_TEST(Cursor, Sleb128SignExtension) {
  const std::vector<std::int64_t> values = {0, -1, 63, -64, 127, -128, 12345, -12345};
  for (std::int64_t want : values) {
    Bytes b;
    b.sleb(want);
    const auto& raw = b.bytes();
    util::Cursor c(raw.data(), raw.size());
    std::int64_t got = 0;
    EXPECT_TRUE(c.read_sleb128(got));
    EXPECT_EQ(got, want);
  }
}

C2D_TEST(Cursor, OutOfBoundsReadLatchesErrorAndDoesNotOverrun) {
  const std::uint8_t buf[3] = {1, 2, 3};
  util::Cursor c(buf, sizeof(buf));
  std::uint64_t v = 0;
  EXPECT_TRUE(c.ok());
  // 8-byte read from a 3-byte buffer must fail cleanly.
  EXPECT_FALSE(c.read_uint(8, v));
  EXPECT_FALSE(c.ok());
  EXPECT_EQ(c.remaining(), std::size_t{0});
}

C2D_TEST(Cursor, TruncatedUlebIsRejected) {
  // 0x80 means "continues" but the buffer ends: must not spin or read past.
  const std::uint8_t buf[2] = {0x80, 0x80};
  util::Cursor c(buf, sizeof(buf));
  std::uint64_t v = 0;
  EXPECT_FALSE(c.read_uleb128(v));
}

// ---------------------------------------------------------------------------
// unit headers
// ---------------------------------------------------------------------------

C2D_TEST(UnitHeader, V4LengthExcludesTheLengthFieldItself) {
  // Regression guard for the classic off-by-4: unit_length counts everything
  // after the length field, so a 2-unit .debug_info must place unit 1 at
  // 4 + unit_length, not 11 + unit_length.
  RawDie root{1, {}};
  std::vector<RawDie> dies = {root, {0, {}}};
  auto u0 = build_unit_v4(0, 8, dies);

  RawDie root2{1, {}};
  std::vector<RawDie> dies2 = {root2, {0, {}}};
  auto u1 = build_unit_v4(0, 8, dies2);
  u0.insert(u0.end(), u1.begin(), u1.end());

  const util::ByteView info(u0.data(), u0.size());
  dwarf::UnitHeader h0{}, h1{};
  std::string err;
  EXPECT_TRUE(parse_unit_header(info, 0, util::Endian::Little, h0, &err));
  EXPECT_EQ(h0.version, 4);
  EXPECT_EQ(h0.address_size, 8);
  EXPECT_EQ(h0.length_size, 4u);
  EXPECT_EQ(h0.die_start, std::uint64_t{11});
  // The second unit must start exactly at the end of the first.
  EXPECT_EQ(h0.die_end, u1.size());
  EXPECT_TRUE(parse_unit_header(info, h0.die_end, util::Endian::Little, h1, &err));
  EXPECT_EQ(h1.die_start, h0.die_end + 11);
}

C2D_TEST(UnitHeader, Dwarf64UsesTheFfffffffMarker) {
  std::vector<RawDie> dies = {{1, {}}, {0, {}}};
  auto u = build_unit_dwarf64(4, 8, 0, dies);
  const util::ByteView info(u.data(), u.size());
  dwarf::UnitHeader h{};
  std::string err;
  EXPECT_TRUE(parse_unit_header(info, 0, util::Endian::Little, h, &err));
  EXPECT_TRUE(h.is_dwarf64());
  EXPECT_EQ(h.length_size, 8u);
  EXPECT_EQ(h.version, 4);
  // DWARF64 v4 header is 4+8+2+8+1 = 23 bytes before the DIEs.
  EXPECT_EQ(h.die_start, std::uint64_t{23});
}

C2D_TEST(UnitHeader, V5HeaderLayoutIsReordered) {
  std::vector<RawDie> dies = {{1, {}}, {0, {}}};
  auto u = build_unit_v5(dwarf::utype::kCompile, 8, 0, dies);
  const util::ByteView info(u.data(), u.size());
  dwarf::UnitHeader h{};
  std::string err;
  EXPECT_TRUE(parse_unit_header(info, 0, util::Endian::Little, h, &err));
  EXPECT_EQ(h.version, 5);
  EXPECT_EQ(static_cast<int>(h.unit_type), static_cast<int>(dwarf::utype::kCompile));
  EXPECT_EQ(h.die_start, std::uint64_t{12});  // 4 + 2 + 1 + 1 + 4
}

C2D_TEST(UnitHeader, RejectsTruncatedAndReservedInput) {
  // Too short to hold a header.
  const std::uint8_t tiny[3] = {0x10, 0, 0};
  dwarf::UnitHeader h{};
  std::string err;
  EXPECT_FALSE(parse_unit_header(util::ByteView(tiny, sizeof(tiny)), 0, util::Endian::Little, h,
                                 &err));
  // Reserved initial length (0xfffffff0..0xfffffffe).
  const std::uint8_t reserved[16] = {0xf0, 0xff, 0xff, 0xff};
  EXPECT_FALSE(
      parse_unit_header(util::ByteView(reserved, sizeof(reserved)), 0, util::Endian::Little, h,
                        &err));
  // A unit that claims to extend past the end of the section.
  Bytes b;
  b.u32(0x00ffffff);  // 16 MB
  b.u16(4);
  b.u32(0);
  b.u8(8);
  const auto& raw = b.bytes();
  EXPECT_FALSE(parse_unit_header(util::ByteView(raw.data(), raw.size()), 0, util::Endian::Little,
                                 h, &err));
}

C2D_TEST(UnitHeader, RejectsImplausibleAddressSize) {
  Bytes b;
  b.u32(20);
  b.u16(4);
  b.u32(0);
  b.u8(0);  // address_size == 0 is invalid
  for (int i = 0; i < 12; ++i) b.u8(0);
  const auto& raw = b.bytes();
  dwarf::UnitHeader h{};
  std::string err;
  EXPECT_FALSE(parse_unit_header(util::ByteView(raw.data(), raw.size()), 0, util::Endian::Little,
                                 h, &err));
}

// ---------------------------------------------------------------------------
// abbreviation tables
// ---------------------------------------------------------------------------

C2D_TEST(Abbrev, ParsesTagsChildrenAndAttributeList) {
  std::vector<AbbrevSpec> specs = {
      {1, dwarf::tag::kCompileUnit, true,
       {{aat::kProducer, form::kStrp, 0}, {aat::kLanguage, form::kData2, 0},
        {aat::kName, form::kStrp, 0}, {aat::kLowPc, form::kAddr, 0},
        {aat::kRanges, form::kSecOffset, 0}}},
      {2, dwarf::tag::kVariable, false,
       {{aat::kName, form::kStrp, 0}, {aat::kType, form::kRef4, 0},
        {aat::kDeclLine, form::kData1, 0}, {aat::kDeclColumn, form::kData1, 0}}},
  };
  auto ab = build_abbrev(specs);
  dwarf::AbbrevTable t;
  std::string err;
  EXPECT_TRUE(t.parse(bv(ab), 0, util::Endian::Little, &err));
  EXPECT_EQ(t.size(), std::size_t{2});
  EXPECT_TRUE(t.valid(1));
  EXPECT_FALSE(t.valid(0));
  EXPECT_FALSE(t.valid(99));
  EXPECT_EQ(t.get(1).tag, static_cast<std::uint32_t>(dwarf::tag::kCompileUnit));
  EXPECT_TRUE(t.get(1).has_children);
  EXPECT_FALSE(t.get(2).has_children);
  EXPECT_EQ(t.get(1).attrs.size(), std::size_t{5});
  EXPECT_EQ(t.get(1).attrs[1].attr, static_cast<std::uint32_t>(dwarf::aat::kLanguage));
  EXPECT_EQ(t.get(1).attrs[1].form, static_cast<uint64_t>(dwarf::form::kData2));
}

C2D_TEST(Abbrev, ImplicitConstCarriesItsValueInTheAbbrev) {
  // DW_FORM_implicit_const must consume a SLEB in the abbreviation and zero
  // bytes in the DIE -- a very easy thing to get wrong when skipping DIEs.
  std::vector<AbbrevSpec> specs = {{1, dwarf::tag::kEnumerator, false,
                                   {{aat::kName, form::kStrp, 0},
                                    {aat::kConstValue, form::kImplicitConst, -3}}},
                                  };
  auto ab = build_abbrev(specs);
  dwarf::AbbrevTable t;
  std::string err;
  EXPECT_TRUE(t.parse(bv(ab), 0, util::Endian::Little, &err));
  EXPECT_EQ(t.get(1).attrs[1].implicit_const, std::int64_t{-3});
}

C2D_TEST(Abbrev, FinaliseComputesFixedSizeOnlyWhenAllFormsAreFixed) {
  std::vector<AbbrevSpec> specs = {
      {1, dwarf::tag::kMember, false,
       {{aat::kName, form::kStrp, 0}, {aat::kType, form::kRef4, 0},
        {aat::kDeclLine, form::kData1, 0}}},
      {2, dwarf::tag::kLexicalBlock, true,
       {{aat::kLocation, form::kExprloc, 0}}},
  };
  auto ab = build_abbrev(specs);
  dwarf::AbbrevTable t;
  std::string err;
  EXPECT_TRUE(t.parse(bv(ab), 0, util::Endian::Little, &err));
  t.finalise(8, 4);
  // strp(4) + ref4(4) + data1(1) == 9
  EXPECT_TRUE(t.get(1).size_known);
  EXPECT_EQ(t.get(1).fixed_size, 9);
  // exprloc is length-prefixed, so no constant size is available.
  EXPECT_FALSE(t.get(2).size_known);
}

C2D_TEST(Abbrev, RejectsOffsetPastEndOfSection) {
  const std::uint8_t buf[2] = {0, 0};
  dwarf::AbbrevTable t;
  std::string err;
  EXPECT_FALSE(t.parse(util::ByteView(buf, sizeof(buf)), 100, util::Endian::Little, &err));
}

C2D_TEST(FormSize, MatchesTheDwarfSpecification) {
  std::uint64_t n = 0;
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kData1, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{1});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kData4, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{4});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kData8, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{8});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kData16, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{16});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kAddr, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{8});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kAddr, 4, 4, n));
  EXPECT_EQ(n, std::uint64_t{4});
  // strp uses the offset size, so DWARF64 makes it 8 bytes.
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kStrp, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{4});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kStrp, 8, 8, n));
  EXPECT_EQ(n, std::uint64_t{8});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kFlagPresent, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{0});
  EXPECT_TRUE(dwarf::form_size(dwarf::form::kImplicitConst, 8, 4, n));
  EXPECT_EQ(n, std::uint64_t{0});
  // Variable-length forms must report "unknown" so the walker decodes them.
  EXPECT_FALSE(dwarf::form_size(dwarf::form::kUdata, 8, 4, n));
  EXPECT_FALSE(dwarf::form_size(dwarf::form::kSdata, 8, 4, n));
  EXPECT_FALSE(dwarf::form_size(dwarf::form::kString, 8, 4, n));
  EXPECT_FALSE(dwarf::form_size(dwarf::form::kExprloc, 8, 4, n));
  EXPECT_FALSE(dwarf::form_size(dwarf::form::kBlock, 8, 4, n));
  EXPECT_FALSE(dwarf::form_size(0x99, 8, 4, n));
}

C2D_TEST(FormClassification, IdentifiesUnitRelativeAndIndexedForms) {
  EXPECT_TRUE(dwarf::is_unit_relative_ref(dwarf::form::kRef1));
  EXPECT_TRUE(dwarf::is_unit_relative_ref(dwarf::form::kRef4));
  EXPECT_TRUE(dwarf::is_unit_relative_ref(dwarf::form::kRefUdata));
  // ref_addr is section-relative in DWARF3+, not unit-relative.
  EXPECT_FALSE(dwarf::is_unit_relative_ref(dwarf::form::kRefAddr));
  EXPECT_FALSE(dwarf::is_unit_relative_ref(dwarf::form::kSecOffset));
  EXPECT_TRUE(dwarf::is_indexed_string(dwarf::form::kStrx));
  EXPECT_TRUE(dwarf::is_indexed_string(dwarf::form::kStrx4));
  EXPECT_FALSE(dwarf::is_indexed_string(dwarf::form::kStrp));
}

// ---------------------------------------------------------------------------
// end-to-end: synthetic ELF -> DwarfContext -> DIE walk
// ---------------------------------------------------------------------------

namespace {

/// Builds a fixture ELF in `path` and returns it (or an empty vector on error).
std::vector<std::uint8_t> make_fixture(const std::string& path) {
  using namespace c2d::dwarf;
  // DWARF4, address size 8.  Table 0:
  //   1 = compile_unit (children) {name strp, language data2, low_pc addr}
  //   2 = base_type       {name strp, encoding data1, byte_size data1}
  //   3 = structure_type  (children) {name strp, byte_size data1}
  //   4 = member          {name strp, type ref4, data_member_location data1}
  //   5 = typedef         {name strp, type ref4}
  std::vector<AbbrevSpec> specs = {
      {1, tag::kCompileUnit, true,
       {{aat::kName, form::kStrp}, {aat::kLanguage, form::kData2}, {aat::kLowPc, form::kAddr}}},
      {2, tag::kBaseType, false,
       {{aat::kName, form::kStrp}, {aat::kEncoding, form::kData1}, {aat::kByteSize, form::kData1}}},
      {3, tag::kStructureType, true,
       {{aat::kName, form::kStrp}, {aat::kByteSize, form::kData1}}},
      {4, tag::kMember, false,
       {{aat::kName, form::kStrp}, {aat::kType, form::kRef4}, {aat::kDataMemberLocation, form::kData1}}},
      {5, tag::kTypedef, false, {{aat::kName, form::kStrp}, {aat::kType, form::kRef4}}},
  };
  c2d::test::DebugSections dbg;
  dbg.abbrev = build_abbrev(specs);

  Bytes str;
  const std::uint32_t off_unit = static_cast<std::uint32_t>(str.size());
  str.cstr("test.cu");
  const std::uint32_t off_int = static_cast<std::uint32_t>(str.size());
  str.cstr("int");
  const std::uint32_t off_point = static_cast<std::uint32_t>(str.size());
  str.cstr("Point");
  const std::uint32_t off_x = static_cast<std::uint32_t>(str.size());
  str.cstr("x");
  const std::uint32_t off_pt = static_cast<std::uint32_t>(str.size());
  str.cstr("PointAlias");
  dbg.str = str.bytes();

  // The compile_unit has children, so its subtree is closed by the null entry
  // emitted after the typedef below -- not here.
  std::vector<RawDie> dies = {
      {1, {}},  // compile_unit
  };
  {
    Bytes p;
    p.u32(off_unit);
    p.u16(0x0c);
    p.u64(0x1000);
    dies[0].payload = p.bytes();
  }

  // base_type "int": encoding 0x05 (signed), byte_size 4
  {
    Bytes p;
    p.u32(off_int);
    p.u8(0x05);
    p.u8(4);
    dies.push_back({2, p.bytes()});
  }
  // structure_type "Point" (size 8)
  {
    Bytes p;
    p.u32(off_point);
    p.u8(8);
    dies.push_back({3, p.bytes()});
  }
  // member "x": type -> ref to the base_type DIE (patched below), offset 0
  {
    Bytes p;
    p.u32(off_x);
    p.u32(0);  // placeholder for the unit-relative ref
    p.u8(0);
    dies.push_back({4, p.bytes()});
  }
  dies.push_back({0, {}});  // close structure_type
  // typedef "PointAlias" -> ref to structure_type DIE
  {
    Bytes p;
    p.u32(off_pt);
    p.u32(0);  // placeholder
    dies.push_back({5, p.bytes()});
  }
  dies.push_back({0, {}});  // close compile_unit

  dbg.info = build_unit_v4(0, 8, dies);

  // Patch the two unit-relative refs. Each is 4 bytes at a known position:
  // recompute by re-walking the layout we just built.
  // Hand-computed DIE layout (code uleb + payload):
  //   compile_unit  @11  (1 + 14) -> 26
  //   base_type     @26  (1 +  6) -> 33
  //   structure_type@33  (1 +  5) -> 39
  //   member        @39  (1 +  9) -> 49 ; name@40, type@44
  //   null          @49  (1)      -> 50
  //   typedef       @50  (1 +  8) -> 59 ; name@51, type@55
  const std::size_t member_type_off = 44;  // DW_AT_type of the member
  const std::size_t typedef_type_off = 55; // DW_AT_type of the typedef
  for (int i = 0; i < 4; ++i) {
    dbg.info[member_type_off + i] = static_cast<std::uint8_t>(26 >> (8 * i));
    dbg.info[typedef_type_off + i] = static_cast<std::uint8_t>(33 >> (8 * i));
  }

  std::vector<std::uint8_t> file = c2d::test::build_elf(dbg);
  c2d::test::write_file(path, file);
  return file;
}

}  // namespace

C2D_TEST(EndToEnd, ParsesSyntheticElfAndWalksDies) {
  const std::string path = c2d::test::temp_path("c2d_fixture_basic.so");
  make_fixture(path);

  std::string err;
  elf::ElfFile f;
  EXPECT_TRUE(f.open(path, &err));
  EXPECT_TRUE(f.is_64bit());
  EXPECT_TRUE(f.is_little_endian());
  EXPECT_EQ(f.sections().size(), std::size_t{5});
  EXPECT_TRUE(f.find_section(".debug_info") != nullptr);
  EXPECT_TRUE(f.find_section(".debug_abbrev") != nullptr);
  EXPECT_TRUE(f.find_section(".debug_str") != nullptr);
  EXPECT_TRUE(f.find_section(".nosuchsection") == nullptr);

  dwarf::DwarfContext ctx(f);
  EXPECT_TRUE(ctx.sections().has_info());
  EXPECT_EQ(ctx.unit_count(), std::uint64_t{1});

  dwarf::UnitHeader h{};
  EXPECT_TRUE(ctx.unit_header(0, h, &err));
  EXPECT_EQ(h.version, 4);
  EXPECT_EQ(h.address_size, 8);

  const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
  EXPECT_TRUE(ab != nullptr);
  EXPECT_EQ(ab->size(), std::size_t{5});

  dwarf::UnitWalker w(ctx.info(), h, ab);
  w.reset();
  dwarf::Die d;

  std::vector<std::uint32_t> tags;
  std::vector<unsigned> depths;
  std::vector<std::string> names;
  std::vector<std::uint64_t> member_type_refs;
  while (w.next(d)) {
    tags.push_back(d.tag());
    depths.push_back(d.depth());
    dwarf::AttrValue v;
    if (d.attr(dwarf::aat::kName, v) && v.form == dwarf::form::kStrp) {
      names.emplace_back(ctx.str_at(v.u64));
    }
    if (d.tag() == dwarf::tag::kMember && d.attr(dwarf::aat::kType, v)) {
      member_type_refs.push_back(dwarf::DwarfContext::resolve_unit_ref(h, v.u64));
    }
  }

  EXPECT_EQ(w.dies_seen(), std::uint64_t{5});
  ASSERT_EQ_SIZE(tags, 5);
  EXPECT_EQ(tags[0], static_cast<std::uint32_t>(dwarf::tag::kCompileUnit));
  EXPECT_EQ(tags[1], static_cast<std::uint32_t>(dwarf::tag::kBaseType));
  EXPECT_EQ(tags[2], static_cast<std::uint32_t>(dwarf::tag::kStructureType));
  EXPECT_EQ(tags[3], static_cast<std::uint32_t>(dwarf::tag::kMember));
  EXPECT_EQ(tags[4], static_cast<std::uint32_t>(dwarf::tag::kTypedef));
  EXPECT_EQ(depths[0], 0u);
  EXPECT_EQ(depths[1], 1u);
  EXPECT_EQ(depths[2], 1u);
  EXPECT_EQ(depths[3], 2u);  // member is inside structure_type
  EXPECT_EQ(depths[4], 1u);

  ASSERT_EQ_SIZE(names, 5);
  EXPECT_STREQ(names[0], "test.cu");
  EXPECT_STREQ(names[1], "int");
  EXPECT_STREQ(names[2], "Point");
  EXPECT_STREQ(names[3], "x");
  EXPECT_STREQ(names[4], "PointAlias");

  // The member's DW_AT_type must resolve to the base_type DIE, proving that
  // unit-relative references are computed as unit_offset + value.
  ASSERT_EQ_SIZE(member_type_refs, 1);
  EXPECT_EQ(member_type_refs[0], std::uint64_t{26});
}

C2D_TEST(EndToEnd, WalksMultipleUnitsAndHonoursScanLimits) {
  // Two units sharing one abbreviation table, concatenated.
  using namespace c2d::dwarf;
  std::vector<AbbrevSpec> specs = {{1, tag::kCompileUnit, false,
                                   {{aat::kName, form::kStrp}}}};
  c2d::test::DebugSections dbg;
  dbg.abbrev = build_abbrev(specs);
  Bytes str;
  str.cstr("a");
  str.cstr("b");
  dbg.str = str.bytes();

  Bytes p1;
  p1.u32(0);
  Bytes p2;
  p2.u32(2);
  std::vector<RawDie> d1 = {{1, p1.bytes()}};
  std::vector<RawDie> d2 = {{1, p2.bytes()}};
  auto u1 = build_unit_v4(0, 8, d1);
  auto u2 = build_unit_v4(0, 8, d2);
  dbg.info = u1;
  dbg.info.insert(dbg.info.end(), u2.begin(), u2.end());

  const std::string path = c2d::test::temp_path("c2d_fixture_two.so");
  c2d::test::write_file(path, c2d::test::build_elf(dbg));

  std::string err;
  elf::ElfFile f;
  EXPECT_TRUE(f.open(path, &err));
  dwarf::DwarfContext ctx(f);
  EXPECT_EQ(ctx.unit_count(), std::uint64_t{2});

  std::vector<std::uint64_t> names;
  dwarf::DwarfContext::UnitIterator it(ctx);
  dwarf::UnitHeader h;
  while (it.next(h, &err)) {
    dwarf::UnitWalker w(ctx.info(), h, ctx.abbrev_table(h, &err));
    w.reset();
    dwarf::Die d;
    while (w.next(d)) {
      dwarf::AttrValue v;
      if (d.attr(aat::kName, v)) names.push_back(v.u64);
    }
  }
  ASSERT_EQ_SIZE(names, 2);
  EXPECT_EQ(names[0], std::uint64_t{0});
  EXPECT_EQ(names[1], std::uint64_t{2});

  // max_units must bound the iteration.
  dwarf::ScanLimits lim;
  lim.max_units = 1;
  dwarf::DwarfContext::UnitIterator it2(ctx, lim);
  int n = 0;
  while (it2.next(h, &err)) ++n;
  EXPECT_EQ(n, 1);
}

C2D_TEST(EndToEnd, RejectsGarbageInsteadOfCrashing) {
  // Every one of these is a malformed input; the contract is "report an error",
  // never crash or read out of bounds.
  const std::vector<std::vector<std::uint8_t>> bad = {
      {},
      {0x01, 0x02, 0x03},
      {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  for (const auto& bytes : bad) {
    dwarf::UnitHeader h{};
    std::string err;
    const bool ok = dwarf::parse_unit_header(util::ByteView(bytes.data(), bytes.size()), 0,
                                             util::Endian::Little, h, &err);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
  }
}

C2D_TEST(EndToEnd, ElfRejectsNonElfInput) {
  const std::string path = c2d::test::temp_path("c2d_not_an_elf.bin");
  const std::vector<std::uint8_t> junk(256, 0x41);
  EXPECT_TRUE(c2d::test::write_file(path, junk));
  elf::ElfFile f;
  std::string err;
  EXPECT_FALSE(f.open(path, &err));
  EXPECT_FALSE(err.empty());
}
