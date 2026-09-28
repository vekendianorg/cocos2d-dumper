// SPDX-License-Identifier: MIT
// Model construction: walks every DIE in the binary and reconstructs a flat
// type graph plus the class, enum and member-function definitions.
//
// Two passes over the DIE stream are used, because DWARF references may point
// forward or backward:
//   Pass A  create a Type node for every type-defining DIE and record
//           offset -> node in an open-addressed table; collect members and
//           enumerators.
//   Pass B  resolve each node's DW_AT_type reference through that table, then
//           infer missing sizes.
//
// Only type-defining DIEs are retained. Members, enumerators and inheritance
// are captured; everything else (the ~92% of DIEs that are function-local) is
// skipped without materialisation, which is what keeps peak memory bounded.
#pragma once

#include <cstdint>
#include <string>

#include "c2d/dwarf/dwarf_context.h"
#include "c2d/dwarf/eh_frame.h"
#include "c2d/ir/model.h"

namespace c2d::ir {

/// DWARF 4 has no standard rvalue-reference tag: clang emits
/// DW_TAG_reference_type for `T&&`, so the IR folds both into a reference. The
/// DWARF 5 value (0x42) collides with DWARF4's atomic_type and is not used.
inline constexpr std::uint32_t kRvalueReferenceTag = 0xffffffffu;

struct BuildOptions {
  /// Stop after this many units (0 = all). Used by the fast tests.
  std::uint64_t max_units = 0;
  /// Include DW_TAG_subprogram DIEs as functions (off by default: the symbol
  /// table gives better names and addresses).
  bool functions_from_dwarf = false;
  /// Emit the symbol table as functions/globals (default true).
  bool symbols = true;
};

struct BuildStats {
  std::uint64_t units = 0;
  std::uint64_t dies = 0;
  std::uint64_t type_nodes = 0;
  std::uint64_t classes = 0;
  std::uint64_t enums = 0;
  std::uint64_t fields = 0;
  std::uint64_t enumerators = 0;
  std::uint64_t methods = 0;
  std::uint64_t params = 0;
  /// Members whose address was recovered from an out-of-line definition.
  std::uint64_t methods_with_addr = 0;
  std::uint64_t unresolved_refs = 0;
  std::uint64_t units_without_abbrev = 0;
  double seconds = 0.0;
};

/// Fills `model` from `ctx`. Returns false only if the DWARF is unusable.
bool build_model(dwarf::DwarfContext& ctx, const BuildOptions& opts, Model& model,
                 BuildStats* stats = nullptr);

/// How much of the output is ground truth.
enum class Confidence {
  kExact,    ///< derived from DWARF
  kInferred, ///< derived from symbols/.eh_frame/RTTI; not ground truth
};

struct DwarflessStats {
  std::uint64_t fdes = 0;
  std::uint64_t functions_named = 0;      ///< name came from the symbol table
  std::uint64_t functions_sub_ = 0;      ///< named sub_<addr> instead
  std::uint64_t globals = 0;
  std::uint64_t classes_from_rtti = 0;
  std::uint64_t vtable_slots = 0;
  double seconds = 0.0;
};

/// Builds what can be recovered from a binary with no DWARF: function ranges
/// from .eh_frame, names from the symbol table, globals, and (where the RTTI
/// is present) class names and vtable layouts.
///
/// This is deliberately *not* called a type reconstruction: field offsets and
/// field types are not recoverable without DWARF, and the model leaves them
/// unknown rather than guessing.
bool build_dwarfless_model(dwarf::DwarfContext& ctx, Model& model, DwarflessStats* stats = nullptr);

}  // namespace c2d::ir
