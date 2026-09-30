// SPDX-License-Identifier: MIT
// Stellar -- native ELF/DWARF dumper.
//
// ELF validation, DWARF unit discovery, streaming DIE traversal, model
// construction and C# dump generation.
#include <cerrno>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "stellar/diag/log.h"
#include "stellar/diag/metrics.h"
#include "stellar/diag/progress.h"
#include "stellar/dwarf/constants.h"
#include "stellar/dwarf/dwarf_context.h"
#include "stellar/elf/elf_file.h"
#include "stellar/ir/build.h"
#include "stellar/output/emit.h"
#include "stellar/util/bytes.h"

// Single source of truth for the version: CMake passes these in.
#ifndef STELLAR_VERSION
#define STELLAR_VERSION "0.0.0-dev"
#endif
#ifndef STELLAR_VERSION_MAJOR
#define STELLAR_VERSION_MAJOR 0
#endif
#ifndef STELLAR_VERSION_MINOR
#define STELLAR_VERSION_MINOR 0
#endif
#ifndef STELLAR_VERSION_PATCH
#define STELLAR_VERSION_PATCH 0
#endif

namespace {

using namespace stellar;

struct Options {
  std::string command;
  std::string path;
  dwarf::ScanLimits limits;
  bool stats = false;
  bool show_tags = false;
  std::uint64_t dump_unit_index = 0;
  std::string log_level = "info";
  std::size_t max_print = 40;
  // `emit` options
  std::string out_path;
  std::string target_name;
  bool emit_bases = false;
  std::uint64_t max_lines = 0;
  std::uint64_t max_build_units = 0;
  std::uint64_t list_conflicts = 0;
  bool pad_layout = true;
  bool emit_methods = true;
  std::string mode = "auto";       // auto | dwarf | dwarfless
  std::string progress_mode = "auto";  // auto | always | never
  int progress_interval = 80;
  bool fail_on_low_confidence = false;
};

/// mkdir -p, for the default output location.
int ensure_directory(const std::string& path) {
  if (path.empty()) return 0;
  std::string acc;
  std::size_t i = 0;
  if (path[0] == '/') { acc = "/"; i = 1; }
  while (i <= path.size()) {
    const std::size_t j = path.find('/', i);
    const std::string part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!part.empty()) {
      if (!acc.empty() && acc.back() != '/') acc += '/';
      acc += part;
  #if defined(_WIN32)
    if (::_mkdir(acc.c_str()) != 0 && errno != EEXIST) return 1;
#else
    if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return 1;
#endif
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return 0;
}

ir::Model metrics_model_;

void usage() {
  std::printf(
      "Stellar " STELLAR_VERSION " -- native ELF/DWARF dumper\n"
      "\n"
      "Usage:\n"
      "  stellar info   [options] <elf>        ELF + DWARF summary\n"
      "  stellar units  [options] <elf>        enumerate compilation units\n"
      "  stellar scan   [options] <elf>        walk DIEs, count tags, measure speed\n"
      "  stellar dump   [options] <elf>        print the DIE tree of one unit\n"
      "  stellar emit   [options] <elf>        build the model and write the dump\n"
      "\n"
      "Options:\n"
      "  --max-units N      stop after N units (0 = all)\n"
      "  --max-dies N       stop after N DIEs overall (0 = all)\n"
      "  --first-unit N     start at unit index N\n"
      "  --unit-stride N    visit every Nth unit (bounded sampling)\n"
      "  --unit N           unit index for 'dump' (default 0)\n"
      "  --tags             print the tag histogram\n"
      "  --stats            print measured timings, RSS and counts\n"
      "  --max-print N      cap printed output lines (default 40)\n"
      "  -o, --out FILE     output file (default output/dump.cs)\n"
      "  --name NAME        target name recorded in the dump header\n"
      "  --emit-bases       also emit a base-class comment per class\n"
      "  --mode M           auto (default) | dwarf | dwarfless\n"
      "  --progress M       auto (default, when stderr is a tty) | always | never\n"
      "  --progress-interval MS   minimum gap between redraws (default 80)\n"
      "  --fail-on-low-confidence  exit 3 when a dump is inferred, not DWARF\n"
      "  --no-pad           do not synthesise padding fields for layout gaps\n"
      "  --no-methods       do not emit member functions\n"
      "  --max-lines N      cap emitted lines (0 = all)\n"
      "  --build-units N    build the model from only the first N units\n"
      "  --log-level LEVEL  trace|debug|info|warn|error|off (default info)\n"
      "  -h, --help         this text\n"
      "\n"
      "Examples:\n"
      "  stellar info  libcocos2dcpp_1.74.2.so\n"
      "  stellar scan  --max-units 20 --tags --stats libcocos2dcpp_1.74.2.so\n"
      "  stellar scan  --unit-stride 100 --stats libcocos2dcpp_1.74.2.so\n"
      "  stellar dump  --unit 0 --max-print 200 libcocos2dcpp_1.74.2.so\n"
      "  stellar emit  --stats libcocos2dcpp_1.74.2.so\n"
      "  stellar emit  --stats -o /tmp/small.cs --max-lines 50000 libcocos2dcpp_1.74.2.so\n");
}

bool parse_u64(const char* s, std::uint64_t& out) {
  if (s == nullptr || *s == '\0') return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || (end != nullptr && *end != '\0')) return false;
  out = v;
  return true;
}

bool parse_args(int argc, char** argv, Options& o) {
  if (argc < 2) return false;
  // Holds the value from a `--opt=value` form until the option consumes it.
  std::string pending_eq_value;
  bool has_pending_eq_value = false;
  std::uint64_t tmp_interval = 0;
  int i = 1;
  o.command = argv[i++];
  if (o.command == "-h" || o.command == "--help") {
    o.command = "help";
    return true;
  }
  if (o.command == "-V" || o.command == "--version") {
    o.command = "version";
    return true;
  }

  for (; i < argc; ++i) {
    std::string a = argv[i];
    // Accept both `--opt value` and `--opt=value` for every long option.
    if (a.size() > 2 && a.compare(0, 2, "--") == 0) {
      const std::size_t eq = a.find('=');
      if (eq != std::string::npos && eq + 1 <= a.size()) {
        // Rewrite in place as two arguments so the table below needs no special
        // cases: "--mode=x" becomes "--mode" "x".
        static std::vector<std::string> rewrite;
        rewrite.clear();
        rewrite.push_back(a.substr(0, eq));
        rewrite.push_back(a.substr(eq + 1));
        a = rewrite[0];
        pending_eq_value = rewrite[1];
        has_pending_eq_value = true;
      }
    }
    auto need = [&](const char* name) -> const char* {
      if (has_pending_eq_value) {
        has_pending_eq_value = false;
        return pending_eq_value.c_str();
      }
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      o.command = "help";
      return true;
    } else if (a == "--max-units") {
      const char* v = need("--max-units");
      if (v == nullptr || !parse_u64(v, o.limits.max_units)) return false;
    } else if (a == "--max-dies") {
      const char* v = need("--max-dies");
      if (v == nullptr || !parse_u64(v, o.limits.max_dies)) return false;
    } else if (a == "--first-unit") {
      const char* v = need("--first-unit");
      if (v == nullptr || !parse_u64(v, o.limits.first_unit)) return false;
    } else if (a == "--unit-stride") {
      const char* v = need("--unit-stride");
      if (v == nullptr || !parse_u64(v, o.limits.unit_stride)) return false;
    } else if (a == "--unit") {
      const char* v = need("--unit");
      if (v == nullptr || !parse_u64(v, o.dump_unit_index)) return false;
    } else if (a == "-o" || a == "--out") {
      const char* v = need("-o");
      if (v == nullptr) return false;
      o.out_path = v;
    } else if (a == "--name") {
      const char* v = need("--name");
      if (v == nullptr) return false;
      o.target_name = v;
    } else if (a == "--emit-bases") {
      o.emit_bases = true;
    } else if (a == "--mode") {
      const char* v = need("--mode");
      if (v == nullptr) return false;
      const std::string m = v;
      if (m != "auto" && m != "dwarf" && m != "dwarfless") {
        std::fprintf(stderr, "error: --mode must be auto, dwarf or dwarfless\n");
        return false;
      }
      o.mode = m;
    } else if (a == "--fail-on-low-confidence") {
      o.fail_on_low_confidence = true;
    } else if (a == "--progress") {
      const char* v = need("--progress");
      if (v == nullptr) return false;
      const std::string m = v;
      if (m != "auto" && m != "always" && m != "never") {
        std::fprintf(stderr, "error: --progress must be auto, always or never\n");
        return false;
      }
      o.progress_mode = m;
    } else if (a == "--progress-interval") {
      const char* v = need("--progress-interval");
      if (v == nullptr || !parse_u64(v, tmp_interval) || tmp_interval > 10000) return false;
      o.progress_interval = static_cast<int>(tmp_interval);
    } else if (a == "--no-pad") {
      o.pad_layout = false;
    } else if (a == "--no-methods") {
      o.emit_methods = false;
    } else if (a == "--max-lines") {
      const char* v = need("--max-lines");
      std::uint64_t n = 0;
      if (v == nullptr || !parse_u64(v, n)) return false;
      o.max_lines = n;
    } else if (a == "--list-conflicts") {
      const char* v = need("--list-conflicts");
      if (v == nullptr || !parse_u64(v, o.list_conflicts)) return false;
    } else if (a == "--build-units") {
      const char* v = need("--build-units");
      if (v == nullptr || !parse_u64(v, o.max_build_units)) return false;
    } else if (a == "--max-print") {
      const char* v = need("--max-print");
      std::uint64_t n = 0;
      if (v == nullptr || !parse_u64(v, n)) return false;
      o.max_print = static_cast<std::size_t>(n);
    } else if (a == "--tags") {
      has_pending_eq_value = false;
      o.show_tags = true;
    } else if (a == "--stats") {
      has_pending_eq_value = false;
      o.stats = true;
    } else if (a == "--log-level") {
      const char* v = need("--log-level");
      if (v == nullptr) return false;
      o.log_level = v;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
      return false;
    } else if (!o.path.empty()) {
      std::fprintf(stderr, "error: unexpected extra argument %s\n", a.c_str());
      return false;
    } else {
      o.path = a;
    }
  }
  // `help` and `version` are the only commands that take no input.
  if (o.path.empty() && o.command != "help" && o.command != "version") {
    std::fprintf(stderr, "error: no input file given\n");
    return false;
  }
  return true;
}


// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

int cmd_info(const elf::ElfFile& elf) {
  const dwarf::Sections probe(elf);
  std::printf("file            : %s\n", elf.path().c_str());
  std::printf("size            : %s (%llu bytes)\n",
              util::human_size(elf.file_size()).c_str(),
              static_cast<unsigned long long>(elf.file_size()));
  std::printf("format          : %s\n", elf.describe().c_str());
  std::printf("machine         : %s\n", elf::machine_name(elf.header().e_machine));
  std::printf("sections        : %zu\n", elf.sections().size());
  std::printf("program headers : %zu\n", elf.program_headers().size());
  std::printf("symbols         : %zu (.symtab %s, .dynsym %s)\n", elf.symbols().size(),
              elf.has_symtab() ? "yes" : "no", elf.has_dynsym() ? "yes" : "no");
  std::printf("debug sections  : %zu (%s)\n", probe.present_debug_sections().size(),
              util::human_size(probe.total_debug_bytes()).c_str());
  for (const auto& [name, size] : probe.present_debug_sections()) {
    std::printf("    %-20s %12s\n", name.c_str(), util::human_size(size).c_str());
  }
  std::printf("capabilities    : %s\n", probe.capability_report().c_str());
  if (!probe.has_info()) {
    std::printf("\nNo .debug_info/.debug_abbrev: nothing further can be parsed.\n");
    return 2;
  }
  dwarf::DwarfContext ctx(elf);
  std::printf("compilation units: %llu\n",
              static_cast<unsigned long long>(ctx.unit_count()));
  std::printf("\n(use `stellar units` for per-unit detail, `stellar scan` to traverse DIEs)\n");
  return 0;
}

int cmd_units(dwarf::DwarfContext& ctx, const Options& o) {
  const std::uint64_t total = ctx.unit_count();
  std::printf("%-8s %-12s %-5s %-5s %-12s %-10s\n", "index", "offset", "ver", "addr",
              "abbrev_off", "dies");
  dwarf::DwarfContext::UnitIterator it(ctx, o.limits);
  dwarf::UnitHeader h;
  std::string err;
  std::size_t shown = 0;
  while (it.next(h, &err)) {
    if (shown++ >= o.max_print) {
      std::printf("... (more units remain; raise --max-print)\n");
      break;
    }
    const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
    std::uint64_t dies = 0;
    if (ab != nullptr) {
      dwarf::UnitWalker w(ctx.info(), h, ab);
      w.reset();
      dwarf::Die d;
      while (w.next(d)) ++dies;
    }
    std::printf("%-8llu 0x%08llx  %-5u %-5u 0x%08llx   %-10llu\n",
                static_cast<unsigned long long>(it.index()),
                static_cast<unsigned long long>(h.offset), h.version, h.address_size,
                static_cast<unsigned long long>(h.abbrev_offset),
                static_cast<unsigned long long>(dies));
  }
  std::printf("\ntotal units in .debug_info: %llu; visited %llu",
              static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(it.visited()));
  if (it.skipped_errors() != 0) {
    std::printf("; %llu unparsable units skipped",
                static_cast<unsigned long long>(it.skipped_errors()));
  }
  std::printf("\n");
  return it.skipped_errors() == 0 ? 0 : 1;
}

int cmd_scan(dwarf::DwarfContext& ctx, const Options& o, diag::Metrics& m) {
  dwarf::WalkStats st;
  const auto t0 = diag::Clock::now();

  dwarf::DwarfContext::UnitIterator it(ctx, o.limits);
  dwarf::UnitHeader h;
  std::string err;
  bool hit_die_limit = false;

  while (!hit_die_limit && it.next(h, &err)) {
    const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
    if (ab == nullptr) {
      ++st.failed_units;
      continue;
    }
    dwarf::UnitWalker w(ctx.info(), h, ab);
    w.reset();
    dwarf::Die d;
    while (w.next(d)) {
      ++st.dies;
      if (d.tag() < st.tag_counts.size()) ++st.tag_counts[d.tag()];
      if (d.depth() > st.max_depth) st.max_depth = d.depth();
      if (o.limits.max_dies != 0 && st.dies >= o.limits.max_dies) {
        hit_die_limit = true;
        break;
      }
    }
    st.bytes_scanned += h.body_size();
    ++st.units;
  }
  // Stop the in-place line before the report, so the report is not written over
  // the status text.
  diag::progress().finish();
  const double elapsed = diag::seconds_since(t0);

  std::printf("scanned units   : %llu\n", static_cast<unsigned long long>(st.units));
  std::printf("scanned DIEs    : %llu\n", static_cast<unsigned long long>(st.dies));
  std::printf("max depth       : %llu\n", static_cast<unsigned long long>(st.max_depth));
  std::printf("bytes traversed : %s\n", util::human_size(st.bytes_scanned).c_str());
  if (st.failed_units != 0) {
    std::printf("failed units    : %llu\n", static_cast<unsigned long long>(st.failed_units));
  }
  if (it.skipped_errors() != 0) {
    std::printf("skipped units   : %llu (unparsable header)\n",
                static_cast<unsigned long long>(it.skipped_errors()));
  }
  if (hit_die_limit) std::printf("note            : stopped early at --max-dies limit\n");

  const double mbps =
      elapsed > 0 ? static_cast<double>(st.bytes_scanned) / (1024.0 * 1024.0) / elapsed : 0.0;
  std::printf("elapsed         : %.3f s (%.1f MB/s of .debug_info)\n", elapsed, mbps);

  if (o.show_tags) {
    std::printf("\n%-8s %-28s %12s %7s\n", "tag", "name", "count", "share");
    for (std::size_t t = 0; t < st.tag_counts.size(); ++t) {
      if (st.tag_counts[t] == 0) continue;
      char hex[16];
      std::snprintf(hex, sizeof(hex), "0x%02zx", t);
      const double share =
          st.dies != 0
              ? 100.0 * static_cast<double>(st.tag_counts[t]) / static_cast<double>(st.dies)
              : 0.0;
      std::printf("%-8s %-28s %12llu %6.2f%%\n", hex,
                  std::string(dwarf::tag_name(static_cast<std::uint32_t>(t))).c_str(),
                  static_cast<unsigned long long>(st.tag_counts[t]), share);
    }
  }

  m.add_phase("scan", elapsed, st.dies, st.bytes_scanned);
  m.set_count("units_scanned", st.units);
  m.set_count("dies_scanned", st.dies);
  m.set_count("max_depth", st.max_depth);
  m.set_count("failed_units", st.failed_units);
  m.set_count("abbrev_cache_hits", ctx.abbrev_cache().hits());
  m.set_count("abbrev_cache_misses", ctx.abbrev_cache().misses());
  m.add_peak_rss(diag::peak_rss_bytes());
  return 0;
}

/// Prints a human-readable DIE tree for one unit, resolving names and simple
/// numeric attributes. This doubles as the cross-check surface against
/// readelf / llvm-dwarfdump output.
int cmd_dump(dwarf::DwarfContext& ctx, const Options& o) {
  dwarf::UnitHeader h;
  std::string err;
  if (!ctx.unit_header(o.dump_unit_index, h, &err)) {
    std::fprintf(stderr, "error: unit %llu: %s\n",
                 static_cast<unsigned long long>(o.dump_unit_index), err.c_str());
    return 1;
  }
  const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
  if (ab == nullptr) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }
  std::printf("unit %llu: offset=0x%llx version=%u addr_size=%u abbrev_off=0x%llx abbrevs=%zu\n",
              static_cast<unsigned long long>(o.dump_unit_index),
              static_cast<unsigned long long>(h.offset), h.version, h.address_size,
              static_cast<unsigned long long>(h.abbrev_offset), ab->size());
  std::printf("%-9s %-26s %s\n", "offset", "tag", "attributes");
  std::printf("---------------------------------------------------------------\n");

