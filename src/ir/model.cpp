// SPDX-License-Identifier: MIT
#include "stellar/ir/model.h"

#include <cstdio>
#include <unordered_map>

namespace stellar::ir {

std::uint32_t Model::arena_add(std::string_view s) {
  const std::uint32_t off = static_cast<std::uint32_t>(arena_.size());
  arena_.insert(arena_.end(), s.begin(), s.end());
  arena_.push_back('\0');
  return off;
}

void Model::reserve_type_slots(std::size_t n) {
  std::size_t cap = 16;
  while (cap < n * 2) cap <<= 1;  // keep the load factor at or below 0.5
  type_slots.assign(cap, 0);
  type_keys.assign(cap, 0);
  type_slot_mask = static_cast<std::uint32_t>(cap - 1);
}

bool Model::index_type(std::uint32_t offset, std::uint32_t type_index) {
  if (type_slots.empty()) reserve_type_slots(1024);
  std::uint32_t i = hash32(offset) & type_slot_mask;
  for (;;) {
    const std::uint32_t v = type_slots[i];
    if (v == 0) {
      type_slots[i] = type_index + 1;  // 0 marks an empty slot
      type_keys[i] = offset;
      return true;
    }
    if (type_keys[i] == offset) {
      type_slots[i] = type_index + 1;  // a redeclaration wins
      return true;
    }
    i = (i + 1) & type_slot_mask;
  }
}

std::uint64_t Model::size_of(std::uint32_t t, unsigned depth) const {
  if (t == kNoType || t >= types.size() || depth > 32) return 0;
  Type& ty = const_cast<Type&>(types[t]);
  if (ty.size_done) return ty.size;
  ty.size_done = 1;  // also breaks reference cycles

  std::uint64_t s = 0;
  switch (ty.kind) {
    case TypeKind::kPointer:
    case TypeKind::kLRef:
    case TypeKind::kRRef:
      s = 8;  // AArch64 LP64
      break;
    case TypeKind::kArray:
      if (ty.elem != kNoType && ty.count != 0) {
        s = size_of(ty.elem, depth + 1) * ty.count;
      }
      break;
    case TypeKind::kTypedef:
    case TypeKind::kConst:
    case TypeKind::kVolatile:
    case TypeKind::kRestrict:
    case TypeKind::kAtomic:
      s = ty.elem != kNoType ? size_of(ty.elem, depth + 1) : 0;
      break;
    case TypeKind::kStruct:
    case TypeKind::kClass:
    case TypeKind::kUnion:
      if (ty.def >= 0 && static_cast<std::size_t>(ty.def) < classes.size()) {
        s = classes[static_cast<std::size_t>(ty.def)].size;
      }
      if (s == 0) s = ty.size;
      break;
    case TypeKind::kEnum:
      s = (ty.def >= 0 && static_cast<std::size_t>(ty.def) < enums.size() &&
           enums[static_cast<std::size_t>(ty.def)].size != 0)
              ? enums[static_cast<std::size_t>(ty.def)].size
              : 4;
      break;
    case TypeKind::kFunction:
      s = 0;  // sizeof(function) is not a meaningful quantity
      break;
    default:
      s = ty.size;  // base_type, or anything DWARF stated directly
      break;
  }
  ty.size = s;
  return s;
}

namespace {

/// Maps a DWARF base type to the C# keyword used by the reference output.
///
/// Both spellings are needed: clang reports "unsigned int" for `uint`, while a
/// plain `char` must stay "char" rather than becoming sbyte/byte.
const char* base_type_name(std::string_view dw, std::uint8_t encoding, std::uint64_t size) {
  using namespace stellar::dwarf;
  if (dw == "char" || dw == "signed char" || dw == "unsigned char" || dw == "wchar_t" ||
      dw == "char8_t" || dw == "char16_t" || dw == "char32_t") {
    return "char";
  }
  if (dw == "bool" || dw == "_Bool") return "bool";
  if (dw == "float") return "float";
  if (dw == "double" || dw == "long double") return "double";
  if (dw == "void") return "void";
  if (dw == "int" || dw == "signed int" || dw == "signed") return "int";
  if (dw == "unsigned int" || dw == "unsigned") return "uint";
  if (dw == "short" || dw == "short int" || dw == "signed short" || dw == "signed short int")
    return "short";
  if (dw == "unsigned short" || dw == "unsigned short int") return "ushort";
  if (dw == "long" || dw == "long int" || dw == "signed long") return "long";
  if (dw == "unsigned long" || dw == "unsigned long int") return "ulong";
  if (dw == "long long" || dw == "long long int" || dw == "signed long long") return "long";
  if (dw == "unsigned long long" || dw == "unsigned long long int") return "ulong";
  if (dw == "__int128") return size == 16 ? "IntPtr" : "long";
  if (dw == "unsigned __int128") return size == 16 ? "UIntPtr" : "ulong";

  // Fall back on encoding + width when the producer spelled it unusually.
  switch (encoding) {
    case ate::kBoolean: return "bool";
    case ate::kFloat:
    case ate::kComplexFloat: return size == 4 ? "float" : "double";
    case ate::kSigned:
    case ate::kSignedChar:
    case ate::kSignedFixed:
      return size == 1 ? "sbyte" : size == 2 ? "short" : size == 4 ? "int" : "long";
    case ate::kUnsigned:
    case ate::kUnsignedChar:
    case ate::kUnsignedFixed:
      return size == 1 ? "byte" : size == 2 ? "ushort" : size == 4 ? "uint" : "ulong";
    case ate::kAddress: return "IntPtr";
    case ate::kUtf:
    case ate::kUcs:
    case ate::kAscii: return "string";
    default: return nullptr;
  }
}

}  // namespace

std::string Model::type_name(std::uint32_t t) const {
  // Iterative, with `suffix` collecting the operators to append: this keeps
  // chains such as pointer-to-array-of-pointer from recursing.
  std::string suffix;
  unsigned guard = 0;
  while (t != kNoType && t < types.size() && guard++ < 64) {
    const Type& ty = types[t];
    switch (ty.kind) {
      case TypeKind::kPointer:
        suffix += '*';
        t = ty.elem;
        continue;
      case TypeKind::kLRef:
        suffix += '&';
        t = ty.elem;
        continue;
      case TypeKind::kRRef:
        suffix += "&&";
        t = ty.elem;
        continue;
      case TypeKind::kArray: {
        char buf[24];
        if (ty.count != 0) {
          std::snprintf(buf, sizeof(buf), "[%llu]", static_cast<unsigned long long>(ty.count));
          suffix = std::string(buf) + suffix;  // [] binds tighter than *
        } else {
          suffix = "[]" + suffix;
        }
        t = ty.elem;
        continue;
      }
      case TypeKind::kConst:
      case TypeKind::kVolatile:
      case TypeKind::kRestrict:
        t = ty.elem;  // transparent
        continue;
      default:
        break;
    }
    break;  // reached a named leaf
  }

  std::string base;
  if (t == kNoType || t >= types.size()) {
    base = "unknown";
  } else {
    const Type& ty = types[t];
    const std::string_view nm = str(ty.name_off);
    const std::string n{nm};
    switch (ty.kind) {
      case TypeKind::kBase: {
        const char* mapped = base_type_name(nm, ty.encoding, ty.size);
        base = mapped != nullptr ? mapped : (n.empty() ? "unknown" : n);
        break;
      }
      // A named struct or union is referred to by name alone: the dump declares
      // every type with `class`, so a redundant `struct` prefix is noise. The
      // keyword is only needed when DWARF gave no name at all.
      case TypeKind::kStruct: base = n.empty() ? "struct" : n; break;
      case TypeKind::kClass: base = n.empty() ? "class" : n; break;
      case TypeKind::kUnion: base = n.empty() ? "union" : n; break;
      case TypeKind::kEnum: base = n.empty() ? "enum" : n; break;
      case TypeKind::kFunction: base = n.empty() ? "IntPtr" : n; break;
      default: base = n.empty() ? "unknown" : n; break;
    }
  }
  return base + suffix;
}

void Model::deduplicate() {
  // Keyed on the *text*, not the .debug_str offset: the linker emits the same
  // string once per object, so identical names in different units can have
  // different offsets.
  // Keyed on (name, kind): a `class Foo` and a `struct Foo` are distinct C++
  // types and both must be emitted.
  std::unordered_map<std::string, std::uint32_t> best;

  const auto rank = [](const ClassDef& c) -> std::uint64_t {
    return (static_cast<std::uint64_t>(c.field_count) << 32) | (c.size & 0xffffffffu);
  };

  // Per-name agreement tracking, so the caller can be told how much the
  // name-based collapse actually discards rather than assuming it is lossless.
  struct Group {
    std::uint32_t best = 0;
    std::uint32_t min_fields = 0xffffffffu;
    std::uint32_t max_fields = 0;
    std::uint64_t min_size = ~std::uint64_t{0};
    std::uint64_t max_size = 0;
    std::uint32_t members = 0;
  };
  std::unordered_map<std::string, Group> groups;
  groups.reserve(classes.size() * 2);

  for (std::uint32_t i = 0; i < classes.size(); ++i) {
    const ClassDef& c = classes[i];
    const std::string_view nm = str(c.name_off);
    if (nm.empty()) { classes[i].hidden = 1; continue; }
    std::string name;
    name.reserve(nm.size() + 2);
    name.push_back(static_cast<char>('0' + c.kind));
    name.append(nm);

    Group& g = groups[name];
    g.members++;
    g.min_fields = c.field_count < g.min_fields ? c.field_count : g.min_fields;
    g.max_fields = c.field_count > g.max_fields ? c.field_count : g.max_fields;
    g.min_size = c.size < g.min_size ? c.size : g.min_size;
    g.max_size = c.size > g.max_size ? c.size : g.max_size;

    if (g.best == 0 || rank(c) > rank(classes[g.best])) g.best = i;
  }
  // Apply the winners and hide everything else.
  for (ClassDef& c : classes) c.hidden = 1;  // default: not emitted
  for (const auto& [key, g] : groups) {
    classes[g.best].hidden = 0;
    if (g.members > 1) {
      ++dedup_conflicting_names;
      if (g.min_fields != g.max_fields) {
        ++dedup_field_conflicts;
        if (g.min_fields == 0) {
          ++dedup_declared_only;  // forward declaration + real definition
        } else {
          ++dedup_real_field_conflict;  // two non-empty layouts: ambiguous
          if (conflicts.size() < 512) {
            NameConflict nc;
            nc.name.assign(key.substr(1));  // strip the kind prefix
            nc.kind = static_cast<std::uint8_t>(key[0] - '0');
            nc.min_fields = g.min_fields;
            nc.max_fields = g.max_fields;
            nc.min_size = g.min_size;
            nc.max_size = g.max_size;
            nc.declarations = g.members;
            conflicts.push_back(std::move(nc));
          }
        }
      }
      if (g.min_size != g.max_size) {
        ++dedup_layout_conflicts;
        if (g.min_size == 0) {
          ++dedup_layout_zero;
        } else {
          ++dedup_real_size_conflict;
        }
      }
    }
    dedup_shadowed_classes += g.members - 1;
  }

  const std::uint64_t unique_classes = groups.size();
  duplicate_class_names = classes.size() - unique_classes;
  // Propagate the winner's layout to the shadowed redeclarations, so a field
  // whose type reference lands on a declaration-only DIE still reports the
  // right size. One pass over the classes, keyed by the same name.
  for (ClassDef& c : classes) {
    if (!c.hidden || c.size != 0) continue;
    const std::string_view nm = str(c.name_off);
    if (nm.empty()) continue;
    std::string probe;
    probe.reserve(nm.size() + 2);
    probe.push_back(static_cast<char>('0' + c.kind));
    probe.append(nm);
    auto it = groups.find(probe);
    if (it != groups.end()) c.size = classes[it->second.best].size;
  }

  // Type nodes carry their own copy of DW_AT_byte_size; keep them in step.
  for (Type& ty : types) {
    if (ty.kind != TypeKind::kStruct && ty.kind != TypeKind::kClass &&
        ty.kind != TypeKind::kUnion) {
      continue;
    }
    if (ty.def < 0 || static_cast<std::size_t>(ty.def) >= classes.size()) continue;
    const std::uint64_t cs = classes[static_cast<std::size_t>(ty.def)].size;
    if (cs != 0) ty.size = cs;
  }

  // Enums get the same treatment, with their own map (values are plain indices).
  std::unordered_map<std::string, std::uint32_t> ebest;
  ebest.reserve(enums.size() * 2);
  for (EnumDef& e : enums) e.hidden = 1;
  for (std::uint32_t i = 0; i < enums.size(); ++i) {
    const EnumDef& e = enums[i];
    const std::string_view nm = str(e.name_off);
    if (nm.empty()) continue;
    std::string name(nm);
    auto it = ebest.find(name);
    if (it == ebest.end()) {
      ebest.emplace(std::move(name), i);
      enums[i].hidden = 0;
      continue;
    }
    if (e.member_count > enums[it->second].member_count) {
      enums[it->second].hidden = 1;
      it->second = i;
      enums[i].hidden = 0;
    }
  }
  unique_enum_names = ebest.size();
}

}  // namespace stellar::ir
