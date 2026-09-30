// SPDX-License-Identifier: MIT
// DWARF constants (tags, attributes, forms, unit types, base encodings).
//
// Values follow the DWARF 5 specification tables, which are a superset of and
// numerically stable against DWARF 2-4. The target binary is DWARF 4
// (verified: 1183 units, all version 4), but the reader accepts 2-5 and both
// DWARF32/DWARF64 offset widths so the same code handles other inputs.
#pragma once

#include <cstdint>
#include <string_view>

namespace stellar::dwarf {

// --- Tags ------------------------------------------------------------------
//
// IMPORTANT: DWARF 2/3/4 and DWARF 5 use *different numbers* for several of the
// same tags. The reference binary is DWARF 4, so the values below are the
// DWARF 4 (DWARF3) table, which is what any DWARF <= 4 producer emits:
//
//     0x24 base_type          (DWARF 5 spells this 0x24 too)
//     0x26 const_type         (DWARF 5: 0x1f)
//     0x28 enumerator         (DWARF 5: 0x21)
//     0x21 subrange_type      (DWARF 5: 0x21 as well)
//     0x2e subprogram         (DWARF 5: 0x27)
//     0x34 variable           (DWARF 5: 0x2e)
//     0x35 volatile_type      (DWARF 5: 0x2f)
//
// These were verified empirically against the target by dumping the attribute
// set of every distinct tag it contains (see docs/MILESTONES.md). Using the
// DWARF 5 table silently misclassifies the most common DIEs in the file.
namespace tag {
enum : std::uint32_t {
  kArray = 0x01,
  kClass = 0x02,
  kEntryPoint = 0x03,
  kEnumeration = 0x04,
  kFormalParameter = 0x05,
  kImportedDeclaration = 0x08,
  kLabel = 0x0a,
  kLexicalBlock = 0x0b,
  kMember = 0x0d,
  kPointerType = 0x0f,
  kReferenceType = 0x10,
  kCompileUnit = 0x11,
  kStringType = 0x12,
  kStructureType = 0x13,
  kSubroutineType = 0x15,
  kTypedef = 0x16,
  kUnion = 0x17,
  kUnspecifiedParameters = 0x18,
  kVariant = 0x19,
  kCommonBlock = 0x1a,
  kCommonInclusion = 0x1b,
  kInheritance = 0x1c,
  kInlinedSubroutine = 0x1d,
  kModule = 0x1e,          ///< DWARF2 spelling of base_type
  kPtrToMemberType = 0x1f, ///< DWARF2 spelling of const_type
  kSetType = 0x20,         ///< DWARF2 only
  kSubrangeType = 0x21,    ///< child of array_type; carries DW_AT_count
  kWithStmt = 0x22,
  kAccessDeclaration = 0x23,
  kBaseType = 0x24,
  kCatchBlock = 0x25,
  kConstType = 0x26,
  kConstant = 0x27,
  kEnumerator = 0x28,
  kFileType = 0x29,
  kFriend = 0x2a,
  kNamelist = 0x2b,
  kNamelistItem = 0x2c,
  kPackedType = 0x2d,
  kSubprogram = 0x2e,
  kTemplateTypeParameter = 0x2f,
  kTemplateValueParameter = 0x30,
  kThrownType = 0x31,
  kTryBlock = 0x32,
  kVariantPart = 0x33,
  kVariable = 0x34,
  kVolatileType = 0x35,
  kDwarfProcedure = 0x36,
  kRestrictType = 0x37,
  kInterfaceType = 0x38,
  kNamespace = 0x39,
  kImportedModule = 0x3a,
  kUnspecifiedType = 0x3b,
  kPartialUnit = 0x3c,
  kImportedDeclaration2 = 0x3d,
  kMutated = 0x3e,
  kCondition = 0x3f,
  kSharedType = 0x40,
  kTypeUnit = 0x41,
  kAtomicType = 0x42,
  kCallSite = 0x43,
  kCallSiteParameter = 0x44,
  kSkeletonUnit = 0x45,
  kImmutableType = 0x46,
  // Vendor / GNU extensions emitted by clang and GCC.
  kTemplateTypeParameterExt = 0x4106,
  kTemplateValueParameterExt = 0x4107,
  kGnuTemplateTemplateParameter = 0x4108,
  kGnuTemplateParameterPack = 0x4109,
  kGnuFormalParameterPack = 0x410a,
  kGnuCallSite = 0x410b,
  kGnuCallSiteParameter = 0x410c,
  // DWARF 5 renames (kept so version-5 input still resolves by name).
  kDw5Enumerator = 0x21,       // would collide with subrange_type in DWARF4
  kDw5BaseType = 0x1e,
};
}  // namespace tag


// --- Attributes (DWARF5 Table 7.5) -----------------------------------------
namespace aat {  // "attr" is a C++20 keyword
enum : std::uint32_t {
  kSiblings = 0x01, kLocation = 0x02, kName = 0x03, kOrdering = 0x09,
  kByteSize = 0x0b, kBitOffset = 0x0c, kBitSize = 0x0d, kStmtList = 0x10,
  kLowPc = 0x11, kHighPc = 0x12, kLanguage = 0x13, kMember = 0x14,
  kDiscriminant = 0x15, kDiscrValue = 0x16, kVisibility = 0x17, kImport = 0x18,
  kStringLength = 0x19, kCommonReference = 0x1a, kCompDir = 0x1b,
  kConstValue = 0x1c, kContainingType = 0x1d, kDefaultValue = 0x1e,
  kInline = 0x20, kIsOptional = 0x21, kLowerBound = 0x22, kProducer = 0x25,
  kPrototyped = 0x27, kReturnAddr = 0x2a, kStartScope = 0x2c, kBitStride = 0x2e,
  kUpperBound = 0x2f, kAbstractOrigin = 0x31, kAccessibility = 0x32,
  kAddressClass = 0x33, kArtificial = 0x34, kBaseTypes = 0x35,
  kCallingConvention = 0x36, kCount = 0x37, kDataMemberLocation = 0x38,
  kDeclColumn = 0x39, kDeclFile = 0x3a, kDeclLine = 0x3b, kDeclaration = 0x3c,
  kDiscriminantList = 0x3d, kEncoding = 0x3e, kExternal = 0x3f,
  kFrameBase = 0x40, kFriend = 0x41, kIdentifierCase = 0x42, kMacroInfo = 0x43,
  kNamelistItem = 0x44, kPriority = 0x45, kSegment = 0x46, kSpecification = 0x47,
  kStaticLink = 0x48, kType = 0x49, kUseLocation = 0x4a,
  kVariableParameter = 0x4b, kVirtuality = 0x4c, kVtableElemLocation = 0x4d,
  kAllocated = 0x4e, kAssociated = 0x4f, kDataLocation = 0x50,
  kByteStride = 0x51, kEntryPc = 0x52, kUseUTF8 = 0x53, kExtension = 0x54,
  kRanges = 0x55, kTrampoline = 0x56, kCallColumn = 0x57, kCallFile = 0x58,
  kCallLine = 0x59, kDescription = 0x5a, kBinaryScale = 0x5b,
  kDecimalScale = 0x5c, kSmall = 0x5d, kDecimalSign = 0x5e, kDigitCount = 0x5f,
  kPictureString = 0x60, kMutable = 0x61, kThreadscaled = 0x62,
  kExplicit = 0x63, kObjectPointer = 0x64, kEndianity = 0x65, kElemental = 0x66,
  kPure = 0x67, kRecursive = 0x68, kSignature = 0x69, kMainSubprogram = 0x6a,
  kDataBitOffset = 0x6b, kConstExpr = 0x6c, kEnumClass = 0x6d, kLinkageName = 0x6e,
  kStringLengthBitSize = 0x6f, kStringLengthByteSize = 0x70, kRank = 0x71,
  kStrOffsetsBase = 0x72, kAddrBase = 0x73, kRnglistsBase = 0x74, kDwoName = 0x76,
  kReference = 0x77, kRvalueReference = 0x78, kMacinfo = 0x79, kCallAllCalls = 0x7a,
  kCallAllSourceCalls = 0x7b, kCallAllTailCalls = 0x7c, kCallReturnPc = 0x7d,
  kCallValue = 0x7e, kCallOrigin = 0x7f, kCallParameter = 0x80, kCallPc = 0x81,
  kCallTailCall = 0x82, kCallTarget = 0x83, kCallTargetClobbered = 0x84,
  kCallDataLocation = 0x85, kCallDataValue = 0x86, kNoreturn = 0x87,
  kAlignment = 0x88, kExportSymbols = 0x89, kDeleted = 0x8a, kDefaulted = 0x8b,
  kObjectiveCCompleteType = 0x8c,
};
}  // namespace aat

// --- Forms (DWARF5 Table 7.5) ----------------------------------------------
namespace form {
enum : std::uint64_t {
  kAddr = 0x01, kBlock2 = 0x03, kBlock4 = 0x04, kData2 = 0x05, kData4 = 0x06,
  kData8 = 0x07, kString = 0x08, kBlock = 0x09, kBlock1 = 0x0a, kData1 = 0x0b,
  kFlag = 0x0c, kSdata = 0x0d, kStrp = 0x0e, kUdata = 0x0f, kRefAddr = 0x10,
  kRef1 = 0x11, kRef2 = 0x12, kRef4 = 0x13, kRef8 = 0x14, kRefUdata = 0x15,
  kIndirect = 0x16, kSecOffset = 0x17, kExprloc = 0x18, kFlagPresent = 0x19,
  kStrx = 0x1a, kAddrx = 0x1b, kRefSup4 = 0x1c, kStrpSup = 0x1d, kData16 = 0x1e,
  kLineStrp = 0x1f, kRefSig8 = 0x20, kImplicitConst = 0x21, kLoclistx = 0x22,
  kRnglistx = 0x23, kStrx1 = 0x24, kStrx2 = 0x25, kStrx3 = 0x26, kStrx4 = 0x27,
  kAddrx1 = 0x28, kAddrx2 = 0x29, kAddrx3 = 0x2a, kAddrx4 = 0x2b,
  kGnuAddrIndex = 0x2c, kGnuStrIndex = 0x2f,
};
}  // namespace form

// --- Unit types (DWARF5) ----------------------------------------------------
namespace utype {
enum : std::uint8_t {
  kCompile = 0x01, kType = 0x02, kPartial = 0x03, kSkeleton = 0x04,
  kSplitCompile = 0x05, kSplitType = 0x06,
  kUnknown = 0xff,  ///< synthesised for DWARF 2-4 (no unit_type field)
};
}  // namespace utype

// --- Base type encodings (DW_AT_encoding) ----------------------------------
namespace ate {
enum : std::uint64_t {
  kNone = 0x00, kAddress = 0x01, kBoolean = 0x02, kComplexFloat = 0x03,
  kFloat = 0x04, kSigned = 0x05, kSignedChar = 0x06, kUnsigned = 0x07,
  kUnsignedChar = 0x08, kImaginaryFloat = 0x09, kPackedDecimal = 0x0a,
  kNumericString = 0x0b, kEdited = 0x0c, kSignedFixed = 0x0d,
  kUnsignedFixed = 0x0e, kDecimalFloat = 0x0f, kUtf = 0x10, kUcs = 0x11,
  kAscii = 0x12,
};
}  // namespace ate

/// Size in bytes of an attribute value of `form`, or false if unknown or
/// context-dependent (DW_FORM_indirect). This is the single source of truth for
/// DIE skipping; keeping it in one place is what keeps the walker in sync.
[[nodiscard]] bool form_size(std::uint64_t f, unsigned address_size,
                             unsigned offset_size, std::uint64_t& size) noexcept;

/// True for forms that reference another DIE *within the same unit*
/// (DW_FORM_ref1/ref2/ref4/ref8/ref_udata).
[[nodiscard]] constexpr bool is_unit_relative_ref(std::uint64_t f) noexcept {
  return f == form::kRef1 || f == form::kRef2 || f == form::kRef4 ||
         f == form::kRef8 || f == form::kRefUdata;
}

/// True for indexed string forms (DW_FORM_strx*) that require .debug_str_offsets.
[[nodiscard]] constexpr bool is_indexed_string(std::uint64_t f) noexcept {
  return f == form::kStrx || f == form::kStrx1 || f == form::kStrx2 ||
         f == form::kStrx3 || f == form::kStrx4 || f == form::kGnuStrIndex;
}

[[nodiscard]] std::string_view tag_name(std::uint32_t tag) noexcept;
[[nodiscard]] std::string_view attr_name(std::uint32_t attr) noexcept;
[[nodiscard]] std::string_view form_name(std::uint64_t form) noexcept;

}  // namespace stellar::dwarf

