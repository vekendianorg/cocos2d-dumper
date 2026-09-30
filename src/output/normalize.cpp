// SPDX-License-Identifier: MIT
#include "stellar/output/normalize.h"

#include <algorithm>
#include <vector>

namespace stellar::output {
namespace {

std::string_view trim(std::string_view s) {
  const auto ws = " \t\n\r";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

/// Splits `a, b<c, d>, e` on top-level commas, respecting angle-bracket nesting.
std::vector<std::string_view> split_args(std::string_view body) {
  std::vector<std::string_view> out;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '<' || c == '(' || c == '[') ++depth;
    else if (c == '>' || c == ')' || c == ']') --depth;
    else if (c == ',' && depth == 0) {
      out.push_back(trim(body.substr(start, i - start)));
      start = i + 1;
    }
  }
  const std::string_view last = trim(body.substr(start));
  if (!last.empty() || !out.empty()) out.push_back(last);
  return out;
}

/// Namespace prefixes erased wherever they appear. These are all vendor
/// spellings of the same standard library, not distinct types.
constexpr std::string_view kVendorPrefixes[] = {
    "std::__ndk1::", "std::__1::", "std::cxx11::", "std::",
    "google::protobuf::internal::", "google::protobuf::", "google::",
    "::__ndk1::", "__ndk1::", "__1::",
    "::internal::", "internal::",
};

/// Types where a pointer is real information rather than managed-reference
/// noise, so signatures keep their `*`.
bool is_scalar(std::string_view b) {
  static const std::string_view kNames[] = {
      "void", "bool", "char", "sbyte", "byte", "short", "ushort", "int", "uint",
      "long", "ulong", "nint", "nuint", "IntPtr", "UIntPtr", "float", "double",
      "decimal", "string",
  };
  return std::find(std::begin(kNames), std::end(kNames), b) != std::end(kNames);
}

std::string_view erase_prefix(std::string_view s) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::string_view p : kVendorPrefixes) {
      if (starts_with(s, p)) {
        s.remove_prefix(p.size());
        changed = true;
        break;
      }
    }
  }
  return s;
}

/// A type that is a managed reference in disguise: shared_ptr, unique_ptr, ...
bool is_smart_pointer(std::string_view base) {
  static const std::string_view kNames[] = {"shared_ptr", "unique_ptr", "weak_ptr",
                                             "scoped_ptr",  "intrusive_ptr",
                                             "reinterpret_ptr", "enable_shared_from_this"};
  return std::find(std::begin(kNames), std::end(kNames), base) != std::end(kNames);
}

