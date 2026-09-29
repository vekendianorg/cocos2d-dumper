// SPDX-License-Identifier: MIT
// Generic type-name and symbol normalisation for managed-style C# output.
//
// This is deliberately a *presentation* layer: the model keeps the DWARF type
// graph exactly as read, and everything here is a pure string rewrite applied
// at emit time. Keeping it out of the model means the normalisation rules can
// be tuned without invalidating the reconstruction, and every rule is
// table-driven rather than specific to any one class.
//
// Rules are all generic:
//   * vendor standard-library namespaces (std::__ndk1, std::__1) are dropped
//   * std::vector<T>            -> List<T>
//   * std::basic_string<...>    -> string
//   * smart pointers            -> the pointee
//   * std::allocator<...>      -> dropped (allocator noise)
//   * google::protobuf::        -> dropped, and internal:: too
//   * reference/pointer syntax is removed in method signatures, where it is
//     noise, and kept in fields, where it is real layout information
#pragma once

#include <string>
#include <string_view>

namespace c2d::output {

/// Rewrites a rendered DWARF type name into managed-style C#.
std::string normalize_type(std::string_view cxx);

/// As `normalize_type`, but additionally drops pointer and reference markers.
/// Used for method signatures and return types, where `&`/`*` on a class type
/// carries no information a managed reader can use.
std::string normalize_signature_type(std::string_view cxx);

/// True when a field is a compiler/ABI artefact that should not be emitted:
/// vtable pointers (`_vptr$X`), or generated protobuf field-number constants
/// (`k<Field>FieldNumber`).
bool is_abi_artifact(std::string_view name);

/// True when a method is compiler-generated and not part of the source API:
/// mangled lambdas/sort helpers and similar, plus destructors of the form the
/// ABI adds. Constructors and destructors are *not* included: they are real.
bool is_generated_method(std::string_view name);

/// Strips ABI tags a producer may append to a name, such as `[abi:cxx11]`.
std::string clean_symbol_name(std::string_view name);

/// Removes vendor namespaces only (`std::__ndk1::`, `google::protobuf::`),
/// without applying the managed remapping. Used for a type's *identity* -- its
/// declaration and base-class names -- where collapsing `vector` to `List` would
/// merge two distinct types under one name.
std::string erase_vendor_namespaces(std::string_view cxx);

}  // namespace c2d::output
