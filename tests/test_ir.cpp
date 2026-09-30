// SPDX-License-Identifier: MIT
// Tests for the intermediate representation, the builder and the emitter.
//
// These run against hand-built synthetic DWARF so the type-reconstruction rules
// (qualifiers, arrays, typedefs, inheritance, offsets, deduplication) are
// covered without the 583 MB input.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stellar/dwarf/dwarf_context.h"
#include "stellar/ir/build.h"
#include "stellar/output/emit.h"
#include "fixture_builder.h"
#include "test_framework.h"

using namespace stellar;

namespace {

/// A stand-in string pool, so Model::str() can be tested without an ELF.
struct StrPool {
  // .debug_str always begins with a NUL, so offset 0 is the empty string.
  std::string blob = std::string(1, '\0');
  std::vector<std::pair<std::uint32_t, std::size_t>> entries;
  std::uint32_t add(std::string_view s) {
    const auto off = static_cast<std::uint32_t>(blob.size());
    blob.append(s);
    blob.push_back('\0');
    return off;
  }
};
StrPool g_pool;

std::string_view pool_str(void* self, std::uint32_t off) {
  auto* p = static_cast<StrPool*>(self);
  if (off >= p->blob.size()) return {};
  const char* start = p->blob.data() + off;
  const std::size_t max = p->blob.size() - off;
  return std::string_view(start, ::strnlen(start, max));
}

/// Builds a small model directly (no DWARF) for unit-testing the IR itself.
ir::Model make_test_model() {
  g_pool.blob.clear();
  ir::Model m;
  m.string_self = &g_pool;
  m.string_fn = &pool_str;
  m.reserve_type_slots(16);

  const auto add_type = [&m](ir::TypeKind k, std::uint32_t name, std::uint32_t elem,
                              std::uint64_t size) {
    ir::Type t;
    t.kind = k;
    t.name_off = name;
    t.elem = elem;
    t.size = size;
    t.size_done = size != 0 ? 1 : 0;
    t.transparent = (k == ir::TypeKind::kConst || k == ir::TypeKind::kVolatile ||
                     k == ir::TypeKind::kTypedef)
                        ? 1
                        : 0;
    const auto idx = static_cast<std::uint32_t>(m.types.size());
    m.types.push_back(t);
    return idx;
  };

  const std::uint32_t t_int = add_type(ir::TypeKind::kBase, g_pool.add("int"), ir::kNoType, 4);
  m.types[t_int].encoding = 5;  // DW_ATE_signed
  const std::uint32_t t_uint = add_type(ir::TypeKind::kBase, g_pool.add("unsigned int"),
                                        ir::kNoType, 4);
  m.types[t_uint].encoding = 7;  // DW_ATE_unsigned
  const std::uint32_t t_char = add_type(ir::TypeKind::kBase, g_pool.add("char"), ir::kNoType, 1);
  m.types[t_char].encoding = 8;  // DW_ATE_unsigned_char
  const std::uint32_t t_float = add_type(ir::TypeKind::kBase, g_pool.add("float"), ir::kNoType, 4);
  m.types[t_float].encoding = 4;  // DW_ATE_float
  const std::uint32_t t_pint = add_type(ir::TypeKind::kPointer, 0, t_int, 8);
  const std::uint32_t t_cint = add_type(ir::TypeKind::kConst, 0, t_int, 4);
  const std::uint32_t t_alias = add_type(ir::TypeKind::kTypedef, g_pool.add("MyInt"), t_int, 4);
  const std::uint32_t t_arr = add_type(ir::TypeKind::kArray, 0, t_char, 0);
  m.types[t_arr].count = 4;

  // A struct with two fields.
  ir::ClassDef cd;
  cd.name_off = g_pool.add("Point");
  cd.size = 8;
  cd.kind = 0;  // struct
  cd.first_field = 0;
  cd.field_count = 2;
  const std::uint32_t t_point = add_type(ir::TypeKind::kStruct, cd.name_off, ir::kNoType, 8);
  m.types[t_point].def = 0;
  m.classes.push_back(cd);
  ir::Field f;
  f.name_off = g_pool.add("x");
  f.type = t_int;
  f.offset = 0;
  f.offset_known = 1;
  m.fields.push_back(f);
  f.name_off = g_pool.add("y");
  f.type = t_float;
  f.offset = 4;
  f.offset_known = 1;
  m.fields.push_back(f);
  (void)t_pint;
  (void)t_cint;
  (void)t_alias;
  (void)t_arr;
  (void)t_uint;
  return m;
}

}  // namespace

STELLAR_TEST(Ir, TypeIndexIsAnOpenAddressedTable) {
  ir::Model m;
  m.reserve_type_slots(4);
  ir::Type t;
  t.kind = ir::TypeKind::kBase;
  EXPECT_TRUE(m.index_type(100, 0));
  EXPECT_TRUE(m.index_type(200, 1));
  EXPECT_EQ(m.lookup_type(100), 0u);
  EXPECT_EQ(m.lookup_type(200), 1u);
  EXPECT_EQ(m.lookup_type(300), ir::kNoType);  // absent
  EXPECT_EQ(m.lookup_type(0), ir::kNoType);
}

