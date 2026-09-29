// SPDX-License-Identifier: MIT
#include "c2d/ir/build.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "c2d/diag/log.h"
#include "c2d/diag/metrics.h"
#include "c2d/diag/progress.h"
#include "c2d/dwarf/eh_frame.h"
#include "c2d/dwarf/constants.h"

namespace c2d::ir {
namespace {

using namespace c2d::dwarf;

/// Resolver shim so the model can read .debug_str without owning the context.
std::string_view str_thunk(void* self, std::uint32_t off) {
  return static_cast<DwarfContext*>(self)->str_at(off);
}

/// True for the tags that define a node in the type graph.
bool is_type_tag(std::uint32_t t) {
  switch (t) {
    case tag::kBaseType:
    case tag::kPointerType:
    case tag::kReferenceType:
    case tag::kConstType:
    case tag::kVolatileType:
    case tag::kRestrictType:
    case tag::kAtomicType:
    case tag::kArray:
    case tag::kTypedef:
    case tag::kStructureType:
    case tag::kClass:
    case tag::kUnion:
    case tag::kEnumeration:
    case tag::kSubroutineType:
    case tag::kUnspecifiedType:
      return true;
    default:
      return false;
  }
}

TypeKind kind_of(std::uint32_t t) {
  switch (t) {
    case tag::kBaseType: return TypeKind::kBase;
    case tag::kPointerType: return TypeKind::kPointer;
    case tag::kReferenceType: return TypeKind::kLRef;
    case tag::kConstType: return TypeKind::kConst;
    case tag::kVolatileType: return TypeKind::kVolatile;
    case tag::kRestrictType: return TypeKind::kRestrict;
    case tag::kAtomicType: return TypeKind::kAtomic;
    case tag::kArray: return TypeKind::kArray;
    case tag::kTypedef: return TypeKind::kTypedef;
    case tag::kStructureType: return TypeKind::kStruct;
    case tag::kClass: return TypeKind::kClass;
    case tag::kUnion: return TypeKind::kUnion;
    case tag::kEnumeration: return TypeKind::kEnum;
    case tag::kSubroutineType: return TypeKind::kFunction;
    case tag::kUnspecifiedType: return TypeKind::kUnspecified;
    default: return TypeKind::kUnknown;
  }
}

/// Extracts a member offset from DW_AT_data_member_location.
///
/// Clang emits a plain constant for most members, but uses a location
/// expression (`DW_OP_plus_uconst`) once a base class is involved. Anything
/// that is genuinely base-relative is reported as unknown rather than guessed,
/// so the emitter can mark it instead of printing a wrong offset.
bool extract_offset(const AttrValue& v, std::uint64_t* out_offset, bool* is_static) {
  *is_static = false;
  if (v.form == form::kExprloc) {
    if (v.block.empty()) return false;
    const std::uint8_t op = v.block.data()[0];
    if (op == 0x23u /* DW_OP_plus_uconst */) {
      if (v.block.size() < 2) return false;
      std::uint64_t val = 0;
      if (util::decode_uleb128(v.block.data() + 1, v.block.data() + v.block.size(), val) == 0) {
        return false;
      }
      *out_offset = val;
      return true;
    }
    if (op == 0x22u /* DW_OP_plus */) return false;  // base-relative: unknown
    return false;
  }
  // Constant forms (udata/sdata/data1/data2/...).
  switch (v.form) {
    case form::kUdata:
    case form::kData1:
    case form::kData2:
    case form::kData4:
    case form::kData8:
    case form::kSecOffset:
      *out_offset = v.u64;
      return true;
    case form::kSdata:
      *out_offset = static_cast<std::uint64_t>(v.i64);
      return true;
    case form::kFlagPresent:
      *is_static = true;
      return true;
    default:
      return false;
  }
}

}  // namespace
namespace {

/// One open aggregate while walking a unit: a class/union whose field list is
/// still being filled, or an enum whose member list is still growing.
///
/// The DIE stream carries no explicit "end of children" marker, so the walker's
/// depth counter is what tells us an aggregate is finished.
struct Open {
  std::uint32_t depth = 0;
  std::int32_t cls = -1;    ///< index into Model::classes, -1 for enums
  std::int32_t enm = -1;    ///< index into Model::enums
  std::uint32_t first = 0;  ///< where its fields/members started
  std::uint32_t node = 0;   ///< the Type node (for array bound tracking)
};

}  // namespace

bool build_model(DwarfContext& ctx, const BuildOptions& opts, Model& model,
                 BuildStats* stats) {
  const auto t_start = diag::Clock::now();
  auto& pr = diag::progress();
  model.string_self = &ctx;
  model.string_fn = &str_thunk;

  // The counters are declared up front so the line is complete and stable from
  // the first frame rather than growing as stages are discovered.
  pr.declare("Units", 0, false);
  pr.declare("DIEs", 0, false);
  pr.declare("Types", 0, false);
  pr.declare("Fields", 0, false);
  pr.declare("Methods", 0, false);
  pr.primary("DIEs");

  BuildStats st;
  // Measured on the real target: ~3.5M type-defining DIEs. Reserving up front
  // avoids repeatedly reallocating several hundred megabytes of vectors.
  model.types.reserve(4u << 20);
  model.fields.reserve(600u << 10);
  model.methods.reserve(200u << 10);
  model.params.reserve(400u << 10);
  model.classes.reserve(600u << 10);
  model.enums.reserve(32u << 10);
  model.enum_members.reserve(64u << 10);
  model.reserve_type_slots(4u << 20);

  // DW_AT_type values that still need turning into node indices. Stored as
  // (slot, raw reference) so pass B can resolve them without re-walking.
  /// What a pending reference will be written into, so one resolve loop can
  /// serve types, fields, base classes, return types and parameters.
  enum class Slot : std::uint8_t {
    kType,       ///< model.types[i].elem
    kField,      ///< model.fields[i].type
    kClassBase,  ///< model.classes[i].base
    kMethodRet,  ///< model.methods[i].ret_type
    kParam,      ///< model.params[i].type
  };
  struct Pending {
    Slot slot;
    std::uint32_t index;     ///< index into the array named by `slot`
    std::uint32_t target;    ///< raw unit-relative DW_AT_type value
    std::uint64_t unit_off;  ///< .debug_info offset of the owning unit
  };
  std::vector<Pending> pending;
  pending.reserve(6u << 20);
  // A C++ member function appears twice: an in-class declaration carrying the
  // name and signature but no code, and an out-of-line definition that names
  // it with DW_AT_specification. The address only exists on the second, so
  // declarations are indexed by DIE offset and the definitions are applied
  // afterwards.
  std::unordered_map<std::uint64_t, std::uint32_t> decl_by_offset;
  decl_by_offset.reserve(1u << 20);
  struct SpecFix {
    std::uint64_t target = 0;   ///< absolute offset of the declaration DIE
    std::uint64_t addr = 0;
    std::uint32_t name_off = 0;
    std::uint32_t name_is_ref = 0;  ///< name came from DW_AT_linkage_name
  };
  std::vector<SpecFix> spec_fixes;
  spec_fixes.reserve(1u << 20);

  dwarf::DwarfContext::UnitIterator it(ctx);
  dwarf::UnitHeader h;
  std::string err;

  // ------------------------------------------------------------------ pass A --
  while (it.next(h, &err)) {
    if (opts.max_units != 0 && st.units >= opts.max_units) break;
    const AbbrevTable* ab = ctx.abbrev_table(h, &err);
    if (ab == nullptr) {
      ++st.units_without_abbrev;
      continue;
    }

    UnitWalker w(ctx.info(), h, ab);
    w.reset();
    Die die;
    std::vector<Open> open;
    open.reserve(32);
    // The most recent open aggregate, for the common "member directly inside a
    // class" case.
    std::int32_t cur_class = -1;
    std::int32_t cur_enum = -1;
    // Index into model.methods of the member function being assembled, and how
    // many formal parameters it has collected so far.
    std::int32_t cur_method = -1;
    std::uint32_t cur_method_first_param = 0;
    std::uint32_t method_depth = 0;

    while (w.next(die)) {
      ++st.dies;
      pr.add("DIEs");
      const unsigned depth = die.depth();
      const std::uint32_t t = die.tag();

      // Close an open member function when we return to class level.
      if (cur_method >= 0 && depth <= method_depth) {
        model.methods[static_cast<std::size_t>(cur_method)].param_count =
            static_cast<std::uint32_t>(model.params.size()) - cur_method_first_param;
        cur_method = -1;
      }

      // Close every aggregate that ends at or above this depth.
      while (!open.empty() && open.back().depth >= depth) {
        const Open& o = open.back();
        if (o.cls >= 0) {
          ClassDef& cd = model.classes[static_cast<std::size_t>(o.cls)];
          cd.field_count = static_cast<std::uint32_t>(model.fields.size()) - o.first;
        } else if (o.enm >= 0) {
          EnumDef& ed = model.enums[static_cast<std::size_t>(o.enm)];
          ed.member_count = static_cast<std::uint32_t>(model.enum_members.size()) - o.first;
        }
        open.pop_back();
      }
      cur_class = open.empty() ? -1 : open.back().cls;
      cur_enum = open.empty() ? -1 : open.back().enm;

      AttrValue v;

      if (is_type_tag(t)) {
        Type ty;
        ty.kind = kind_of(t);
        ty.transparent = (ty.kind == TypeKind::kConst || ty.kind == TypeKind::kVolatile ||
                          ty.kind == TypeKind::kRestrict || ty.kind == TypeKind::kTypedef)
                             ? 1
                             : 0;
        if (die.attr(aat::kName, v) && v.form == form::kStrp) {
          ty.name_off = static_cast<std::uint32_t>(v.u64);
        }
        if (die.attr(aat::kByteSize, v)) ty.size = v.u64;
        if (die.attr(aat::kEncoding, v)) ty.encoding = static_cast<std::uint8_t>(v.u64);

        Open o;
        o.depth = depth;
        o.node = static_cast<std::uint32_t>(model.types.size());
        o.first = 0;
        if (ty.kind == TypeKind::kStruct || ty.kind == TypeKind::kClass ||
            ty.kind == TypeKind::kUnion) {
          ClassDef cd;
          cd.name_off = ty.name_off;
          cd.size = ty.size;
          cd.kind = ty.kind == TypeKind::kClass ? 1 : (ty.kind == TypeKind::kUnion ? 2 : 0);
          cd.first_field = static_cast<std::uint32_t>(model.fields.size());
          cd.first_method = static_cast<std::uint32_t>(model.methods.size());
          o.cls = static_cast<std::int32_t>(model.classes.size());
          model.classes.push_back(cd);
          ty.def = o.cls;
          ++st.classes;
        } else if (ty.kind == TypeKind::kEnum) {
          EnumDef ed;
          ed.name_off = ty.name_off;
          ed.size = ty.size;
          ed.first_member = static_cast<std::uint32_t>(model.enum_members.size());
          o.enm = static_cast<std::int32_t>(model.enums.size());
          model.enums.push_back(ed);
          ty.def = o.enm;
          ++st.enums;
        }

        const std::uint32_t node = static_cast<std::uint32_t>(model.types.size());
        model.types.push_back(ty);
        ++st.type_nodes;
        pr.add("Types");
        model.index_type(static_cast<std::uint32_t>(die.offset()), node);

        if (die.attr(aat::kType, v)) {
          pending.push_back({Slot::kType, node, static_cast<std::uint32_t>(v.u64), h.offset});
        }
        if (die.has_children()) {
          o.first = (ty.kind == TypeKind::kArray)
                        ? 0
                        : (o.cls >= 0
                               ? model.classes[static_cast<std::size_t>(o.cls)].first_field
                               : (o.enm >= 0
                                      ? model.enums[static_cast<std::size_t>(o.enm)].first_member
                                      : 0));
          open.push_back(o);
          cur_class = o.cls;
          cur_enum = o.enm;
        }
        continue;
      }

      switch (t) {
        case tag::kMember: {
          if (cur_class < 0) break;
          Field f;
          if (die.attr(aat::kName, v) && v.form == form::kStrp) {
            f.name_off = static_cast<std::uint32_t>(v.u64);
          }
          // DW_AT_data_member_location gives the instance offset; its absence
          // means the member is static.
          bool is_static = false;
          if (die.attr(aat::kDataMemberLocation, v)) {
            std::uint64_t off = 0;
            f.offset_known = extract_offset(v, &off, &is_static) ? 1 : 0;
            f.offset = off;
          } else {
            is_static = true;
          }
          // A static member's address lives in DW_AT_location as DW_OP_addr.
          if (is_static && die.attr(aat::kLocation, v) && v.has_block && !v.block.empty() &&
              v.block.data()[0] == 0x03 /* DW_OP_addr */) {
            std::uint64_t a = 0;
            util::Cursor ac(v.block.data() + 1, v.block.size() - 1, h.endian);
            if (ac.read_uint(h.address_size, a)) {
              f.offset = a;
              f.offset_known = 1;
            }
          }
          f.is_static = static_cast<std::uint8_t>(is_static ? 1 : 0);
          if (die.attr(aat::kBitSize, v)) {
            f.is_bitfield = 1;
            f.bit_size = v.u64;
            if (die.attr(aat::kBitOffset, v)) {
              f.bit_offset = static_cast<std::uint32_t>(v.u64);
            }
          }
          const std::uint32_t field_index = static_cast<std::uint32_t>(model.fields.size());
          model.fields.push_back(f);
          ++st.fields;
          pr.add("Fields");
          if (die.attr(aat::kType, v)) {
            pending.push_back(
                {Slot::kField, field_index, static_cast<std::uint32_t>(v.u64), h.offset});
          }
          break;
        }
        case tag::kEnumerator: {
          if (cur_enum < 0) break;
          EnumMember em;
          if (die.attr(aat::kName, v) && v.form == form::kStrp) {
            em.name_off = static_cast<std::uint32_t>(v.u64);
          }
          if (die.attr(aat::kConstValue, v)) em.value = v.i64;
          model.enum_members.push_back(em);
          ++st.enumerators;
          break;
        }
        case tag::kInheritance: {
          if (cur_class < 0) break;
          if (die.attr(aat::kType, v)) {
            pending.push_back({Slot::kClassBase, static_cast<std::uint32_t>(cur_class),
                               static_cast<std::uint32_t>(v.u64), h.offset});
          }
          break;
        }
        case tag::kSubprogram: {
          // An out-of-line definition points back at its in-class declaration
          // with DW_AT_specification; that is where the code address lives.
          // Both attributes mean "this definition stands in for that other DIE":
          // DW_AT_specification for a plain out-of-line member function, and
          // DW_AT_abstract_origin for inlined or virtual ones.
          if ((die.attr(aat::kSpecification, v) || die.attr(aat::kAbstractOrigin, v)) &&
              dwarf::is_unit_relative_ref(v.form)) {
            SpecFix fix;
            fix.target = h.offset + v.u64;
            if (die.attr(aat::kLowPc, v)) fix.addr = v.u64;
            if (die.attr(aat::kName, v) && v.form == form::kStrp) {
              fix.name_off = static_cast<std::uint32_t>(v.u64);
            } else if (die.attr(aat::kLinkageName, v) && v.form == form::kStrp) {
              fix.name_off = static_cast<std::uint32_t>(v.u64);
              fix.name_is_ref = 1;
            }
            if (fix.addr != 0 || fix.name_off != 0) spec_fixes.push_back(fix);
            break;
          }
          // Otherwise this is the declaration, which is what carries the
          // member list; a class-scoped subprogram is always a declaration.
          if (cur_class < 0) break;
          Method m;
          if (die.attr(aat::kName, v) && v.form == form::kStrp) {
            m.name_off = static_cast<std::uint32_t>(v.u64);
          }
          if (die.attr(aat::kLinkageName, v) && v.form == form::kStrp) {
            m.linkage_off = static_cast<std::uint32_t>(v.u64);
          }
          if (die.attr(aat::kLowPc, v)) m.addr = v.u64;
          if (die.attr(aat::kVirtuality, v)) m.is_virtual = v.u64 != 0 ? 1 : 0;
          if (die.attr(aat::kArtificial, v)) m.is_artificial = 1;
          if (die.attr(aat::kExternal, v) && v.u64 == 0) m.is_static = 1;
          if (die.attr(aat::kAccessibility, v)) {
            m.access = static_cast<std::uint8_t>(v.u64);
          }
          const std::string_view nm = ctx.str_at(m.name_off);
          if (nm == ".ctor" || nm == "C1" || nm == "C2") m.is_ctor = 1;

          m.first_param = static_cast<std::uint32_t>(model.params.size());
          const std::int32_t mi = static_cast<std::int32_t>(model.methods.size());
          model.methods.push_back(m);
          if (die.attr(aat::kType, v)) {
            pending.push_back({Slot::kMethodRet, static_cast<std::uint32_t>(mi),
                               static_cast<std::uint32_t>(v.u64), h.offset});
          }
          model.classes[static_cast<std::size_t>(cur_class)].method_count++;
          cur_method = mi;
          cur_method_first_param = m.first_param;
          method_depth = depth;
          decl_by_offset.emplace(h.offset + die.offset(), static_cast<std::uint32_t>(mi));
          ++st.methods;
          pr.add("Methods");
          break;
        }
        case tag::kFormalParameter: {
          if (cur_method < 0) break;
          // Compiler-generated parameters (the implicit `this`, vtable
          // helpers) are not part of the source-level signature.
          if (die.attr(aat::kArtificial, v) && v.u64 != 0) break;
          Param prm;
          if (die.attr(aat::kName, v) && v.form == form::kStrp) {
            prm.name_off = static_cast<std::uint32_t>(v.u64);
          }
          // Push first, then queue the reference against the real index:
          // computing size()-1 before the push underflows on the first param.
          const auto pi = static_cast<std::uint32_t>(model.params.size());
          model.params.push_back(prm);
          if (die.attr(aat::kType, v)) {
            pending.push_back(
                {Slot::kParam, pi, static_cast<std::uint32_t>(v.u64), h.offset});
          }
          ++st.params;
          break;
        }
        case tag::kSubrangeType: {
          // The first child of an array_type carries the element count.
          if (open.empty()) break;
          const Open& parent = open.back();
          if (parent.node >= model.types.size()) break;
          if (model.types[parent.node].kind != TypeKind::kArray) break;
          std::uint64_t count = 0;
          if (die.attr(aat::kCount, v)) {
            count = v.u64;
          } else if (die.attr(aat::kUpperBound, v)) {
            count = v.i64 >= 0 ? static_cast<std::uint64_t>(v.i64) + 1 : 0;
          }
          if (count != 0) model.types[parent.node].count = count;
          break;
        }
        default:
          break;
      }
    }

    // Close anything still open at end of unit.
    while (!open.empty()) {
      const Open& o = open.back();
      if (o.cls >= 0) {
        model.classes[static_cast<std::size_t>(o.cls)].field_count =
            static_cast<std::uint32_t>(model.fields.size()) - o.first;
      } else if (o.enm >= 0) {
        model.enums[static_cast<std::size_t>(o.enm)].member_count =
            static_cast<std::uint32_t>(model.enum_members.size()) - o.first;
      }
      open.pop_back();
    }
    ++st.units;
    pr.add("Units");
  }

  // Most member functions were inlined by the compiler, so DWARF holds only the
  // declaration and never a concrete DIE to read an address from. The mangled
  // linkage name is the join key: the linker kept a symbol for every out-of-line
  // copy, and that symbol carries the address.
  {
    std::unordered_map<std::string_view, std::uint64_t> by_name;
    for (const elf::Symbol& sym : ctx.elf().symbols()) {
      if (sym.value != 0 && !sym.name.empty()) by_name.emplace(sym.name, sym.value);
    }
    pr.declare("Methods", model.methods.size());
    pr.set("Methods", 0);
    for (Method& m : model.methods) {
      if (m.addr != 0 || m.linkage_off == 0) continue;
      const std::string_view mangled = ctx.str_at(m.linkage_off);
      if (mangled.empty()) continue;
      const auto it = by_name.find(mangled);
      if (it != by_name.end()) {
        m.addr = it->second;
        ++st.methods_with_addr;
      }
    }
  }

  pr.declare("Units", st.units);
  pr.set("DIEs", st.dies);
  pr.primary("DIEs");
  pr.stage("Resolving types");
  pr.checkpoint();

  // Fold the out-of-line definitions into their declarations: this is what
  // gives every member function a code address, and a mangled name when the
  // declaration is anonymous (clang emits the linkage name on the definition).
  for (const SpecFix& fix : spec_fixes) {
    const auto it = decl_by_offset.find(fix.target);
    if (it == decl_by_offset.end()) continue;
    ir::Method& m = model.methods[it->second];
    if (fix.addr != 0) m.addr = fix.addr;
    if (fix.name_off != 0 && (m.name_off == 0 || fix.name_is_ref != 0)) {
      m.name_off = fix.name_off;
    }
    ++st.methods_with_addr;
  }

  model.total_units = st.units;
  model.total_dies = st.dies;

  // ------------------------------------------------------------------ pass B --
  // Resolve every recorded DW_AT_type into a node index. The reference is
  // relative to the start of the unit that contains it, so the unit offset is
  // added back before the offset->type lookup.
  pr.declare("Types", pending.size());
  pr.set("Types", 0);
  pr.checkpoint();
  for (const Pending& p : pending) {
    pr.add("Types");
    const auto abs = static_cast<std::uint32_t>(p.unit_off + p.target);
    const std::uint32_t node = model.lookup_type(abs);
    if (node == kNoType) {
      ++st.unresolved_refs;
      continue;
    }
    switch (p.slot) {
      case Slot::kType: model.types[p.index].elem = node; break;
      case Slot::kField: model.fields[p.index].type = node; break;
      case Slot::kClassBase: model.classes[p.index].base = node; break;
      case Slot::kMethodRet: model.methods[p.index].ret_type = node; break;
      case Slot::kParam: model.params[p.index].type = node; break;
    }
  }

  // Collapse redeclarations first: a field's type reference may land on a
  // declaration-only DIE, and dedup is what tells us the real size.
  model.deduplicate();

  // Compute every size now that the graph is complete.
  pr.stage("Deduplicating");
  pr.checkpoint();
  pr.stage("Sizing types");
  pr.declare("Types", model.types.size());
  pr.set("Types", 0);
  for (std::uint32_t i = 0; i < model.types.size(); ++i) {
    model.size_of(i);
    pr.add("Types");
  }
  for (Field& f : model.fields) {
    if (!f.offset_known && !f.is_static) ++model.fields_unknown_offset;
  }

  // A union whose DWARF byte_size is absent is as large as its widest member.
  for (ClassDef& cd : model.classes) {
    if (cd.size != 0 || cd.kind != 2) continue;
    std::uint64_t widest = 0;
    const std::uint32_t end = cd.first_field + cd.field_count;
    for (std::uint32_t i = cd.first_field; i < end && i < model.fields.size(); ++i) {
      const std::uint64_t s = model.size_of(model.fields[i].type);
      if (s > widest) widest = s;
    }
    if (widest != 0) cd.size = widest;
  }

  pr.stage("Reading symbols");
  // ---------------------------------------------------------------- symbols --
  //
  // A static data member has no DW_AT_location; its address comes from the
  // symbol table, whose Itanium encoding stores the class and member as
  // adjacent length-prefixed components ("4Vec33ZERO"). Recovering them lets the
  // emitter print a real RVA instead of a placeholder.
  {
    for (const elf::Symbol& sym : ctx.elf().symbols()) {
      if (sym.type() != static_cast<std::uint8_t>(elf::StT::kObject)) continue;
      if (sym.value == 0 || sym.name.size() < 4 || sym.name.compare(0, 3, "_ZN") != 0) continue;
      // Parse _ZN <len><name><len><name>... and keep the final two components.
      std::vector<std::string_view> parts;
      std::size_t i = 3;
      bool ok = true;
      while (i < sym.name.size() && ok) {
        std::size_t j = i;
        while (j < sym.name.size() && std::isdigit(static_cast<unsigned char>(sym.name[j]))) ++j;
        if (j == i || j - i > 4) { ok = false; break; }
        const std::size_t len = static_cast<std::size_t>(std::strtoul(sym.name.substr(i, j - i).c_str(), nullptr, 10));
        if (j + len > sym.name.size()) { ok = false; break; }
        parts.emplace_back(sym.name.data() + j, len);
        i = j + len;
        // A trailing access-specifier letter ends the symbol.
        if (i < sym.name.size() && (sym.name[i] == 'E' || sym.name[i] == 'F' ||
                                    sym.name[i] == 'A' || sym.name[i] == 'B')) {
          break;
        }
      }
      if (ok && parts.size() >= 2) {
        std::string key(parts[parts.size() - 2]);
        key.append(parts[parts.size() - 1]);
        model.static_syms.emplace(std::move(key), sym.value);
      }
    }
  }

  // Functions and globals come from the symbol table: it has authoritative
  // names, addresses and sizes, and covers objects that have no DWARF DIE of
  // their own.
  if (opts.symbols) {
    const auto& syms = ctx.elf().symbols();
    model.functions.reserve(syms.size() / 2);
    model.globals.reserve(syms.size() / 4);
    std::uint64_t fn = 0, gl = 0;
    for (const elf::Symbol& s : syms) {
      if (s.name.empty() || s.value == 0) continue;
      switch (static_cast<elf::StT>(s.type())) {
        case elf::StT::kFunc: {
          FunctionDef f;
          f.name_off = model.arena_add(s.name);
          f.addr = s.value;
          f.size = s.size;
          model.functions.push_back(f);
          ++fn;
          break;
        }
        case elf::StT::kObject: {
          GlobalDef g;
          g.name_off = model.arena_add(s.name);
          g.addr = s.value;
          g.size = s.size;
          model.globals.push_back(g);
          ++gl;
          break;
        }
        default:
          break;
      }
    }
    C2D_DEBUG("symbols: %llu functions, %llu globals", static_cast<unsigned long long>(fn),
              static_cast<unsigned long long>(gl));
  }

  // Count definitions that carry no DWARF name; the emitter needs to know.
  for (const ClassDef& cd : model.classes) {
    if (cd.name_off == 0) ++model.unnamed_classes;
  }

  model.methods_count = st.methods;
  model.params_count = st.params;
  for (const ClassDef& c : model.classes) {
    if (c.base != kNoType) ++model.classes_with_bases;
  }
  st.seconds = diag::seconds_since(t_start);
  if (stats != nullptr) *stats = st;
  return true;
}

}  // namespace c2d::ir