/// Wrapper types that carry no type information of their own.
bool is_droppable(std::string_view base) {
  static const std::string_view kNames[] = {
      "allocator", "less", "greater", "equal_to", "hash", "allocator_traits",
      "char_traits", "remove_reference", "remove_const", "remove_cv",
      "remove_pointer", "add_pointer", "enable_if", "conditional", "integral_constant",
      "default_delete", "type_identity", "move", "forward", "decay", "common_type",
  };
  return std::find(std::begin(kNames), std::end(kNames), base) != std::end(kNames);
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

/// One rewrite pass. `drop_refs` strips `*`/`&` markers (signatures only).
std::string rewrite(std::string_view in, bool drop_refs, int depth);

/// Normalises each element of a comma-separated list, e.g. a parameter list.
std::string rewrite_list(std::string_view body, bool drop_refs, int depth) {
  std::string out;
  for (std::string_view item : split_args(body)) {
    if (item.empty()) continue;
    if (!out.empty()) out += ", ";
    out += rewrite(item, drop_refs, depth + 1);
  }
  return out;
}

/// Which trailing markers survive into a signature.
///
/// A reference carries nothing for a managed reader, so `&` always goes. A
/// pointer does, when it points at a scalar: `int*` is meaningfully different
/// from `int`.
std::string kept_markers(std::string_view peeled, std::string_view structural, bool drop_refs,
                         bool scalar) {
  std::string keep;
  if (drop_refs) {
    // A reference carries nothing for a managed reader; a pointer to a scalar
    // still does, so only that one is kept.
    if (scalar) {
      for (const char c : peeled) {
        if (c == '*') keep.push_back(c);
      }
    }
  } else {
    keep.assign(peeled);
  }
  keep.append(structural);
  return keep;
}

std::string rewrite(std::string_view in, bool drop_refs, int depth) {
  std::string_view s = trim(in);
  if (s.empty() || depth > 24) return std::string(s);

  for (std::string_view q : {"const ", "volatile ", "const volatile "}) {
    if (starts_with(s, q)) {
      s = trim(s.substr(std::string_view(q).size()));
      break;
    }
  }

  // Split the trailing pointer/reference/array markers off the end.
  // `peeled` holds only the `*`/`&` that came off the end of the name; the
  // structural suffixes (`[]`, `()`) are kept in `suffix` and are never
  // confused with an indirection marker.
  std::string suffix;
  std::string peeled;
  while (!s.empty()) {
    const char c = s.back();
    if (c == '*' || c == '&') {
      suffix.insert(suffix.begin(), c);
      peeled.insert(peeled.begin(), c);
      s.remove_suffix(1);
      s = trim(s);
    } else if (c == ']') {
      // Array suffix: find its matching '['.
      std::size_t d = 0;
      std::size_t i = s.size();
      while (i > 0) {
        --i;
        if (s[i] == ']') ++d;
        else if (s[i] == '[') {
          if (--d == 0) {
            suffix += s.substr(i);
            s = s.substr(0, i);
            s = trim(s);
            break;
          }
        }
      }
      break;
    } else if (c == ')') {
      // Trailing parameter list of a function type: find its matching '('.
      std::size_t d = 0;
      std::size_t i = s.size();
      while (i > 0) {
        --i;
        if (s[i] == ')') ++d;
        else if (s[i] == '(') {
          if (--d == 0) {
            suffix += s.substr(i);
            s = s.substr(0, i);
            s = trim(s);
            break;
          }
        }
      }
      break;
    } else {
      break;
    }
  }

  // A template-id may appear partway through a qualified name, as in
  // `FSEvent<Key>::Handler`. Split at the *first* `<` and handle whatever
  // follows it, so both shapes are covered.
  const std::size_t first_lt = s.find('<');
  if (first_lt != std::string_view::npos) {
    std::size_t d = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t i = first_lt; i < s.size(); ++i) {
      if (s[i] == '<') ++d;
      else if (s[i] == '>' && --d == 0) { close = i; break; }
    }
    // When the template spans the whole string the ordinary trailing-template
    // path below already handles it; only take this path when something
    // follows, otherwise this would recurse on its own input.
    if (close != std::string_view::npos && close + 1 < s.size()) {
      const std::string head = rewrite(s.substr(0, close + 1), drop_refs, depth + 1);
      std::string rest;
      if (close + 1 < s.size()) {
        rest = rewrite(s.substr(close + 1), drop_refs, depth + 1);
      }
      const std::string joined = rest.empty() ? head : head + rest;
      const bool scalar_here = is_scalar(trim(joined));
      return joined + kept_markers(peeled, suffix.substr(peeled.size()), drop_refs, scalar_here);
    }
  }

  // Split the base name from its template arguments.
  std::string_view base = s;
  std::string args;
  bool had_args = false;
  const std::size_t lt = s.find('<');
  if (lt != std::string_view::npos && s.back() == '>') {
    base = trim(s.substr(0, lt));
    args = std::string(s.substr(lt + 1, s.size() - lt - 2));
    had_args = true;
  }
  base = trim(base);
  if (base.empty()) base = "unknown";

  base = erase_prefix(base);
  // A nested base such as `std::__ndk1::vector` can hide the vendor prefix
  // behind template arguments, which have already been rewritten below.

  const std::string base_name(base);

  // Rewrite each argument first, so decisions are made on clean names.
  std::vector<std::string> arg_out;
  if (!args.empty()) {
    for (std::string_view a : split_args(args)) {
      if (a.empty()) continue;
      const std::string r = rewrite(a, drop_refs, depth + 1);
      if (!r.empty()) arg_out.push_back(r);
    }
  }

  // Now decide what this node means.
  // Normalise each entry inside a trailing parameter list.
  if (suffix.size() >= 2 && suffix.front() == '(' && suffix.back() == ')') {
    suffix = "(" + rewrite_list(suffix.substr(1, suffix.size() - 2), drop_refs, depth) + ")";
  }

  // A reference is always noise in a signature; a pointer is noise only when
  // it points at something a managed language would hold by reference.
  const bool scalar = is_scalar(base_name);
  const std::string tail_mark = kept_markers(peeled, suffix.substr(peeled.size()), drop_refs, scalar);
  const auto tail = [&tail_mark](const std::string& t) { return t + tail_mark; };

  if (is_droppable(base_name)) return {};  // allocator noise and friends
  if (base_name == "basic_string") return tail("string");
  if (base_name == "vector") {
    if (arg_out.empty()) return tail("List");
    return tail("List<" + arg_out.front() + ">");
  }
  if (base_name == "string" || base_name == "String") return tail("string");
  if (is_smart_pointer(base_name)) {
    // A smart pointer is a managed reference: the pointee is the type.
    if (arg_out.empty()) return tail("object");
    return tail(arg_out.front());
  }

  std::string out = base_name;
  if (had_args) {
    out += "<";
    for (std::size_t i = 0; i < arg_out.size(); ++i) {
      if (i != 0) out += ", ";
      out += arg_out[i];
    }
    out += ">";
  }
  return tail(out);
}

}  // namespace