STELLAR_TEST(Ir, SizeOfInfersThroughQualifiersTypedefsAndArrays) {
  ir::Model m = make_test_model();
  // base int = 4, const int = 4, pointer = 8, char[4] = 4
  for (std::size_t i = 0; i < m.types.size(); ++i) (void)m.size_of(static_cast<std::uint32_t>(i));
  std::uint32_t cint = ir::kNoType, p = ir::kNoType, arr = ir::kNoType, alias = ir::kNoType;
  for (std::size_t i = 0; i < m.types.size(); ++i) {
    const ir::Type& t = m.types[i];
    if (t.kind == ir::TypeKind::kConst && t.elem != ir::kNoType) cint = static_cast<std::uint32_t>(i);
    if (t.kind == ir::TypeKind::kPointer) p = static_cast<std::uint32_t>(i);
    if (t.kind == ir::TypeKind::kArray) arr = static_cast<std::uint32_t>(i);
    if (t.kind == ir::TypeKind::kTypedef) alias = static_cast<std::uint32_t>(i);
  }
  EXPECT_TRUE(cint != ir::kNoType && p != ir::kNoType && arr != ir::kNoType && alias != ir::kNoType);
  EXPECT_EQ(m.size_of(cint), std::uint64_t{4});
  EXPECT_EQ(m.size_of(p), std::uint64_t{8});
  EXPECT_EQ(m.size_of(arr), std::uint64_t{4});
  EXPECT_EQ(m.size_of(alias), std::uint64_t{4});
}

STELLAR_TEST(Ir, TypeNameRendersCSharpStyleNames) {
  ir::Model m = make_test_model();
  struct Case {
    ir::TypeKind kind;
    std::uint32_t elem;
    std::uint64_t count;
    const char* want;
  };
  const Case cases[] = {
      {ir::TypeKind::kBase, ir::kNoType, 0, "int"},
      {ir::TypeKind::kPointer, 0, 0, "int*"},
      {ir::TypeKind::kLRef, 0, 0, "int&"},
      {ir::TypeKind::kConst, 0, 0, "int"},
  };
  for (const Case& c : cases) {
    ir::Type t;
    t.kind = c.kind;
    t.elem = c.elem;
    const auto idx = static_cast<std::uint32_t>(m.types.size());
    m.types.push_back(t);
    EXPECT_STREQ(m.type_name(idx), c.want);
  }
  // A typedef keeps its own name.
  {
    ir::Type t;
    t.kind = ir::TypeKind::kTypedef;
    t.name_off = g_pool.add("MyInt");
    const auto idx = static_cast<std::uint32_t>(m.types.size());
    m.types.push_back(t);
    EXPECT_STREQ(m.type_name(idx), "MyInt");
  }
  // char[4] renders with its count; an unknown count renders as [].
  ir::Type a;
  a.kind = ir::TypeKind::kArray;
  a.count = 4;
  const std::uint32_t ai = static_cast<std::uint32_t>(m.types.size());
  m.types.push_back(a);
  // An array with no element type still reports its bound.
  EXPECT_STREQ(m.type_name(ai), "unknown[4]");
}

STELLAR_TEST(Ir, DeduplicateKeepsTheMostCompleteDefinition) {
  g_pool.blob.clear();
  ir::Model m;
  m.string_self = &g_pool;
  m.string_fn = &pool_str;
  const std::uint32_t name = g_pool.add("Foo");

  // Three redeclarations: empty, complete, partial.
  for (int i = 0; i < 3; ++i) {
    ir::ClassDef c;
    c.name_off = name;
    c.size = i == 1 ? 16 : 0;
    c.field_count = i == 1 ? 2u : (i == 2 ? 1u : 0u);
    c.first_field = static_cast<std::uint32_t>(i * 2);
    m.classes.push_back(c);
  }
  // A struct and a class that share a name must both survive.
  ir::ClassDef s;
  s.name_off = name;
  s.kind = 2;  // union
  s.field_count = 1;
  s.first_field = 6;
  m.classes.push_back(s);

  m.deduplicate();
  EXPECT_EQ(m.classes[0].hidden, std::uint8_t{1});
  EXPECT_EQ(m.classes[1].hidden, std::uint8_t{0});  // the complete one wins
  EXPECT_EQ(m.classes[2].hidden, std::uint8_t{1});
  EXPECT_EQ(m.classes[3].hidden, std::uint8_t{0});  // different kind survives
  EXPECT_EQ(m.classes[1].size, std::uint64_t{16});
}