namespace c2d::ir {

// ---------------------------------------------------------------------------
// Dwarfless mode
//
// A stripped binary has no DIE stream, so there is nothing to reconstruct
// types from. What it does still contain is the information the loader itself
// needs, and all of it is authoritative for a *different* question:
//
//   .eh_frame    every function's exact [start, end) range
//   .dyn/.symtab  names for whatever the toolchain exported
//   .data.rel.ro  vtable and RTTI arrays, which name classes and their method
//                 tables without any debug info
//
// Field offsets and field types are NOT recoverable this way, and this function
// does not pretend otherwise: the model is left with no field data and the
// emitter marks the result as inferred.
// ---------------------------------------------------------------------------

namespace {

/// Minimal Itanium name reader: pulls the class path out of a mangled name so a
/// vtable/typeinfo pair can be attributed to a class. Returns an empty string
/// when the name is not a recognisable `_ZN` encoding.
std::string mangled_class_path(std::string_view name) {
  // Accepts a full symbol (`_ZN7cocos2d4Vec34ZEROE`) or a vtable/typeinfo symbol
  // (`_ZTVN7cocos2d4Vec3E`), and returns the class path it encodes.
  if (name.size() >= 4 && (name.compare(0, 4, "_ZTV") == 0 ||
                           name.compare(0, 4, "_ZTI") == 0 ||
                           name.compare(0, 4, "_ZTS") == 0)) {
    name.remove_prefix(4);
  } else if (name.size() >= 3 && name.compare(0, 3, "_ZN") == 0) {
    name.remove_prefix(3);
  }
  std::size_t i = 0;
  if (i < name.size() && name[i] == 'N') ++i;  // nested-name marker

  std::string out;
  bool any = false;
  while (i < name.size()) {
    // `St` is the substitution for std:: and, unlike every other component, it
    // carries no length prefix. Without it every standard-library type fails to
    // resolve, which is most of the vtable population.
    if (i + 1 < name.size() && name[i] == 'S' && name[i + 1] == 't') {
      if (any) out += "::";
      out += "std";
      any = true;
      i += 2;
      continue;
    }
    std::size_t j = i;
    while (j < name.size() && name[j] >= '0' && name[j] <= '9') ++j;
    if (j == i || j - i > 3) break;  // not a length prefix: end of the path
    std::size_t len = 0;
    for (std::size_t k = i; k < j; ++k) len = len * 10 + static_cast<std::size_t>(name[k] - '0');
    if (len == 0 || j + len > name.size()) break;
    if (any) out += "::";
    out.append(name.substr(j, len));
    any = true;
    i = j + len;
    // A trailing access specifier, or the start of a template argument list,
    // ends the class path.
    if (i < name.size()) {
      const char c = name[i];
      if (c == 'E' || c == 'F' || c == 'A' || c == 'B' || c == 'C' || c == 'D' ||
          c == 'I') {
        break;
      }
    }
  }
  return any ? out : std::string();
}

}  // namespace

bool build_dwarfless_model(DwarfContext& ctx, Model& model, DwarflessStats* stats) {
  const auto t_start = diag::Clock::now();
  auto& pr = diag::progress();
  pr.stage("dwarfless");
  pr.declare("FDEs", 0, false);
  pr.declare("Functions", 0, false);
  pr.declare("Globals", 0, false);
  pr.declare("Classes", 0, false);
  DwarflessStats st;
  model.string_self = &ctx;
  model.string_fn = &str_thunk;
  model.inferred = true;

  const elf::ElfFile& elf = ctx.elf();

  // --- functions: .eh_frame gives every range, the symbol table names them ---
  std::unordered_map<std::uint64_t, const elf::Symbol*> by_addr;
  by_addr.reserve(elf.symbols().size() * 2);
  for (const elf::Symbol& s : elf.symbols()) {
    if (s.value == 0) continue;
    if (s.type() == static_cast<std::uint8_t>(elf::StT::kFunc) ||
        s.type() == static_cast<std::uint8_t>(elf::StT::kGnuIfunc)) {
      by_addr.emplace(s.value, &s);
    }
  }

  dwarf::EhFrameStats est;
  const std::vector<dwarf::FdeRange> fdes =
      dwarf::parse_eh_frame(ctx.sections().view(dwarf::Sec::kEhFrame),
                            ctx.sections().section_addr(dwarf::Sec::kEhFrame), &est);
  st.fdes = fdes.size();
  model.functions.reserve(fdes.size());

  pr.set("FDEs", fdes.size());
  for (const dwarf::FdeRange& f : fdes) {
    pr.add("FDEs");
    FunctionDef d;
    auto it = by_addr.find(f.start);
    if (it != by_addr.end() && !it->second->name.empty()) {
      d.name_off = model.arena_add(it->second->name);
      d.named = 1;
      ++st.functions_named;
    } else {
      // Unnamed: the address is the only stable identifier, so it becomes the
      // name. This is the `sub_<addr>` convention used by IDA and Ghidra.
      char buf[32];
      std::snprintf(buf, sizeof(buf), "sub_%llx",
                    static_cast<unsigned long long>(f.start));
      d.name_off = model.arena_add(buf);
      ++st.functions_sub_;
    }
    d.addr = f.start;
    d.size = f.size;
    model.functions.push_back(d);
  }

  // --- globals from the symbol table ---
  for (const elf::Symbol& s : elf.symbols()) {
    if (s.name.empty() || s.value == 0) continue;
    if (s.type() != static_cast<std::uint8_t>(elf::StT::kObject)) continue;
    pr.add("Globals");
    GlobalDef g;
    g.name_off = model.arena_add(s.name);
    g.addr = s.value;
    g.size = s.size;
    model.globals.push_back(g);
    ++st.globals;
  }

  // --- vtable symbols give class names and method tables ---
  // A `_ZTV<name>` symbol marks a vtable; its size divided by the pointer size
  // is the slot count, which is a real, derived fact about the class.
  const std::uint64_t ptr = 8;
  for (const elf::Symbol& s : elf.symbols()) {
    if (s.name.empty() || s.value == 0) continue;
    if (s.name.compare(0, 4, "_ZTV") != 0) continue;
    const std::string path = mangled_class_path(s.name.substr(4));
    if (path.empty() || s.size < ptr) continue;
    ClassDef cd;
    // Names live in the symbol arena, not .debug_str; record both so the
    // emitter can print either.
    cd.name_off = model.arena_add(path);
    cd.from_arena = 1;
    cd.size = s.size;
    cd.vtable_addr = s.value;
    // The first two words of a vtable are the header (offset-to-top and the
    // typeinfo pointer), not virtual functions. Excluding them is what makes
    // the count comparable with other dumpers: e.g. 205 words -> 203, of which
    // 201 are real slots once the header is removed.
    cd.vtable_words = s.size / ptr;
    cd.vtable_slots = cd.vtable_words > 2 ? cd.vtable_words - 2 : 0;
    const auto idx = static_cast<std::int32_t>(model.classes.size());
    model.classes.push_back(cd);
    ++st.classes_from_rtti;
    st.vtable_slots += cd.vtable_slots;
  }

  st.seconds = diag::seconds_since(t_start);
  if (stats != nullptr) *stats = st;
  return true;
}

}  // namespace c2d::ir
