// SPDX-License-Identifier: MIT
// C# emitter, reproducing the IL2CPP-dumper layout.
//
// Structure reproduced from the reference:
//   namespace Dump
//   {
//       public static class Enums    { public static class X { public const long M = v; } }
//       public class X               { public const int SizeOf = 0x..;
//                                       public T name; // 0xOFFSET }
//       public static class Functions{ public const long name = 0xADDR; // name }
//       public static class Globals  { public const long name = 0xADDR; // name }
//   }
//
// Indentation is 4 spaces per level and lines are CRLF-terminated, matching the
// reference byte for byte in those respects.
#pragma once

#include <cstdio>
#include <string>
#include <string_view>

#include "c2d/ir/model.h"

namespace c2d::output {

struct EmitOptions {
  /// Binary name recorded in the header comment.
  std::string target_name = "unknown";
  std::string arch = "AArch64";
  /// Cap on emitted lines per section (0 = unlimited). Used by tests.
  std::uint64_t max_lines = 0;
  /// Include the `// Bases:` line for classes with a base class. On by default
  /// in the IL2CPP style, which is where the information is useful.
  bool emit_bases = true;
  /// Synthesise `field_XXXX` entries so the fields tile the whole object.
  bool pad_layout = true;
  /// Emit member functions.
  bool emit_methods = true;
  /// Emitted when the model was built without DWARF. Prints the low-accuracy
  /// banner and tags every record with its provenance.
  bool inferred = false;
  /// Counts shown in the banner, so a reader can size the problem without
  /// running anything.
  std::uint64_t named_functions = 0;
  std::uint64_t unnamed_functions = 0;
  std::uint64_t vtable_slots = 0;
  std::uint64_t classes_from_rtti = 0;
  /// Longest symbol name emitted verbatim. Longer names are truncated and given
  /// an `__0xADDR` suffix so the identifier stays unique, which is what keeps
  /// the RTTI section of the dump readable (and bounded in size).
  std::size_t max_name = 180;
};

struct EmitStats {
  std::uint64_t enums = 0;
  std::uint64_t enumerators = 0;
  std::uint64_t classes = 0;
  std::uint64_t structs = 0;
  std::uint64_t unions = 0;
  std::uint64_t fields = 0;
  std::uint64_t functions = 0;
  std::uint64_t globals = 0;
  std::uint64_t methods = 0;
  std::uint64_t params = 0;
  std::uint64_t padded_fields = 0;
  std::uint64_t with_bases = 0;
  std::uint64_t lines = 0;
  std::uint64_t bytes = 0;
};

/// Writes the IL2CPP-dumper style dump. `out` is left open; the caller owns it.
void emit_il2cpp(const ir::Model& model, std::FILE* out, const EmitOptions& opts,
                 EmitStats* stats = nullptr);

}  // namespace c2d::output