  dwarf::UnitWalker w(ctx.info(), h, ab);
  w.reset();
  dwarf::Die d;
  std::size_t printed = 0;
  while (w.next(d)) {
    if (printed++ >= o.max_print) {
      std::printf("... (truncated at --max-print %zu)\n", o.max_print);
      break;
    }
    std::string indent(static_cast<std::size_t>(d.depth()) * 2, ' ');
    std::string attrs;
    bool first = true;
    d.for_each_attr([&](const dwarf::AttrValue& v) {
      if (!first) attrs += ", ";
      first = false;
      char buf[256];
      const std::string an(dwarf::attr_name(v.attr));
      if (v.form == dwarf::form::kStrp) {
        const std::string_view sv = ctx.str_at(v.u64);
        std::snprintf(buf, sizeof(buf), "%s=\"%.*s\"", an.c_str(), static_cast<int>(sv.size()),
                      sv.data());
      } else if (v.form == dwarf::form::kSdata) {
        std::snprintf(buf, sizeof(buf), "%s=%lld", an.c_str(), static_cast<long long>(v.i64));
      } else if (dwarf::is_unit_relative_ref(v.form)) {
        const std::uint64_t abs = dwarf::DwarfContext::resolve_unit_ref(d.unit(), v.u64);
        std::snprintf(buf, sizeof(buf), "%s=<0x%llx>", an.c_str(),
                      static_cast<unsigned long long>(abs));
      } else {
        std::snprintf(buf, sizeof(buf), "%s=%llu", an.c_str(),
                      static_cast<unsigned long long>(v.u64));
      }
      attrs += buf;
    });
    std::printf("0x%06llx %s%-26s %s\n", static_cast<unsigned long long>(d.offset()),
                indent.c_str(), std::string(dwarf::tag_name(d.tag())).c_str(), attrs.c_str());
  }
  std::printf("\ntotal DIEs in unit: %llu\n", static_cast<unsigned long long>(w.dies_seen()));
  return 0;
}

