// SPDX-License-Identifier: MIT
// IL2CPP-dumper style emitter, matching the layout of `dump_1.73.cs`.
//
// Layout reproduced from that file:
//
//   // Namespace: <ns>
//   public enum E // TypeDefIndex: 1 Size: 0x4 UnderlyingType: int
//   { A = 1, B = 3, }
//
//   public class C : Base // TypeDefIndex: 845 Size: 0x320 Confidence: exact
//   {
//       // Bases: ns::Base @ 0x0 public
//       // Fields
//       private int m_x; // 0x8
//       public int field_000C; // 0xc        <- synthesised padding
//
//       // Methods
//       // RVA: 0x1535394 Offset: 0x1535394 VA: 0x1535394
//       public virtual void init() { }
//   }
//
// The padding entries matter: they make the field list tile the whole object,
// so a consumer can walk offsets without inferring the gaps.
#include "stellar/output/emit.h"

#include "stellar/diag/progress.h"
#include "stellar/output/normalize.h"

#include <algorithm>
#include <unordered_map>
#include <cstring>
#include <vector>

namespace stellar::output {
namespace {

using ir::TypeKind;

constexpr std::string_view kHeader1 = "Stellar (Cocos2dcpp Dumper with Il2cpp-style dump)";
constexpr std::string_view kHeader2 =
    "Names, types and offsets come from DWARF; where DWARF has no name,";
constexpr std::string_view kHeader3 =
    "field_OFFSET names a field by its offset and sub_ADDR a function by address.";
constexpr std::string_view kHeader4 =
    "TIER tags record each record's provenance: exact, rtti, sym or infer.";

const char* access_name(std::uint8_t a) {
  switch (a) {
    case 2: return "protected";
    case 3: return "private";
    default: return "public";
  }
}

/// Lowercase hex with the 0x prefix, matching dump_1.73.cs.
std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

/// Lowercase hex, used for field_XXXX padding names.
std::string hex_lower4(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%04llx", static_cast<unsigned long long>(v & 0xffffu));
  return b;
}

std::string ident(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                    (c >= '0' && c <= '9' && i != 0) || c == '.' || c == ':' ||
                    c == '<' || c == '>' || c == ',' || c == '*' || c == '&' ||
                    c == '[' || c == ']' || c == ' ' || c == '(' || c == ')' ||
                    c == '$' || c == '@' || c == '~' || c == '`' || c == '=';
    out.push_back(ok ? static_cast<char>(c) : '_');
  }
  if (out.empty()) return "_";
  if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
  return out;
}

/// A bounded identifier: long mangled names (RTTI, vtables) are truncated and
/// disambiguated with the address, so the dump stays readable instead of being
/// dominated by 800-character lines.
std::string bounded(std::string_view name, std::uint64_t addr, std::size_t limit) {
  std::string s = ident(name);
  if (s.size() <= limit) return s;
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "__0x%llx", static_cast<unsigned long long>(addr));
  const std::size_t keep = limit - std::strlen(suffix);
  return s.substr(0, keep) + suffix;
}

/// "ns::Name" using DWARF's own spelling, which keeps C++ namespaces intact.
std::string qualified(const ir::Model& m, std::uint32_t type) {
  if (type == ir::kNoType || type >= m.types.size()) return "unknown";
  return std::string(m.str(m.types[type].name_off));
}

}  // namespace

