// SPDX-License-Identifier: MIT
#include "stellar/dwarf/constants.h"

namespace stellar::dwarf {

bool form_size(std::uint64_t f, unsigned address_size, unsigned offset_size,
               std::uint64_t& size) noexcept {
  const std::uint64_t asz = address_size ? address_size : 8;
  const std::uint64_t osz = offset_size ? offset_size : 4;
  switch (f) {
    // Zero-width forms.
    case form::kFlagPresent:
    case form::kImplicitConst:
      size = 0;
      return true;

    // Fixed-width integer forms.
    case form::kData1:
    case form::kFlag:
    case form::kRef1:
    case form::kStrx1:
    case form::kAddrx1:
      size = 1;
      return true;
    case form::kData2:
    case form::kRef2:
    case form::kStrx2:
    case form::kAddrx2:
      size = 2;
      return true;
    case form::kStrx3:
    case form::kAddrx3:
      size = 3;
      return true;
    case form::kData4:
    case form::kRef4:
    case form::kRefSup4:
    case form::kStrx4:
    case form::kAddrx4:
      size = 4;
      return true;
    case form::kData8:
    case form::kRef8:
    case form::kRefSig8:
      size = 8;
      return true;
    case form::kData16:
      size = 16;
      return true;

    // Widths that depend on the unit.
    case form::kAddr:
      size = asz;
      return true;
    case form::kRefAddr:
    case form::kStrp:
    case form::kSecOffset:
    case form::kStrpSup:
    case form::kLineStrp:
      size = osz;
      return true;

    // LEB128 values: the encoded width varies, so a constant size cannot be
    // reported. Callers treat these as "must be decoded, not skipped".
    case form::kUdata:
    case form::kSdata:
    case form::kRefUdata:
    case form::kStrx:
    case form::kAddrx:
    case form::kGnuAddrIndex:
    case form::kGnuStrIndex:
    case form::kLoclistx:
    case form::kRnglistx:
    case form::kIndirect:
      size = 0;
      return false;

    // Strings and blocks: NUL-terminated or length-prefixed, variable.
    case form::kString:
    case form::kBlock:
    case form::kBlock1:
    case form::kBlock2:
    case form::kBlock4:
    case form::kExprloc:
      size = 0;
      return false;

    default:
      size = 0;
      return false;
  }
}

namespace {

// Name lookup tables. Used for diagnostics, tag histograms and the emitter,
// never on the DIE-walking hot path, so a plain sorted array with a linear
// scan is more than fast enough and keeps each spelling next to its constant.
struct NameEntry {
  std::uint32_t value;
  const char* name;
};

constexpr NameEntry kTags[] = {
    {0x01, "array_type"},          {0x02, "class_type"},
    {0x03, "entry_point"},         {0x04, "enumeration_type"},
    {0x05, "formal_parameter"},    {0x08, "imported_declaration"},
    {0x0a, "label"},               {0x0b, "lexical_block"},
    {0x0d, "member"},              {0x0f, "pointer_type"},
    {0x10, "reference_type"},      {0x11, "compile_unit"},
    {0x12, "string_type"},         {0x13, "structure_type"},
    {0x15, "subroutine_type"},     {0x16, "typedef"},
    {0x17, "union_type"},          {0x18, "unspecified_parameters"},
    {0x1c, "inheritance"},         {0x1d, "inlined_subroutine"},
    {0x1e, "base_type"},           {0x1f, "const_type"},
    {0x21, "subrange_type"},       {0x22, "with_stmt"},
    {0x24, "base_type"},           {0x25, "catch_block"},
    {0x26, "const_type"},          {0x27, "constant"},
    {0x28, "enumerator"},          {0x29, "file_type"},
    {0x2a, "friend"},              {0x2b, "namelist"},
    {0x2d, "packed_type"},         {0x2e, "subprogram"},
    {0x2f, "template_type_parameter"}, {0x30, "template_value_parameter"},
    {0x31, "thrown_type"},         {0x32, "try_block"},
    {0x34, "variable"},            {0x35, "volatile_type"},
    {0x36, "dwarf_procedure"},     {0x37, "restrict_type"},
    {0x38, "interface_type"},      {0x39, "namespace"},
    {0x3a, "imported_module"},     {0x3b, "unspecified_type"},
    {0x3c, "partial_unit"},        {0x3d, "imported_declaration"},
    {0x40, "shared_type"},         {0x41, "type_unit"},
    {0x42, "atomic_type"},         {0x43, "call_site"},
    {0x44, "call_site_parameter"}, {0x45, "skeleton_unit"},
    {0x46, "immutable_type"},
    {0x4106, "template_type_parameter"}, {0x4107, "template_value_parameter"},
    {0x4108, "template_template_parameter"}, {0x4109, "template_parameter_pack"},
    {0x410a, "formal_parameter_pack"}, {0x410b, "call_site"},
    {0x410c, "call_site_parameter"},
};

constexpr NameEntry kAttrs[] = {
    {0x01, "sibling"},   {0x02, "location"},    {0x03, "name"},
    {0x0b, "byte_size"}, {0x0c, "bit_offset"},  {0x0d, "bit_size"},
    {0x10, "stmt_list"}, {0x11, "low_pc"},      {0x12, "high_pc"},
    {0x13, "language"},  {0x14, "member"},      {0x17, "visibility"},
    {0x19, "string_length"}, {0x1a, "common_reference"}, {0x1b, "comp_dir"},
    {0x1c, "const_value"}, {0x1d, "containing_type"}, {0x1e, "default_value"},
    {0x20, "inline"},    {0x22, "lower_bound"}, {0x25, "producer"},
    {0x27, "prototyped"}, {0x2e, "bit_stride"}, {0x2f, "upper_bound"},
    {0x31, "abstract_origin"}, {0x32, "accessibility"}, {0x33, "address_class"},
    {0x34, "artificial"}, {0x35, "base_types"}, {0x36, "calling_convention"},
    {0x37, "count"},     {0x38, "data_member_location"}, {0x39, "decl_column"},
    {0x3a, "decl_file"}, {0x3b, "decl_line"},  {0x3c, "declaration"},
    {0x3e, "encoding"},  {0x3f, "external"},   {0x40, "frame_base"},
    {0x41, "friend"},    {0x47, "specification"}, {0x49, "type"},
    {0x4c, "virtuality"}, {0x4d, "vtable_elem_location"}, {0x50, "data_location"},
    {0x51, "byte_stride"}, {0x52, "entry_pc"},  {0x55, "ranges"},
    {0x57, "call_column"}, {0x58, "call_file"}, {0x59, "call_line"},
    {0x5a, "description"}, {0x61, "mutable"},   {0x67, "pure"},
    {0x68, "recursive"},  {0x69, "signature"},  {0x6c, "const_expr"},
    {0x6d, "enum_class"},  {0x6e, "linkage_name"}, {0x72, "str_offsets_base"},
    {0x73, "addr_base"},  {0x77, "reference"},  {0x78, "rvalue_reference"},
    {0x87, "noreturn"},   {0x88, "alignment"},  {0x8a, "deleted"},
};

constexpr NameEntry kForms[] = {
    {0x01, "addr"},        {0x03, "block2"},  {0x04, "block4"},
    {0x05, "data2"},       {0x06, "data4"},   {0x07, "data8"},
    {0x08, "string"},      {0x09, "block"},   {0x0a, "block1"},
    {0x0b, "data1"},       {0x0c, "flag"},    {0x0d, "sdata"},
    {0x0e, "strp"},        {0x0f, "udata"},   {0x10, "ref_addr"},
    {0x11, "ref1"},        {0x12, "ref2"},    {0x13, "ref4"},
    {0x14, "ref8"},        {0x15, "ref_udata"}, {0x16, "indirect"},
    {0x17, "sec_offset"},  {0x18, "exprloc"}, {0x19, "flag_present"},
    {0x1a, "strx"},        {0x1b, "addrx"},   {0x1c, "ref_sup4"},
    {0x1d, "strp_sup"},    {0x1e, "data16"},  {0x1f, "line_strp"},
    {0x20, "ref_sig8"},    {0x21, "implicit_const"}, {0x22, "loclistx"},
    {0x23, "rnglistx"},    {0x24, "strx1"},   {0x25, "strx2"},
    {0x26, "strx3"},       {0x27, "strx4"},   {0x28, "addrx1"},
    {0x29, "addrx2"},      {0x2a, "addrx3"},  {0x2b, "addrx4"},
    {0x2c, "GNU_addr_index"}, {0x2f, "GNU_str_index"},
};

std::string_view lookup(const NameEntry* table, std::size_t n, std::uint64_t v) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (table[i].value == v) return table[i].name;
    if (table[i].value > v) break;  // tables are sorted ascending
  }
  return "unknown";
}

}  // namespace

std::string_view tag_name(std::uint32_t t) noexcept {
  return lookup(kTags, std::size(kTags), t);
}

std::string_view attr_name(std::uint32_t a) noexcept {
  return lookup(kAttrs, std::size(kAttrs), a);
}

std::string_view form_name(std::uint64_t f) noexcept {
  return lookup(kForms, std::size(kForms), f);
}

}  // namespace stellar::dwarf