/// Builds the model and writes the C#-style dump.
/// The completion summary.
///
/// Composed as a single string and printed through the reporter, which clears
/// any status line first, so the output ends tidy whether or not progress was
/// drawn.
std::string summary_line(const char* mode, const std::string& path, double secs,
                         const ir::BuildStats& bs, const ir::DwarflessStats& ds,
                         const output::EmitStats& es) {
  const auto num = [](std::uint64_t v) {
    char d[24];
    int n = std::snprintf(d, sizeof(d), "%llu", static_cast<unsigned long long>(v));
    std::string out;
    for (int i = 0; i < n; ++i) {
      if (i != 0 && (n - i) % 3 == 0) out.push_back(',');
      out.push_back(d[i]);
    }
    return out;
  };
  char buf[640];
  const std::uint64_t units = bs.units != 0 ? bs.units : 0;
  const std::uint64_t dies = bs.units != 0 ? bs.dies : ds.fdes;
  const std::uint64_t types = bs.type_nodes;
  const std::uint64_t fields = bs.fields;
  const std::uint64_t methods = bs.methods;
  const char* dies_label = bs.units != 0 ? "DIEs" : "FDEs";
  std::snprintf(buf, sizeof(buf),
                "stellar: done (%s) %s in %.1fs -- %s units, %s %s, %s types, %s fields, "
                "%s methods -> %s lines, %s",
                mode, path.c_str(), secs, num(units).c_str(), num(dies).c_str(), dies_label,
                num(types).c_str(), num(fields).c_str(), num(methods).c_str(),
                num(es.lines).c_str(), util::human_size(es.bytes).c_str());
  return buf;
}

