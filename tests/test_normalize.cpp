// SPDX-License-Identifier: MIT
// Rules for the managed-style type and symbol normalisation.
//
// The rules are generic (driven by table, not by class), so they are pinned
// here with the exact shapes DWARF actually produces for libc++ on AArch64.
#include <string>

#include "c2d/output/normalize.h"
#include "test_framework.h"

using c2d::output::clean_symbol_name;
using c2d::output::is_abi_artifact;
using c2d::output::is_generated_method;
using c2d::output::normalize_signature_type;
using c2d::output::normalize_type;

C2D_TEST(Normalize, StripsVendorStandardLibraryNamespaces) {
  // std::__ndk1 is libc++'s inline namespace; it is noise to a reader.
  EXPECT_STREQ(normalize_type("std::__ndk1::deque<int>"), "deque<int>");
  EXPECT_STREQ(normalize_type("std::__1::vector<int>"), "List<int>");
  EXPECT_STREQ(normalize_type("std::pair<int, int>"), "pair<int, int>");
  EXPECT_STREQ(normalize_type("__ndk1::foo"), "foo");
}

C2D_TEST(Normalize, VectorBecomesList) {
  EXPECT_STREQ(normalize_type("std::__ndk1::vector<int>"), "List<int>");
  // The allocator argument is dropped rather than shown to the reader.
  EXPECT_STREQ(normalize_type("std::__ndk1::vector<cocos2d::Node*>"), "List<cocos2d::Node*>");
  EXPECT_STREQ(
      normalize_type("std::__ndk1::vector<cocos2d::Value, std::__ndk1::allocator<cocos2d::Value> >"),
      "List<cocos2d::Value>");
  // Nested containers collapse to managed spellings all the way down.
  EXPECT_STREQ(
      normalize_type("std::__ndk1::vector<std::__ndk1::vector<int> >"), "List<List<int>>");
}

C2D_TEST(Normalize, BasicStringBecomesString) {
  EXPECT_STREQ(normalize_type("std::__ndk1::basic_string<char>"), "string");
  EXPECT_STREQ(
      normalize_type("std::__ndk1::basic_string<char, std::__ndk1::char_traits<char>, "
                     "std::__ndk1::allocator<char> >"),
      "string");
  EXPECT_STREQ(normalize_type("std::basic_string<unsigned short>"), "string");
}

C2D_TEST(Normalize, SmartPointersBecomeThePointee) {
  EXPECT_STREQ(normalize_type("std::__ndk1::shared_ptr<FSEvent>"), "FSEvent");
  EXPECT_STREQ(normalize_type("std::__ndk1::unique_ptr<Node>"), "Node");
  EXPECT_STREQ(normalize_type("std::__ndk1::weak_ptr<Node>"), "Node");
  EXPECT_STREQ(normalize_type("std::__ndk1::shared_ptr<std::__ndk1::vector<int> >"),
               "List<int>");
}

C2D_TEST(Normalize, AllocatorNoiseIsDropped) {
  EXPECT_STREQ(normalize_type("std::__ndk1::allocator<int>"), "");
  EXPECT_STREQ(normalize_type("std::__ndk1::less<int>"), "");
  EXPECT_STREQ(normalize_type("std::__ndk1::char_traits<char>"), "");
  // A dropped argument must not leave a dangling separator behind.
  EXPECT_STREQ(
      normalize_type("std::__ndk1::vector<int, std::__ndk1::allocator<int> >"), "List<int>");
}

C2D_TEST(Normalize, ProtobufTypesLoseTheirNamespace) {
  EXPECT_STREQ(normalize_type("google::protobuf::Message"), "Message");
  EXPECT_STREQ(normalize_type("google::protobuf::internal::FieldDescriptor"),
               "FieldDescriptor");
  EXPECT_STREQ(normalize_type("std::__ndk1::vector<google::protobuf::Message *>"),
               "List<Message*>");
}

C2D_TEST(Normalize, FieldTypesKeepPointerSyntax) {
  // In a field the indirection is real layout information and must survive.
  EXPECT_STREQ(normalize_type("cocos2d::Node*"), "cocos2d::Node*");
  EXPECT_STREQ(normalize_type("cocos2d::Node&"), "cocos2d::Node&");
  EXPECT_STREQ(normalize_type("cocos2d::Node*[4]"), "cocos2d::Node*[4]");
  EXPECT_STREQ(normalize_type("std::__ndk1::vector<int>*"), "List<int>*");
}

C2D_TEST(Normalize, SignaturesDropPointersAndReferences) {
  // In a signature a reference to a class is the managed form already.
  EXPECT_STREQ(normalize_signature_type("Vec3&"), "Vec3");
  EXPECT_STREQ(normalize_signature_type("cocos2d::Node*"), "cocos2d::Node");
  EXPECT_STREQ(normalize_signature_type("std::__ndk1::vector<int>*"), "List<int>");
  // Scalars keep their indirection, where it is meaningful.
  EXPECT_STREQ(normalize_signature_type("int*"), "int*");
}