std::string normalize_type(std::string_view cxx) {
  return rewrite(cxx, /*drop_refs=*/false, 0);
}

std::string normalize_signature_type(std::string_view cxx) {
  return rewrite(cxx, /*drop_refs=*/true, 0);
}

bool is_abi_artifact(std::string_view name) {
  if (name.empty()) return false;
  // vtable pointer injected by the ABI into every polymorphic class.
  if (starts_with(name, "_vptr")) return true;
  // The complete-object/base-class bookkeeping fields.
  if (name == "_BASES_" || name == "_BASES_") return true;
  // protobuf's generated `k<Field>FieldNumber` constants.
  static constexpr std::string_view kSuffix = "FieldNumber";
  if (name.size() > kSuffix.size() && name[0] == 'k' && name[1] >= 'A' && name[1] <= 'Z' &&
      name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    return true;
  }
  return false;
}

bool is_generated_method(std::string_view name) {
  if (name.empty()) return false;
  // Mangled names the ABI emits: lambdas, sort/introsort helpers, thunks.
  if (starts_with(name, "_Z")) return true;
  if (starts_with(name, "__introsort_") || starts_with(name, "__sort3_") ||
      starts_with(name, "__sort4_") || starts_with(name, "__merge_sort_") ||
      starts_with(name, "__quick_sort") || starts_with(name, "__unguarded_") ||
      starts_with(name, "__clamp_") || starts_with(name, "__ndk1_") ||
      starts_with(name, "__libcpp_") || starts_with(name, "_GLOBAL__")) {
    return true;
  }
  // Virtual thunks and typeinfo plumbing.
  if (starts_with(name, "non-virtual thunk") || starts_with(name, "virtual thunk") ||
      starts_with(name, "guard variable for") || starts_with(name, "typeinfo for") ||
      starts_with(name, "vtable for")) {
    return true;
  }
  return false;
}

std::string erase_vendor_namespaces(std::string_view cxx) {
  std::string out;
  std::size_t i = 0;
  while (i < cxx.size()) {
    // Recurse into template arguments so nested vendor names are removed too.
    if (cxx[i] == '<') {
      int depth = 0;
      const std::size_t start = i;
      while (i < cxx.size()) {
        if (cxx[i] == '<') ++depth;
        else if (cxx[i] == '>' && --depth == 0) { ++i; break; }
        ++i;
      }
      out += '<';
      out += erase_vendor_namespaces(cxx.substr(start + 1, i - start - 2));
      out += '>';
      continue;
    }
    // Try each vendor prefix at this position.
    bool matched = false;
    for (std::string_view pfx : kVendorPrefixes) {
      if (cxx.size() - i >= pfx.size() && cxx.compare(i, pfx.size(), pfx) == 0) {
        i += pfx.size();
        matched = true;
        break;
      }
    }
    if (matched) continue;
    out.push_back(cxx[i++]);
  }
  return out;
}

std::string clean_symbol_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    // `[abi:cxx11]` and similar tags carry no meaning for a reader.
    if (name[i] == '[') {
      const std::size_t close = name.find(']', i);
      if (close != std::string_view::npos) {
        const std::string_view tag = name.substr(i, close - i + 1);
        if (tag.find("abi:") != std::string_view::npos) {
          // `operator[abi:cxx11]` is a real subscript wearing a tag: keep the
          // subscript and drop only the tag, so the operator still reads.
          const bool after_operator =
              i >= 8 && name.compare(i - 8, 8, "operator") == 0;
          if (after_operator) out += "[]";
          i = close;
          continue;
        }
      }
    }
    out.push_back(name[i]);
  }
  return out;
}

}  // namespace stellar::output
