// SPDX-License-Identifier: MIT
#include "stellar/tui/analysis.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "stellar/diag/progress.h"
#include "stellar/dwarf/dwarf_context.h"
#include "stellar/elf/elf_file.h"
#include "stellar/elf/elf_types.h"
#include "stellar/ir/build.h"
#include "stellar/output/emit.h"
#include "stellar/util/bytes.h"

namespace stellar::tui {
namespace {

/// How often the live counters are read back, in milliseconds.
///
/// The same cadence the reporter redraws at: sampling faster only buys jitter,
/// and the counters are read by parsing a rendered line rather than by asking
/// the reporter, so every sample is a small allocation.
constexpr double kSampleIntervalMs = 80.0;

/// Extracts one counter from a rendered progress line.
///
/// `Progress` exposes no getter for a counter's *value* (its counters are a
/// private cache that only the hot path writes), and it is not thread-safe, so
/// the rendered line is the only thing that can be read from the outside. The
/// line looks like
///
///   [ 24%] DIEs: 12,450/50,000 | Types: 1,203 | Stage: Resolving types
///
/// so a match has to start at a field boundary -- otherwise "Types" would also
/// be found inside some future counter's name -- and the digits that follow may
/// carry thousands separators. Returns false when the label is absent, which is
/// normal: a counter only exists once the core has declared it.
bool parse_counter(const std::string& line, const char* label, std::uint64_t& out) {
  const std::size_t n = std::strlen(label);
  std::size_t at = 0;
  while ((at = line.find(label, at)) != std::string::npos) {
    const char before = at == 0 ? '\0' : line[at - 1];
    const bool boundary = at == 0 || before == '|' || before == ']' || before == ' ';
    if (boundary && line.compare(at + n, 2, ": ") == 0) {
      std::uint64_t value = 0;
      bool any = false;
      for (std::size_t i = at + n + 2; i < line.size(); ++i) {
        const char c = line[i];
        if (c >= '0' && c <= '9') {
          value = value * 10 + static_cast<std::uint64_t>(c - '0');
          any = true;
        } else if (c != ',') {
          break;  // the value ends at '/' (a total) or a separator
        }
      }
      if (any) {
        out = value;
        return true;
      }
      return false;
    }
    at += n;
  }
  return false;
}

/// The text after "Stage: " on a rendered progress line, or empty.
///
/// The core names its own steps ("Resolving types", "Deduplicating"), and that
/// is strictly more informative than anything this module could invent, so it is
/// what the status line shows while a core call is running.
std::string parse_stage(const std::string& line) {
  constexpr const char* kKey = "Stage: ";
  const std::size_t at = line.find(kKey);
  if (at == std::string::npos) return {};
  const std::size_t key = std::strlen(kKey);
  const std::size_t end = line.find(" | ", at);
  // npos as a length means "to the end of the string", which is what a stage
  // with no note after it needs.
  return line.substr(at + key, end == std::string::npos ? end : end - at - key);
}

/// mkdir -p for the parent of the output file.
///
/// The same loop the CLI uses (src/app/main.cpp) rather than <filesystem>:
/// the project builds on toolchains where linking the filesystem library is a
/// separate decision, and this keeps the TUI free of one.
int ensure_directory(const std::string& path) {
  if (path.empty()) return 0;
  std::string acc;
  std::size_t i = 0;
  if (path[0] == '/' || path[0] == '\\') {
    acc = path.substr(0, 1);
    i = 1;
  }
  while (i <= path.size()) {
    const std::size_t j = path.find_first_of("/\\", i);
    const std::string part =
        path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!part.empty()) {
      if (!acc.empty() && acc.back() != '/' && acc.back() != '\\') acc += '/';
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

/// The file name without its directory, used as the default target name.
std::string basename_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

Analysis::~Analysis() noexcept {
  // A UI can be torn down mid-dump; the worker is joinable, so waiting here is
  // the only way to guarantee it is not touching the snapshot (or the model, or
  // the output file) after this object is gone. join() cannot throw.
  join();
}

const char* Analysis::phase_name(AnalysisSnapshot::Phase p) noexcept {
  switch (p) {
    case AnalysisSnapshot::Phase::kIdle: return "idle";
    case AnalysisSnapshot::Phase::kOpening: return "opening";
    case AnalysisSnapshot::Phase::kDiscovering: return "discovering";
    case AnalysisSnapshot::Phase::kBuilding: return "building";
    case AnalysisSnapshot::Phase::kEmitting: return "emitting";
    case AnalysisSnapshot::Phase::kDone: return "done";
    case AnalysisSnapshot::Phase::kFailed: return "failed";
    case AnalysisSnapshot::Phase::kCancelled: return "cancelled";
  }
  return "unknown";
}

void Analysis::request_cancel() noexcept {
  // Relaxed is enough: the flag is a hint, and the worker re-reads it at every
  // phase and unit boundary rather than reacting to the write itself.
  cancel_.store(true, std::memory_order_relaxed);
}

bool Analysis::running() const noexcept {
  return worker_.joinable() && !done_.load(std::memory_order_acquire);
}

bool Analysis::done() const noexcept { return done_.load(std::memory_order_acquire); }

AnalysisSnapshot Analysis::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  AnalysisSnapshot copy = snap_;
  // The one number that can be brought up to date from the reading side: a run
  // in flight is almost always inside a core call that cannot be polled, and a
  // status line whose clock has stopped is what makes a 20-second build look
  // like a hang. t0_ is written by start() before the worker exists and is
  // never touched again, so this needs no second thread and no new lock.
  if (running()) copy.elapsed_seconds = diag::seconds_since(t0_);

  // Re-read the core's counters here rather than trusting the worker's last
  // stamp. Progress's counters are relaxed atomics, so this is safe from this
  // thread while the worker is inside build_model(), and it is the difference
  // between a live counter and one frozen for the length of that call. Only the
  // worker's own mutating calls (declare/add/set/stage) stay on its thread.
  if (copy.phase == AnalysisSnapshot::Phase::kBuilding ||
      copy.phase == AnalysisSnapshot::Phase::kEmitting) {
    const auto& pr = diag::progress();
    std::uint64_t v = 0;
    std::uint64_t total = 0;
    if (pr.get("Units", v)) copy.units = v;
    if (pr.get("Units", v, &total, nullptr) && total != 0) copy.units_total = total;
    if (pr.get("DIEs", v)) copy.dies = v;
    if (pr.get("DIEs", v, &total, nullptr) && total != 0) copy.dies_total = total;
    if (pr.get("Types", v)) copy.types = v;
    if (pr.get("Fields", v)) copy.fields = v;
    if (pr.get("Methods", v)) copy.methods = v;
    if (pr.get("Lines", v)) copy.out_lines = v;
    // The core reports no per-unit note while it builds, but its Units counter
    // is live, so the current operation is derived from it rather than left
    // showing whichever unit the worker happened to be on at the boundary. Same
    // for RSS, which is a /proc read and cheap enough at frame rate.
    if (copy.phase == AnalysisSnapshot::Phase::kBuilding && copy.units_total != 0) {
      copy.current_note = "compilation unit " + std::to_string(copy.units) + " of " +
                          std::to_string(copy.units_total);
      copy.rss_bytes = diag::current_rss_bytes();
    }
  }
  return copy;
}

diag::Metrics Analysis::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
}

void Analysis::join() noexcept {
  if (!worker_.joinable()) return;
  // Defensive: joining from the worker itself would deadlock. Nothing in the
  // current call graph does that, but the destructor is the last line of
  // defence and a hang there is undebuggable.
  if (worker_.get_id() == std::this_thread::get_id()) return;
  worker_.join();
}

bool Analysis::wait_for_finish(int timeout_ms) {
  std::unique_lock<std::mutex> lock(done_mutex_);
  const auto finished = [this] { return done_.load(std::memory_order_acquire); };
  if (finished()) return true;
  if (timeout_ms < 0) {
    done_cv_.wait(lock, finished);
    return true;
  }
  return done_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), finished);
}