C2D_TEST(Normalize, UnknownAndEmptyInputsAreSafe) {
  EXPECT_STREQ(normalize_type(""), "");
  EXPECT_STREQ(normalize_type("unknown"), "unknown");
  EXPECT_STREQ(normalize_type("List<>"), "List<>");
}

C2D_TEST(Normalize, AbiArtifactFieldsAreIdentified) {
  EXPECT_TRUE(is_abi_artifact("_vptr$AccountObserver"));
  EXPECT_TRUE(is_abi_artifact("_vptr_AdObserver"));
  EXPECT_TRUE(is_abi_artifact("kVersionFieldNumber"));
  EXPECT_TRUE(is_abi_artifact("kNameFieldNumber"));
  // Real fields must never be classified as artefacts.
  EXPECT_FALSE(is_abi_artifact("m_version"));
  EXPECT_FALSE(is_abi_artifact("version"));
  EXPECT_FALSE(is_abi_artifact("fieldNumberOfThing"));
  EXPECT_FALSE(is_abi_artifact("m_myFieldNumber"));
  EXPECT_FALSE(is_abi_artifact(""));
}

C2D_TEST(Normalize, GeneratedMethodsAreIdentified) {
  EXPECT_TRUE(is_generated_method("_ZZ4mainENK3$_0clEv"));
  EXPECT_TRUE(is_generated_method("__introsort_std__ClassicAlgPolicy"));
  EXPECT_TRUE(is_generated_method("__sort3_abi_ne180000_std__"));
  EXPECT_TRUE(is_generated_method("non-virtual thunk to Foo::bar()"));
  EXPECT_TRUE(is_generated_method("vtable for Foo"));
  EXPECT_TRUE(is_generated_method("typeinfo for Foo"));
  // Real API members must survive.
  EXPECT_FALSE(is_generated_method(".ctor"));
  EXPECT_FALSE(is_generated_method("~Sprite"));
  EXPECT_FALSE(is_generated_method("init"));
  EXPECT_FALSE(is_generated_method("adjustParticlePos"));
  EXPECT_FALSE(is_generated_method("Finalize"));
  EXPECT_FALSE(is_generated_method(""));
}

C2D_TEST(Normalize, AbiTagsAreStrippedFromNames) {
  EXPECT_STREQ(clean_symbol_name("getName[abi:cxx11]"), "getName");
  EXPECT_STREQ(clean_symbol_name("plain"), "plain");
  EXPECT_STREQ(clean_symbol_name("index[abi:cxx11]_M_"), "index_M_");
  // An operator subscript is not a tag and must survive.
  EXPECT_STREQ(clean_symbol_name("operator[abi:cxx11]"), "operator[]");
  EXPECT_STREQ(clean_symbol_name("a[abi:cxx11]b"), "ab");
}

C2D_TEST(Normalize, FunctionTypesCarryNormalisedParameters) {
  // A std::function field as it appears in the dump: the parameter list holds
  // commas and template arguments of its own, so the splitter has to respect
  // both kinds of nesting.
  const std::string in =
      "std::function<void(cocostudio::Bone *, const std::__ndk1::basic_string<char, "
      "std::__ndk1::char_traits<char>, std::__ndk1::allocator<char> > &, int, int)>";
  const std::string out = normalize_type(in);
  // The inner string must have collapsed no matter how deep it was nested.
  EXPECT_TRUE(out.find("std::__ndk1") == std::string::npos);
  EXPECT_TRUE(out.find("allocator<") == std::string::npos);
  EXPECT_TRUE(out.find("basic_string") == std::string::npos);
  EXPECT_TRUE(out.find("string") != std::string::npos);
  EXPECT_TRUE(out.find("function<") == 0);
  // Field form keeps the indirection, since it is real layout information;
  // spacing around it is tidied but nothing is invented or lost.
  EXPECT_STREQ(out, "function<void(cocostudio::Bone*, string&, int, int)>");
  // Signature form drops it: a managed reader gains nothing from `&` here.
  EXPECT_STREQ(normalize_signature_type(in),
               "function<void(cocostudio::Bone, string, int, int)>");
}

C2D_TEST(Normalize, TemplateIdInAQualifiedName) {
  // `FSEvent<...>::Handler` puts the template in the middle of the name, which
  // a parser that only looks for a trailing `<...>` gets wrong.
  const std::string in =
      "FSEvent<GameController::KeyCode, const std::__ndk1::basic_string<char, "
      "std::__ndk1::char_traits<char>, std::__ndk1::allocator<char> > &, bool &>::EventHandler*";
  EXPECT_STREQ(normalize_type(in),
               "FSEvent<GameController::KeyCode, string&, bool&>::EventHandler*");
  EXPECT_STREQ(normalize_signature_type(in),
               "FSEvent<GameController::KeyCode, string, bool>::EventHandler");
}