STELLAR_TEST(Output, EmitsTheIl2CppShape) {
  ir::Model m = make_test_model();
  m.deduplicate();
  // An enum and a couple of symbols so every section is non-empty.
  ir::EnumDef e;
  e.name_off = g_pool.add("Colour");
  e.size = 4;
  e.first_member = 0;
  e.member_count = 2;
  m.enums.push_back(e);
  ir::EnumMember em;
  em.name_off = g_pool.add("Red");
  em.value = 0;
  m.enum_members.push_back(em);
  em.name_off = g_pool.add("Blue");
  em.value = 7;
  m.enum_members.push_back(em);

  // A member function with no recoverable address: the emitter must still
  // precede it with an explicit location comment.
  {
    ir::Method mem;
    mem.name_off = m.arena_add("mystery");
    mem.ret_type = ir::kNoType;
    mem.addr = 0;
    mem.param_count = 1;
    const auto pi = static_cast<std::uint32_t>(m.params.size());
    m.params.push_back(ir::Param{});
    mem.first_param = pi;
    m.methods.push_back(mem);
    m.classes[0].first_method = static_cast<std::uint32_t>(m.methods.size() - 1);
    m.classes[0].method_count = 1;
  }

  ir::FunctionDef f;
  f.name_off = m.arena_add("do_thing");
  f.addr = 0xABC;
  m.functions.push_back(f);
  ir::GlobalDef g;
  g.name_off = m.arena_add("g_counter");
  g.addr = 0x2000;
  m.globals.push_back(g);

  output::EmitOptions o;
  o.target_name = "unit-test.so";
  const std::string path = stellar::test::temp_path("stellar_emit_test.cs");
  std::FILE* out = std::fopen(path.c_str(), "wb");
  EXPECT_TRUE(out != nullptr);
  output::EmitStats st;
  output::emit_il2cpp(m, out, o, &st);
  std::fclose(out);

  std::string text;
  if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) text.append(buf, n);
    std::fclose(in);
  }

  EXPECT_TRUE(text.find("// Stellar Il2cpp-style dump (Cocos2d ELF/DWARF)\n") != std::string::npos);
  EXPECT_TRUE(text.find("public enum Colour // TypeDefIndex: 1 Size: 0x4 "
                        "UnderlyingType: int\n") != std::string::npos);
  EXPECT_TRUE(text.find("    Blue = 7,\n") != std::string::npos);
  // The reference declares every aggregate as `class`, even DWARF structs.
  EXPECT_TRUE(text.find("public class Point // TypeDefIndex: 2 Size: 0x8 "
                        "Confidence: exact\n") != std::string::npos);
  EXPECT_TRUE(text.find("public struct ") == std::string::npos);
  EXPECT_TRUE(text.find("    // Fields\n") != std::string::npos);
  EXPECT_TRUE(text.find("    // Methods\n") != std::string::npos);
  EXPECT_TRUE(text.find("    public int x; // 0x0\n") != std::string::npos);
  EXPECT_TRUE(text.find("    public float y; // 0x4\n") != std::string::npos);
  // No blank line between consecutive fields, as in the reference.
  EXPECT_TRUE(text.find("    public int x; // 0x0\n    public float y; // 0x4\n") !=
              std::string::npos);
  // ...and the offsets must be non-decreasing down the field list.
  {
    const std::size_t fields_at = text.find("    // Fields\n");
    ASSERT_TRUE(fields_at != std::string::npos);
    const std::size_t methods_at = text.find("    // Methods\n", fields_at);
    ASSERT_TRUE(methods_at != std::size_t{0});
    std::uint64_t prev = 0;
    bool first = true;
    std::size_t pos = fields_at;
    while ((pos = text.find("// 0x", pos)) != std::string::npos && pos < methods_at) {
      const std::uint64_t off =
          std::strtoull(text.c_str() + pos + 5, nullptr, 16);
      if (!first) EXPECT_TRUE(off >= prev);
      prev = off;
      first = false;
      pos += 5;
    }
  }
  EXPECT_TRUE(text.find("public static class Functions // TypeDefIndex: 3\n") !=
              std::string::npos);
  EXPECT_TRUE(text.find("public static class GlobalVariables // TypeDefIndex: 4\n") !=
              std::string::npos);
  EXPECT_TRUE(text.find("    public static IntPtr do_thing; // RVA: 0xabc") !=
              std::string::npos);
  // Every method must carry a location comment, including unresolved ones.
  EXPECT_TRUE(text.find("// RVA: unavailable Offset: unavailable VA: unavailable") !=
              std::string::npos);
  // Generated methods and ABI artefacts are not emitted.
  EXPECT_TRUE(text.find("_vptr") == std::string::npos);
  EXPECT_TRUE(text.find("std::") == std::string::npos);
  EXPECT_TRUE(text.find("allocator<") == std::string::npos);
  // dump_1.73.cs uses lowercase hex throughout, so hex() must not uppercase.
  EXPECT_TRUE(text.find("0xabc") != std::string::npos);
  EXPECT_TRUE(text.find("0xABC") == std::string::npos);
  EXPECT_TRUE(text.find("Size: 0x8 ") != std::string::npos);
  EXPECT_EQ(st.classes + st.structs + st.unions, std::uint64_t{1});
  EXPECT_EQ(st.enums, std::uint64_t{1});
  EXPECT_EQ(st.enumerators, std::uint64_t{2});
  EXPECT_EQ(st.functions, std::uint64_t{1});
  EXPECT_EQ(st.globals, std::uint64_t{1});
  EXPECT_EQ(st.fields, std::uint64_t{2});
}