int cmd_emit(dwarf::DwarfContext& ctx, const Options& o, diag::Metrics& metrics) {
  // ---- choose a mode -------------------------------------------------------
  const bool have_dwarf = ctx.sections().has_info() && ctx.unit_count() != 0;
  bool use_dwarfless = false;
  if (o.mode == "dwarfless") {
    use_dwarfless = true;
  } else if (o.mode == "dwarf") {
    if (!have_dwarf) {
      std::fprintf(stderr, "error: --mode=dwarf requested but %s has no DWARF\n",
                   o.path.c_str());
      return 2;
    }
  } else {
    use_dwarfless = !have_dwarf;
  }

  const auto t_emit_start = diag::Clock::now();
  ir::BuildStats bst;
  ir::DwarflessStats dst;
  const bool built = use_dwarfless
                         ? ir::build_dwarfless_model(ctx, metrics_model_, &dst)
                         : ir::build_model(ctx, ir::BuildOptions{}, metrics_model_, &bst);
  if (!built) {
    std::fprintf(stderr, "error: could not build the model\n");
    return 1;
  }

  output::EmitOptions eopts;
  eopts.emit_bases = true;
  eopts.pad_layout = o.pad_layout;
  eopts.emit_methods = o.emit_methods;
  eopts.target_name = o.target_name.empty() ? std::string("target.so") : o.target_name;
  eopts.inferred = use_dwarfless;
  if (use_dwarfless) {
    eopts.named_functions = dst.functions_named;
    eopts.unnamed_functions = dst.functions_sub_;
    eopts.vtable_slots = dst.vtable_slots;
    eopts.classes_from_rtti = dst.classes_from_rtti;
  }

  // The filename carries the warning too: a dump this large gets copied,
  // renamed and committed, and the name is the only signal that survives.
  std::string out_path = o.out_path;
  if (out_path.empty()) {
    if (ensure_directory("output") != 0) {
      std::fprintf(stderr, "error: cannot create output/\n");
      return 1;
    }
    out_path = use_dwarfless ? "output/dump.dwarfless.cs" : "output/dump.cs";
  } else {
    const std::size_t slash = out_path.find_last_of('/');
    if (slash != std::string::npos) ensure_directory(out_path.substr(0, slash));
  }
  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "error: cannot open %s for writing\n", out_path.c_str());
    return 1;
  }

  const auto t_emit = diag::Clock::now();
  output::EmitStats est;
  output::emit_il2cpp(metrics_model_, out, eopts, &est);
  const double emit_secs = diag::seconds_since(t_emit);
  std::fclose(out);

  // The human-readable block below goes to stderr too, so the in-place status
  // line has to go first.
  diag::progress().finish();

  if (use_dwarfless) {
    // Loud, and on stderr, because stdout is the pipe and stderr is the
    // terminal the person is actually looking at.
    // Pad to a common inner width so the rules line up whatever the path is.
    const std::string w1 = "  #  WARNING: LOW-ACCURACY OUTPUT - NOT GROUND TRUTH";
    const std::string w2 = "  #  " + o.path + " has no DWARF debug information.";
    const std::string w3 = "  #  Field offsets and field TYPES are NOT recoverable,";
    const std::string w4 = "  #  so they are not guessed. Reconnaissance only.";
    const std::size_t w = std::max({w1.size(), w2.size(), w3.size(), w4.size()});
    const std::string rule(w + 4, '#');
    std::fprintf(stderr, "\n%s\n", rule.c_str());
    std::fprintf(stderr, "%s #\n", (w1 + std::string(w - w1.size(), ' ')).c_str());
    std::fprintf(stderr, "%s #\n", (w2 + std::string(w - w2.size(), ' ')).c_str());
    std::fprintf(stderr, "%s #\n", (w3 + std::string(w - w3.size(), ' ')).c_str());
    std::fprintf(stderr, "%s #\n", (w4 + std::string(w - w4.size(), ' ')).c_str());
    std::fprintf(stderr, "%s\n\n", rule.c_str());
    std::fprintf(stderr,
                 "  functions : %llu named from symbols, %llu as sub_<addr> (%.2fs)\n"
                 "  vtables   : %llu classes, %llu slots\n"
                 "  globals   : %llu\n",
                 static_cast<unsigned long long>(dst.functions_named),
                 static_cast<unsigned long long>(dst.functions_sub_), dst.seconds,
                 static_cast<unsigned long long>(dst.classes_from_rtti),
                 static_cast<unsigned long long>(dst.vtable_slots),
                 static_cast<unsigned long long>(dst.globals));
    metrics.add_phase("build_dwarfless", dst.seconds, dst.fdes);
    metrics.set_count("functions_named", dst.functions_named);
    metrics.set_count("functions_unnamed", dst.functions_sub_);
    metrics.set_count("classes_from_rtti", dst.classes_from_rtti);
    metrics.set_count("vtable_slots", dst.vtable_slots);
  } else {
    std::fprintf(stderr,
                 "model: %llu units, %llu dies, %llu type nodes, %llu classes, %llu enums, "
                 "%llu fields, %llu enumerators, %llu unresolved refs (%.2fs)\n",
                 static_cast<unsigned long long>(bst.units),
                 static_cast<unsigned long long>(bst.dies),
                 static_cast<unsigned long long>(bst.type_nodes),
                 static_cast<unsigned long long>(bst.classes),
                 static_cast<unsigned long long>(bst.enums),
                 static_cast<unsigned long long>(bst.fields),
                 static_cast<unsigned long long>(bst.enumerators),
                 static_cast<unsigned long long>(bst.unresolved_refs), bst.seconds);
    std::fprintf(stderr,
                 "         %llu methods, %llu of them with a code address recovered\n",
                 static_cast<unsigned long long>(bst.methods),
                 static_cast<unsigned long long>(bst.methods_with_addr));
    metrics.add_phase("build_model", bst.seconds, bst.dies);
  }
  std::fprintf(stderr,
               "output: %llu enums, %llu classes, %llu structs, %llu unions, %llu fields, "
               "%llu methods, %llu functions, %llu globals, %llu lines, %s (%.2fs)\n",
               static_cast<unsigned long long>(est.enums),
               static_cast<unsigned long long>(est.classes),
               static_cast<unsigned long long>(est.structs),
               static_cast<unsigned long long>(est.unions),
               static_cast<unsigned long long>(est.fields),
               static_cast<unsigned long long>(est.methods),
               static_cast<unsigned long long>(est.functions),
               static_cast<unsigned long long>(est.globals),
               static_cast<unsigned long long>(est.lines),
               util::human_size(est.bytes).c_str(), emit_secs);
  std::fprintf(stderr, "wrote: %s\n", out_path.c_str());

  diag::progress().finish_line(summary_line(use_dwarfless ? "dwarfless" : "dwarf", o.path,
                                            diag::seconds_since(t_emit_start), bst, dst, est));

  metrics.add_phase("emit", emit_secs, est.lines, est.bytes);
  metrics.set_count("classes", metrics_model_.classes.size());
  metrics.set_count("enums", metrics_model_.enums.size());
  metrics.set_count("fields", metrics_model_.fields.size());
  metrics.set_count("type_nodes", metrics_model_.types.size());
  metrics.set_count("output_bytes", est.bytes);
  metrics.set_count("output_lines", est.lines);
  metrics.add_peak_rss(diag::peak_rss_bytes());
  return use_dwarfless && o.fail_on_low_confidence ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) {
    usage();
    return 64;
  }
  if (o.command == "help") {
    usage();
    return 0;
  }
  if (o.command == "version") {
    // Reported as both a dotted version and a semver string so packaging and
    // humans get what they expect.
    std::printf("%s\n", STELLAR_VERSION);
    std::printf("Stellar %d.%d.%d\n", STELLAR_VERSION_MAJOR, STELLAR_VERSION_MINOR, STELLAR_VERSION_PATCH);
    return 0;
  }
  if (!diag::Log::set_level_from_string(o.log_level)) {
    std::fprintf(stderr, "error: unknown log level '%s'\n", o.log_level.c_str());
    return 64;
  }

  auto& pr = diag::progress();
  pr.reset();
  pr.configure(o.progress_mode == "always", o.progress_interval);
  if (o.progress_mode == "never") pr.set_enabled(false);

  const auto t_start = diag::Clock::now();
  diag::Metrics metrics;
  metrics.set_label("stellar " + o.command + " " + o.path);

  // --- open + validate the container --------------------------------------
  const auto t_open = diag::Clock::now();
  elf::ElfFile elf;
  std::string err;
  if (!elf.open(o.path, &err)) {
    std::fprintf(stderr, "error: %s: %s\n", o.path.c_str(), err.c_str());
    return 1;
  }
  metrics.add_phase("open", diag::seconds_since(t_open), 1, elf.file_size());
  STELLAR_DEBUG("%s: %s", elf.path().c_str(), elf.describe().c_str());

  if (o.command == "info") {
    const int rc = cmd_info(elf);
    if (o.stats) std::fputs(metrics.report().c_str(), stdout);
    return rc;
  }

  // --- DWARF detection + unit discovery ------------------------------------
  const auto t_dwarf = diag::Clock::now();
  dwarf::DwarfContext ctx(elf);
  // `emit` handles the no-DWARF case itself (dwarfless mode), so only the
  // DWARF-dependent commands are gated here.
  const bool emit_cmd = o.command == "emit";
  if (!ctx.sections().has_info() && !emit_cmd) {
    std::fprintf(stderr, "error: %s has no .debug_info/.debug_abbrev\n", o.path.c_str());
    return 2;
  }
  const std::uint64_t unit_total = ctx.unit_count();
  metrics.add_phase("unit_discovery", diag::seconds_since(t_dwarf), unit_total, ctx.info().size());

  int rc = 0;
  if (o.command == "units") {
    rc = cmd_units(ctx, o);
  } else if (o.command == "scan") {
    rc = cmd_scan(ctx, o, metrics);
  } else if (o.command == "dump") {
    rc = cmd_dump(ctx, o);
  } else if (o.command == "emit") {
    rc = cmd_emit(ctx, o, metrics);
  } else {
    std::fprintf(stderr, "error: unknown command '%s'\n", o.command.c_str());
    usage();
    rc = 64;
  }

  if (o.stats) {
    metrics.set_count("units_total", unit_total);
    metrics.add_peak_rss(diag::peak_rss_bytes());
    const diag::ResourceReport res = diag::resource_report(diag::seconds_since(t_start));
    std::fputs(metrics.report().c_str(), stdout);
    std::printf("  wall_seconds = %.3f\n", res.wall_seconds);
    std::printf("  user_seconds = %.3f\n", res.user_seconds);
    std::printf("  sys_seconds  = %.3f\n", res.system_seconds);
  }
  return rc;
}