void emit_il2cpp(const ir::Model& model, std::FILE* out, const EmitOptions& opts,
                 EmitStats* stats) {
  EmitStats st;
  auto& pr = diag::progress();
  pr.stage("Writing dump");
  pr.declare("Lines", 0, false);
  pr.declare("Enums", 0, false);
  pr.declare("Classes", 0, false);
  pr.declare("Methods", 0, false);
  pr.primary("Lines");
  const char* eol = "\n";
  auto line = [&](const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), out);
    std::fputc('\n', out);
    st.bytes += s.size() + 1;
    ++st.lines;
    // One call site covers the entire dump: writing is the longest stage and
    // the emitted line count is the only meaningful progress it has.
    pr.add("Lines", 1);
  };
  (void)eol;

  line("// " + std::string(kHeader1));
  line("// " + std::string(kHeader2));
  line("// " + std::string(kHeader3));
  line("// " + std::string(kHeader4));
  line("");
  if (opts.inferred) {
    // Deliberately blunt. This file is not ground truth and must not be
    // mistaken for the DWARF-backed dump.
    const std::string bar(77, '=');
    line("// " + bar);
    line("// WARNING: LOW-ACCURACY OUTPUT - NOT GROUND TRUTH");
    line("//");
    line("// This binary has no DWARF (.debug_info absent). Everything below was");
    line("// INFERRED from the symbol table, .eh_frame unwind data and RTTI.");
    line("//");
    line("//   reliable : function names/addresses/sizes, RTTI class names,");
    line("//              vtable slot counts");
    line("//   inferred : nothing else about fields - treat with suspicion");
    line("//   missing  : field offsets and field TYPES are NOT recoverable");
    line("//              without DWARF and are not guessed here");
    line("//");
    line("//   functions named from symbols : " + std::to_string(opts.named_functions));
    line("//   functions named sub_<addr>   : " + std::to_string(opts.unnamed_functions));
    line("//   classes from RTTI            : " + std::to_string(opts.classes_from_rtti));
    line("//   vtable slots recovered        : " + std::to_string(opts.vtable_slots));
    line("//   field offsets recoverable     : 0");
    line("//");
    line("// Use this as reconnaissance, not as a schema. Rebuild unstripped and");
    line("// re-run for an authoritative dump.");
    line("// " + bar);
    line("");
  }

  std::uint32_t type_index = 0;  // TypeDefIndex counter

  // ---- enums, first, in name order ----------------------------------------
  {
    std::vector<const ir::EnumDef*> enums;
    enums.reserve(model.enums.size());
    for (const ir::EnumDef& e : model.enums) {
      if (!e.hidden && e.name_off != 0 && e.member_count != 0) enums.push_back(&e);
    }
    std::sort(enums.begin(), enums.end(), [&model](const ir::EnumDef* a, const ir::EnumDef* b) {
      return model.str(a->name_off) < model.str(b->name_off);
    });
    for (const ir::EnumDef* e : enums) {
      line("// Namespace: ");
      // DWARF gives the enum's byte size; it is what decides the C# underlying
      // type, exactly as an enumerator's declared type would.
      const std::uint64_t size = e->size != 0 ? e->size : 4;
      const char* underlying = size == 1 ? "byte" : size == 8 ? "long" : "int";
      line("public enum " + ident(model.str(e->name_off)) + " // TypeDefIndex: " +
           std::to_string(++type_index) + " Size: " + hex(size) + " UnderlyingType: " +
           underlying);
      line("{");
      const std::uint32_t end = e->first_member + e->member_count;
      for (std::uint32_t i = e->first_member; i < end && i < model.enum_members.size(); ++i) {
        const ir::EnumMember& m = model.enum_members[i];
        if (m.name_off == 0) continue;
        line("    " + ident(model.str(m.name_off)) + " = " + std::to_string(m.value) + ",");
        ++st.enumerators;
      }
      line("}");
      line("");
      ++st.enums;
    }
  }

  // ---- classes, structs, unions --------------------------------------------
  {
    std::vector<const ir::ClassDef*> classes;
    classes.reserve(model.classes.size());
    for (const ir::ClassDef& c : model.classes) {
      if (c.hidden || c.name_off == 0) continue;
      if (model.class_name(c).empty()) continue;
      classes.push_back(&c);
    }
    // Some producers reuse one string-table entry for every unnamed parameter
    // (in this binary it is `_CharT`, ~680k times). A name shared across a large
    // number of distinct methods cannot be a real identifier, so collect those
    // once and treat them as absent.
    std::unordered_map<std::string_view, std::size_t> param_use;
    for (const ir::ClassDef* c : classes) {
      const std::uint32_t mend = c->first_method + c->method_count;
      for (std::uint32_t mi = c->first_method; mi < mend && mi < model.methods.size(); ++mi) {
        const ir::Method& m = model.methods[mi];
        for (std::uint32_t pi = m.first_param;
             pi < m.first_param + m.param_count && pi < model.params.size(); ++pi) {
          const std::string_view n = model.str(model.params[pi].name_off);
          if (!n.empty()) ++param_use[n];
        }
      }
    }
    const auto artefact = [&param_use](std::string_view n) {
      const auto it = param_use.find(n);
      return it != param_use.end() && it->second > 32;
    };

    std::sort(classes.begin(), classes.end(), [&model](const ir::ClassDef* a,
                                                        const ir::ClassDef* b) {
      return model.class_name(*a) < model.class_name(*b);
    });

    for (const ir::ClassDef* c : classes) {
      const std::string_view nm = model.class_name(*c);
      // The reference dump declares every type with `class`, regardless of
      // whether DWARF called it a struct or a union. Matching that keeps the
      // two dumps diffable, so struct/union is not distinguished here.
      const char* kw = "class";
      const std::string base_name =
          opts.emit_bases && c->base != ir::kNoType
              ? erase_vendor_namespaces(qualified(model, c->base))
              : std::string();
      line("// Namespace: ");
      const char* tier = opts.inferred ? " | TIER:rtti" : "";
      if (c->from_arena != 0) {
        // Dwarfless: all we have is the vtable, so report that. Printing its
        // byte size as the object's Size would be a plain lie.
        line(std::string("public class ") + ident(nm) + " // TypeDefIndex: " +
             std::to_string(++type_index) + " | vtable RVA: " + hex(c->vtable_addr) +
             " slots: " + std::to_string(c->vtable_slots) + " (of " +
             std::to_string(c->vtable_words) + " words) | TIER:rtti");
        line("{");
        line("    // No DWARF: field offsets and field types are not recoverable.");
        line("    // Fields");
        line("    // Methods (" + std::to_string(c->vtable_slots) +
             " virtual slots; the Function pointers live in the Functions section)");
        line("}");
        line("");
        ++st.classes;
        continue;
      }
      line(std::string("public ") + kw + " " + ident(erase_vendor_namespaces(nm)) +
           (base_name.empty() ? "" : " : " + ident(base_name)) + " // TypeDefIndex: " +
           std::to_string(++type_index) + " Size: " + hex(c->size) +
           (c->size != 0 ? " Confidence: exact" : " Confidence: partial") + tier);
      line("{");
      if (opts.emit_bases && c->base != ir::kNoType) {
        line(std::string("    // Bases: ") + erase_vendor_namespaces(base_name) + " @ 0x0 public");
        ++st.with_bases;
      }
      line("    // Fields");
      {
        // Ordering follows the reference: statics first (they have no instance
        // offset, so they are sorted by name), then instance fields strictly
        // ascending by offset, which is what makes a layout readable and
        // diffable between library versions. Padding is synthesised against
        // that offset order.
        std::vector<std::uint32_t> statics, members;
        const std::uint32_t raw_end = c->first_field + c->field_count;
        for (std::uint32_t k = c->first_field; k < raw_end && k < model.fields.size(); ++k) {
          (model.fields[k].is_static ? statics : members).push_back(k);
        }
        const auto by_name = [&model](std::uint32_t a, std::uint32_t b) {
          return model.str(model.fields[a].name_off) < model.str(model.fields[b].name_off);
        };
        std::sort(statics.begin(), statics.end(), by_name);
        std::sort(members.begin(), members.end(),
                  [&model](std::uint32_t a, std::uint32_t b) -> bool {
                    const ir::Field& fa = model.fields[a];
                    const ir::Field& fb = model.fields[b];
                    if (fa.offset_known != fb.offset_known) {
                      return fa.offset_known != 0;
                    }
                    if (fa.offset != fb.offset) return fa.offset < fb.offset;
                    return model.str(fa.name_off) < model.str(fb.name_off);
                  });
        std::vector<std::uint32_t> ordered;
        ordered.reserve(statics.size() + members.size());
        for (const std::uint32_t i : statics) ordered.push_back(i);
        for (const std::uint32_t i : members) ordered.push_back(i);
        // Walk the fields in offset order, synthesising padding so the list
        // tiles the object exactly.
        std::uint64_t cursor = 0;
        bool cursor_known = true;
        for (const std::uint32_t i : ordered) {
          const ir::Field& f = model.fields[i];
          if (f.is_static) {
            // Statics live in .data/.bss: their "location" is an address, and
            // the reference lists them alongside the instance fields.
            const std::string_view sname = model.str(f.name_off);
            if (sname.empty() || is_abi_artifact(sname)) continue;
            // Prefer the DWARF address; otherwise recover it from the mangled
            // symbol for this class member. Never print a wrong 0x0.
            std::uint64_t addr = f.offset_known ? f.offset : 0;
            if (addr == 0) {
              std::string key(nm);
              key.append(sname);
              const auto it = model.static_syms.find(key);
              if (it != model.static_syms.end()) addr = it->second;
            }
            line(std::string("    private static readonly ") +
                 normalize_type(model.type_name(f.type)) + " " + ident(sname) + ";" +
                 (addr != 0 ? " // RVA: " + hex(addr) : std::string(" // static")));
            ++st.fields;
            continue;
          }
          const std::string_view fname = model.str(f.name_off);
          if (!f.offset_known) {
            if (fname.empty()) continue;
            line(std::string("    public ") + model.type_name(f.type) + " " + ident(fname) +
                 "; // ?base-relative");
            ++st.fields;
            cursor_known = false;  // the tiling is broken from here on
            continue;
          }
          // Only synthesise padding while the layout is still contiguous and we
          // know the previous member's width; otherwise the gap is an artefact.
          if (is_abi_artifact(fname)) continue;  // vptr / protobuf constants
          if (opts.pad_layout && cursor_known && f.offset > cursor) {
            line("    public int field_" + hex_lower4(cursor) + "; // " + hex(cursor));
            ++st.padded_fields;
          }
          const std::string shown =
              fname.empty() ? ("field_" + hex_lower4(f.offset)) : ident(fname);
          line(std::string("    public ") + normalize_type(model.type_name(f.type)) + " " +
               shown + "; // " + hex(f.offset));
          ++st.fields;
          const std::uint64_t w = model.size_of(f.type);
          if (w != 0) {
            cursor = f.offset + w;
            cursor_known = true;
          } else {
            cursor_known = false;
          }
        }
        if (opts.pad_layout && cursor_known && c->size > cursor) {
          line("    public int field_" + hex_lower4(cursor) + "; // " + hex(cursor));
          ++st.padded_fields;
        }
      }
      line("");
      line("    // Methods");
      if (opts.emit_methods) {
        // Methods are ordered by address, so the list reads as a layout of the
        // code and diffs cleanly between versions.
        std::vector<std::uint32_t> ms;
        const std::uint32_t mend = c->first_method + c->method_count;
        for (std::uint32_t mi = c->first_method; mi < mend && mi < model.methods.size(); ++mi) {
          ms.push_back(mi);
        }
        std::sort(ms.begin(), ms.end(),
                  [&model](std::uint32_t a, std::uint32_t b) -> bool {
                    const ir::Method& ma = model.methods[a];
                    const ir::Method& mb = model.methods[b];
                    if (ma.addr != mb.addr) return ma.addr < mb.addr;
                    return model.str(ma.name_off) < model.str(mb.name_off);
                  });
        for (const std::uint32_t mi : ms) {
          const ir::Method& m = model.methods[mi];
          const std::string_view raw_name = model.str(m.name_off);
          if (raw_name.empty()) continue;
          // A templated method name carries its arguments in the name, so the
          // same normalisation applies to it.
          const std::string cleaned = normalize_type(clean_symbol_name(raw_name));
          if (cleaned.empty()) continue;
          // Mangled lambdas, sort helpers, thunks and vtable plumbing are not
          // part of the source API and only add noise.
          if (is_generated_method(cleaned)) continue;
          const std::string_view mname = cleaned;
          // Every method is preceded by its location. When the address could
          // not be recovered it says so explicitly rather than being omitted,
          // so a reader can always tell "no code" from "not looked up".
          line("");
          line(m.addr != 0
                   ? "    // RVA: " + hex(m.addr) + " Offset: " + hex(m.addr) +
                         " VA: " + hex(m.addr)
                   : std::string("    // RVA: unavailable Offset: unavailable VA: unavailable"));
          // Decide which parameters actually have a usable name. clang shares
          // one abbreviation across named and unnamed parameters, so an unnamed
          // one often carries a stale string-table offset -- in the reference
          // binary that shows up as every parameter called `_CharT`. A name
          // repeated within one signature cannot be right either, so those
          // fall back to argN exactly like a genuinely absent name.
          std::vector<std::string_view> pnames(m.param_count);
          for (std::uint32_t k = 0; k < m.param_count; ++k) {
            const std::uint32_t pi = m.first_param + k;
            if (pi >= model.params.size()) break;
            pnames[k] = model.str(model.params[pi].name_off);
          }
          // A name is believable only if it occurs exactly once in the
          // signature: two parameters cannot share a name, so a repeated one is
          // a stale string-table offset rather than a real identifier.
          const auto usable = [&pnames, &artefact](std::string_view n) {
            if (n.empty() || artefact(n)) return false;
            std::size_t seen = 0;
            for (const std::string_view other : pnames) {
              if (other == n && ++seen > 1) return false;
            }
            return true;
          };
          bool any_unnamed = false;
          for (const std::string_view n : pnames) {
            if (!usable(n)) any_unnamed = true;
          }
          std::string sig = std::string("    ") + access_name(m.access) + " ";
          if (m.is_static) sig += "static ";
          if (m.is_virtual) sig += "virtual ";
          // Constructors and destructors have no DW_AT_type, because C++ has
          // no return type for them; those render as void. A destructor is
          // recognised by its leading tilde, a constructor by DW_AT_name being
          // the class name (already flagged as is_ctor in the builder).
          const bool dtor = !mname.empty() && mname.front() == '~';
          const bool void_return = m.ret_type == ir::kNoType || dtor || m.is_ctor;
          sig += void_return ? "void" : normalize_signature_type(model.type_name(m.ret_type));
          sig += " " + ident(mname) + "(";
          const std::uint32_t pend = m.first_param + m.param_count;
          for (std::uint32_t pi = m.first_param; pi < pend && pi < model.params.size(); ++pi) {
            if (pi != m.first_param) sig += ", ";
            const std::string_view pname = pnames[pi - m.first_param];
            sig += normalize_signature_type(model.type_name(model.params[pi].type));
            // Positional fallback, matching the reference: arg0, arg1, ...
            sig += " " + (usable(pname)
                               ? ident(pname)
                               : ("arg" + std::to_string(pi - m.first_param)));
          }
          sig += ") { }";
          if (any_unnamed) {
            line("    // Parameter names unavailable in ELF/debug data; argN is "
                 "positional fallback.");
          }
          line(sig);
          ++st.methods;
          pr.add("Methods");
          st.params += m.param_count;
        }
      }
      line("}");
      line("");
      if (c->kind == 1) ++st.classes;
      else if (c->kind == 2) ++st.unions;
      else ++st.structs;
      pr.add("Classes");
    }
  }

  // ---- exported functions --------------------------------------------------
  if (!model.functions.empty()) {
    line("// Namespace: ");
    line("public static class Functions // TypeDefIndex: " + std::to_string(++type_index));
    line("{");
    line("    // Fields");
    std::vector<const ir::FunctionDef*> fns;
    fns.reserve(model.functions.size());
    for (const ir::FunctionDef& f : model.functions) fns.push_back(&f);
    std::sort(fns.begin(), fns.end(), [&model](const ir::FunctionDef* a,
                                               const ir::FunctionDef* b) {
      if (a->addr != b->addr) return a->addr < b->addr;
      return model.arena(a->name_off) < model.arena(b->name_off);
    });
    for (const ir::FunctionDef* f : fns) {
      const std::string_view nm = model.arena(f->name_off);
      if (nm.empty()) continue;
      line("    public static IntPtr " + bounded(nm, f->addr, opts.max_name) + "; // RVA: " +
           hex(f->addr) + " VA: " + hex(f->addr) + " Size: " + hex(f->size) +
           (opts.inferred ? (f->named ? " | TIER:sym" : " | TIER:infer") : ""));
      ++st.functions;
    }
    line("}");
    line("");
  }

  // ---- global variables ----------------------------------------------------
  if (!model.globals.empty()) {
    line("// Namespace: ");
    line("public static class GlobalVariables // TypeDefIndex: " +
         std::to_string(++type_index));
    line("{");
    line("    // Fields");
    std::vector<const ir::GlobalDef*> gls;
    gls.reserve(model.globals.size());
    for (const ir::GlobalDef& g : model.globals) gls.push_back(&g);
    std::sort(gls.begin(), gls.end(), [&model](const ir::GlobalDef* a,
                                               const ir::GlobalDef* b) {
      if (a->addr != b->addr) return a->addr < b->addr;
      return model.arena(a->name_off) < model.arena(b->name_off);
    });
    for (const ir::GlobalDef* g : gls) {
      const std::string_view nm = model.arena(g->name_off);
      if (nm.empty()) continue;
      line("    private static readonly IntPtr " + bounded(nm, g->addr, opts.max_name) +
           "; // RVA: " + hex(g->addr) + " VA: " + hex(g->addr) + " Size: " + hex(g->size));
      ++st.globals;
    }
    line("}");
    line("");
  }

  pr.stage("Finalising");
  pr.checkpoint();
  if (stats != nullptr) *stats = st;
}

}  // namespace stellar::output