bool Analysis::start(const StartOptions& o) {
  if (o.input_path.empty()) return false;
  if (worker_.joinable()) {
    // A finished run is reaped here rather than in the destructor: the thread
    // has already returned, so this only releases the handle. A run that is
    // still going is a hard error, not a queue.
    if (!done_.load(std::memory_order_acquire)) return false;
    worker_.join();
  }

  cancel_.store(false, std::memory_order_release);
  done_.store(false, std::memory_order_release);
  // Fixed before the worker exists and never written again, so the UI may read
  // it to keep its clock ticking while the worker is inside a core call.
  t0_ = diag::Clock::now();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = AnalysisSnapshot{};
    snap_.out_path = o.out_path;
    snap_.mode_label = o.mode;
    metrics_ = diag::Metrics{};
    metrics_.set_label("stellar tui " + o.input_path);
  }

  // The reporter is process-wide and the TUI owns the terminal, so drawing is
  // switched off here, before the worker exists: the first core call inside it
  // would otherwise rewrite a row of the UI. Counting stays on, because those
  // counters are where the live numbers come from. No other thread can be
  // touching the reporter at this point -- a run in progress was rejected above
  // and a finished one has been joined.
  diag::Progress& pr = diag::progress();
  pr.set_draw_enabled(false);
  pr.set_enabled(true);
  pr.reset();

  try {
    worker_ = std::thread(&Analysis::run, this, o);
  } catch (const std::exception& e) {
    // Out of threads, most likely. Reported like any other failure rather than
    // escaping into the TUI's draw loop.
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.phase = AnalysisSnapshot::Phase::kFailed;
    snap_.stage = "Failed";
    snap_.error = std::string("cannot start the analysis thread: ") + e.what();
    done_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void Analysis::run(StartOptions o) {
  const auto t0 = diag::Clock::now();

  // The dump is written beside its destination and renamed only once the emitter
  // has returned. A cancelled or failed run must never leave a truncated file
  // where the next run will silently overwrite it, and the UI shows a path.
  std::string partial;

  /// Applies a change to the snapshot under the lock. Every write to the shared
  /// state goes through here or through settle(), which is what makes
  /// snapshot() safe to call on the UI thread at any moment.
  const auto publish = [this, t0](auto&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    fn(snap_);
    snap_.elapsed_seconds = diag::seconds_since(t0);
  };

  /// Phase timings, taken under the same lock as the snapshot because metrics()
  /// hands out a copy of the very same object.
  const auto record = [this](std::string name, double secs, std::uint64_t count,
                             std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.add_phase(std::move(name), secs, count, bytes);
  };
  const auto count = [this](const char* key, std::uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.set_count(key, value);
  };

  const auto cancelled = [this] { return cancel_.load(std::memory_order_relaxed); };

  /// Reaches a terminal phase and, unless the run completed, throws away the
  /// half-written dump.
  const auto settle = [&](AnalysisSnapshot::Phase phase, std::string error) {
    if (phase != AnalysisSnapshot::Phase::kDone && !partial.empty()) {
      std::remove(partial.c_str());
      partial.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.phase = phase;
    snap_.error = std::move(error);
    snap_.current_note.clear();
    if (phase == AnalysisSnapshot::Phase::kDone) {
      snap_.stage = "Done";
    } else if (phase == AnalysisSnapshot::Phase::kCancelled) {
      snap_.stage = "Cancelled";
    } else if (phase == AnalysisSnapshot::Phase::kFailed) {
      snap_.stage = "Failed";
    }
    metrics_.add_peak_rss(diag::peak_rss_bytes());
  };

  /// Reads the core's live counters back into the snapshot, at most every
  /// kSampleIntervalMs.
  ///
  /// The two calls that dominate a run -- build_model() and emit_il2cpp() -- own
  /// their loops and cannot be polled from outside, so while one of them is on
  /// the stack the numbers are necessarily the ones read just before it started.
  /// Everything between them is sampled, and the reporter's own stage name keeps
  /// the UI honest about which core step is running.
  diag::Clock::time_point last_sample{};
  const auto sample = [&](std::string note) {
    if (diag::ms_since(last_sample) < kSampleIntervalMs) return;
    last_sample = diag::Clock::now();
    // Only ever called on the worker thread: the reporter is not thread-safe,
    // and this is the one thread allowed to read it.
    const std::string line = diag::progress().render();
    std::uint64_t v = 0;
    publish([&](AnalysisSnapshot& s) {
      if (parse_counter(line, "Units", v)) s.units = v;
      if (parse_counter(line, "DIEs", v)) s.dies = v;
      if (parse_counter(line, "Types", v)) s.types = v;
      if (parse_counter(line, "Fields", v)) s.fields = v;
      if (parse_counter(line, "Methods", v)) s.methods = v;
      if (parse_counter(line, "Lines", v)) s.out_lines = v;
      const std::string core_stage = parse_stage(line);
      s.current_note = core_stage.empty() ? std::move(note) : core_stage;
      s.rss_bytes = diag::current_rss_bytes();
    });
  };

  // The work itself is a lambda so that every early return still falls through
  // to the done_ notification at the end of run().
  const auto body = [&]() {
    publish([&](AnalysisSnapshot& s) {
      s.phase = AnalysisSnapshot::Phase::kOpening;
      s.stage = "Opening ELF";
      s.current_note = o.input_path;
    });

    elf::ElfFile elf;
    std::string err;
    const auto t_open = diag::Clock::now();
    if (!elf.open(o.input_path, &err)) {
      settle(AnalysisSnapshot::Phase::kFailed, o.input_path + ": " + err);
      return;
    }
    record("open", diag::seconds_since(t_open), 1, elf.file_size());
    publish([&](AnalysisSnapshot& s) {
      s.file.valid = true;
      s.file.path = elf.path();
      s.file.format = elf.describe();
      s.file.machine = elf::machine_name(elf.header().e_machine);
      s.file.size_text = util::human_size(elf.file_size());
      s.file.file_size = elf.file_size();
      s.file.endianness = elf.is_little_endian() ? "little" : "big";
    });
    if (cancelled()) {
      settle(AnalysisSnapshot::Phase::kCancelled, {});
      return;
    }

    // ---- discovery ---------------------------------------------------------
    publish([&](AnalysisSnapshot& s) {
      s.phase = AnalysisSnapshot::Phase::kDiscovering;
      s.stage = "Discovering DWARF";
    });
    const auto t_discovery = diag::Clock::now();
    dwarf::DwarfContext ctx(elf);
    const dwarf::Sections& probe = ctx.sections();
    // The same test cmd_emit() makes: both sections, and at least one unit that
    // actually parses. A .debug_info of length zero is not usable DWARF.
    const std::uint64_t unit_total = ctx.unit_count();
    const bool have_dwarf = probe.has_info() && unit_total != 0;
    publish([&](AnalysisSnapshot& s) {
      s.file.has_dwarf = have_dwarf;
      s.file.dwarf_summary = probe.capability_report();
      s.file.unit_total = unit_total;
      s.file.debug_sections = probe.present_debug_sections();
      s.units_total = unit_total;
    });
    record("unit_discovery", diag::seconds_since(t_discovery), unit_total, ctx.info().size());

    // Mode selection mirrors cmd_emit() exactly, so a dump started here is the
    // dump the CLI would have produced from the same flags.
    bool use_dwarfless = false;
    if (o.mode == "dwarfless") {
      use_dwarfless = true;
    } else if (o.mode == "dwarf") {
      if (!have_dwarf) {
        settle(AnalysisSnapshot::Phase::kFailed,
               "dwarf mode requested but " + o.input_path + " has no DWARF");
        return;
      }
    } else {
      use_dwarfless = !have_dwarf;
    }
    publish([&](AnalysisSnapshot& s) {
      s.dwarfless = use_dwarfless;
      s.mode_label = use_dwarfless ? "dwarfless" : "dwarf";
    });

    if (have_dwarf) {
      // A header-only pass over the units: unit_count() has already memoised
      // every unit offset, so this costs one header read per unit. It exists to
      // give the UI a real "compilation unit N of M" and -- more importantly --
      // to observe the cancel flag between units, which build_model() cannot do
      // for us because it owns its own loop.
      dwarf::DwarfContext::UnitIterator it(ctx);
      dwarf::UnitHeader header;
      std::string unit_err;
      while (!cancelled() && it.next(header, &unit_err)) {
        sample("compilation unit " + std::to_string(it.index()) + " of " +
               std::to_string(unit_total));
      }
      if (cancelled()) {
        settle(AnalysisSnapshot::Phase::kCancelled, {});
        return;
      }
    }

    // ---- model build -------------------------------------------------------
    publish([&](AnalysisSnapshot& s) {
      s.phase = AnalysisSnapshot::Phase::kBuilding;
      s.stage = use_dwarfless ? "Recovering model (dwarfless)" : "Building model";
    });
    sample("starting");
    ir::Model model;
    ir::BuildStats bst;
    ir::DwarflessStats dst;
    const bool built = use_dwarfless ? ir::build_dwarfless_model(ctx, model, &dst)
                                     : ir::build_model(ctx, ir::BuildOptions{}, model, &bst);
    if (!built) {
      settle(AnalysisSnapshot::Phase::kFailed, "could not build the model");
      return;
    }
    if (use_dwarfless) {
      record("build_dwarfless", dst.seconds, dst.fdes, 0);
      count("functions_named", dst.functions_named);
      count("functions_unnamed", dst.functions_sub_);
      count("classes_from_rtti", dst.classes_from_rtti);
      count("vtable_slots", dst.vtable_slots);
    } else {
      record("build_model", bst.seconds, bst.dies, 0);
    }
    publish([&](AnalysisSnapshot& s) {
      s.units = use_dwarfless ? s.units : bst.units;
      // There is no DIE count anywhere in a DWARF section, so the dwarfless
      // path reports the FDEs it walked through the same slot rather than
      // leaving it blank.
      s.dies = use_dwarfless ? dst.fdes : bst.dies;
      s.types = bst.type_nodes;
      s.fields = bst.fields;
      s.methods = bst.methods;
    });
    count("type_nodes", model.types.size());
    sample("model built");
    if (cancelled()) {
      settle(AnalysisSnapshot::Phase::kCancelled, {});
      return;
    }


    // ---- emit --------------------------------------------------------------
    publish([&](AnalysisSnapshot& s) {
      s.phase = AnalysisSnapshot::Phase::kEmitting;
      s.stage = "Writing dump";
    });
    std::string out_path = o.out_path;
    if (out_path.empty()) {
      out_path = use_dwarfless ? "output/dump.dwarfless.cs" : "output/dump.cs";
    }
    const std::size_t slash = out_path.find_last_of("/\\");
    if (slash != std::string::npos) {
      const std::string dir = out_path.substr(0, slash);
      if (ensure_directory(dir) != 0) {
        settle(AnalysisSnapshot::Phase::kFailed, "cannot create directory " + dir);
        return;
      }
    }
    publish([&](AnalysisSnapshot& s) { s.out_path = out_path; });

    partial = out_path + ".part";
    std::FILE* out = std::fopen(partial.c_str(), "wb");
    if (out == nullptr) {
      settle(AnalysisSnapshot::Phase::kFailed, "cannot open " + partial + " for writing");
      return;
    }
    output::EmitOptions eopts;
    eopts.emit_bases = true;  // what cmd_emit() always sets
    eopts.pad_layout = o.pad_layout;
    eopts.emit_methods = o.emit_methods;
    eopts.max_lines = o.max_lines;
    eopts.target_name = o.target_name.empty() ? basename_of(o.input_path) : o.target_name;
    eopts.inferred = use_dwarfless;
    if (use_dwarfless) {
      eopts.named_functions = dst.functions_named;
      eopts.unnamed_functions = dst.functions_sub_;
      eopts.vtable_slots = dst.vtable_slots;
      eopts.classes_from_rtti = dst.classes_from_rtti;
    }
    output::EmitStats est;
    const auto t_emit = diag::Clock::now();
    output::emit_il2cpp(model, out, eopts, &est);
    std::fclose(out);
    record("emit", diag::seconds_since(t_emit), est.lines, est.bytes);

    // A cancel that lands during the emitter still discards the output: half a
    // dump is worse than none, because nothing in the file name says so.
    if (cancelled()) {
      settle(AnalysisSnapshot::Phase::kCancelled, {});
      return;
    }
    if (std::rename(partial.c_str(), out_path.c_str()) != 0) {
      settle(AnalysisSnapshot::Phase::kFailed, "cannot write " + out_path);
      return;
    }
    partial.clear();

    count("classes", model.classes.size());
    count("enums", model.enums.size());
    count("fields", model.fields.size());
    count("output_bytes", est.bytes);
    count("output_lines", est.lines);
    publish([&](AnalysisSnapshot& s) {
      s.out_bytes = est.bytes;
      s.out_lines = est.lines;
      s.enums = est.enums;
      s.classes = est.classes;
      s.structs = est.structs;
      s.unions = est.unions;
      s.fields_total = est.fields;
      s.methods_total = est.methods;
      s.functions = est.functions;
      s.globals = est.globals;
    });
    settle(AnalysisSnapshot::Phase::kDone, {});
  };

  try {
    body();
  } catch (const std::exception& e) {
    // Nothing may escape a thread: an uncaught exception would take the whole
    // TUI down, and this is the one place where "something went wrong" can
    // always be turned into a message on screen instead.
    settle(AnalysisSnapshot::Phase::kFailed, std::string("internal error: ") + e.what());
  } catch (...) {
    settle(AnalysisSnapshot::Phase::kFailed, "internal error: unknown exception");
  }

  done_.store(true, std::memory_order_release);
  done_cv_.notify_all();
}

}  // namespace stellar::tui

