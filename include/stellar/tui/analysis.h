// SPDX-License-Identifier: MIT
// The background analysis job behind the TUI's "run" action.
//
// Dumping the real target takes 30+ seconds. Everything the core does -- opening
// the container, walking 20M DIEs, building a multi-million-node type graph and
// writing a 300 MB dump -- is a single blocking call, so it cannot run on the
// thread that owns the terminal. This module wraps those *existing* core entry
// points in one worker thread and publishes what it can see into a copyable
// snapshot the UI polls on its own frame loop.
//
// Two rules shape the design:
//
//   * The snapshot is a plain value with no locks in it. The worker fills it
//     under one mutex; the UI copies it out under the same mutex and then reads
//     the copy with no synchronisation at all. A widget therefore cannot ever
//     block on, or be blocked by, the analysis.
//   * No analysis logic lives here. The mode decision, the model build and the
//     emitter are the core's, byte for byte, so a dump started from the TUI is
//     the same dump `stellar emit` would write.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "stellar/diag/metrics.h"

namespace stellar::tui {

/// What the input file actually is, as reported by the ELF/DWARF readers.
///
/// Every field is measured, never guessed: the Info screen renders this as-is,
/// and a field the readers could not determine is left empty rather than filled
/// with something plausible.
struct FileFacts {
  bool valid = false;                 ///< false until elf::ElfFile::open() succeeds
  std::string path;                   ///< the path as opened
  std::string format;                 ///< ElfFile::describe(), e.g. "ELF64 DYN aarch64"
  std::string machine;                ///< elf::machine_name(e_machine)
  std::string size_text;              ///< util::human_size(file_size())
  std::string endianness;             ///< "little" or "big"
  std::uint64_t file_size = 0;        ///< bytes
  bool has_dwarf = false;             ///< .debug_info + .debug_abbrev, and >=1 unit
  std::string dwarf_summary;          ///< Sections::capability_report()
  std::uint64_t unit_total = 0;       ///< compilation units in .debug_info
  /// Every .debug_* section present, with its size, in section-table order.
  std::vector<std::pair<std::string, std::uint64_t>> debug_sections;
};

/// The worker's whole observable state, as one copyable value.
struct AnalysisSnapshot {
  enum class Phase : std::uint8_t {
    kIdle,        ///< no run has been started yet
    kOpening,     ///< mapping and validating the ELF container
    kDiscovering, ///< probing DWARF sections and counting compilation units
    kBuilding,    ///< ir::build_model / ir::build_dwarfless_model
    kEmitting,    ///< output::emit_il2cpp writing the dump
    kDone,        ///< finished; the result counts below are final
    kFailed,      ///< `error` explains why
    kCancelled,   ///< request_cancel() was honoured
  };

  Phase phase = Phase::kIdle;
  /// Human-readable description of the current phase, e.g. "Building model".
  std::string stage;
  /// Finer detail, e.g. "compilation unit 972". While the core is running this
  /// carries the reporter's own stage name, which is more precise than ours.
  std::string current_note;
  /// Non-empty only for Phase::kFailed.
  std::string error;

  /// Live progress, sampled from the core's progress reporter.
  uint64_t units = 0;
  uint64_t units_total = 0;   ///< from DwarfContext::unit_count()
  uint64_t dies = 0;
  uint64_t dies_total = 0;    ///< 0 = unknown: DWARF carries no DIE count
  uint64_t types = 0;
  uint64_t fields = 0;
  uint64_t methods = 0;

  /// Output written so far; final once Phase::kEmitting completes.
  uint64_t out_bytes = 0;
  uint64_t out_lines = 0;

  double elapsed_seconds = 0.0;
  uint64_t rss_bytes = 0;     ///< current resident set size of this process

  /// True when the run fell back to the no-DWARF path.
  bool dwarfless = false;
  std::string out_path;       ///< where the dump is being written
  /// The mode the run settled on: "dwarf" or "dwarfless".
  std::string mode_label;

  /// Final result, read from output::EmitStats. Meaningful at Phase::kDone.
  uint64_t enums = 0;
  uint64_t classes = 0;
  uint64_t structs = 0;
  uint64_t unions = 0;
  uint64_t fields_total = 0;
  uint64_t methods_total = 0;
  uint64_t functions = 0;

  uint64_t globals = 0;

  /// What the ELF/DWARF readers know about the input.
  FileFacts file;
};

/// Owns at most one running analysis.
///
/// The thread is always joinable (never detached) so the destructor can wait for
/// it; `request_cancel()` is the way to stop early, and it is honoured between
/// phases and between compilation units. The two core calls that own their own
/// loops -- build_model() and emit_il2cpp() -- cannot be interrupted from
/// outside, so a cancel that arrives during one of them takes effect as soon as
/// it returns.
class Analysis {
 public:
  struct StartOptions {
    std::string input_path;
    std::string out_path;                  ///< empty = the core's default location
    std::string target_name;               ///< empty = the input file's name
    bool pad_layout = true;
    bool emit_methods = true;
    uint64_t max_lines = 0;                ///< 0 = unlimited
    /// "auto" | "dwarf" | "dwarfless"; "auto" picks dwarfless when the input
    /// has no .debug_info, exactly as `stellar emit --mode=auto` does.
    std::string mode = "auto";
  };

  Analysis() = default;
  ~Analysis() noexcept;
  Analysis(const Analysis&) = delete;
  Analysis& operator=(const Analysis&) = delete;

  /// Reaps a finished run and starts a new one. Returns false when a run is
  /// still in progress, when `input_path` is empty, or when the thread could not
  /// be created (the snapshot then carries the reason).
  bool start(const StartOptions& opts);

  /// Asks the worker to stop. Safe from any thread at any time, including when
  /// no run is in progress.
  void request_cancel() noexcept;

  /// True between a successful start() and the worker reaching a final phase.
  [[nodiscard]] bool running() const noexcept;

  /// True once the worker has reached kDone, kFailed or kCancelled.
  [[nodiscard]] bool done() const noexcept;

  /// A consistent copy of the current state. Cheap enough for a frame loop.
  ///
  /// Two things are refreshed here, on the reading side, because the worker is
  /// usually blocked inside a core call that cannot be polled:
  ///
  ///   * `elapsed_seconds` is stamped from the clock. The start time is fixed
  ///     before the thread exists and nothing else writes it, so a progress bar
  ///     that stopped counting -- the one thing that makes a long dump look hung
  ///     -- cannot happen.
  ///   * the counters are re-read from diag::Progress. Its counters are relaxed
  ///     atomics, so reading them from here while the worker is inside
  ///     build_model() is safe and gives genuinely live numbers rather than the
  ///     ones stamped at the last phase boundary.
  [[nodiscard]] AnalysisSnapshot snapshot() const;

  /// Measured phase timings and counters, for a results screen. Only final once
  /// done() is true; before that it is a snapshot of the phases so far.
  [[nodiscard]] diag::Metrics metrics() const;

  /// Waits for the worker. Idempotent, and safe to call from a destructor.
  void join() noexcept;

  /// Blocks until the worker finishes or `timeout_ms` elapses; a negative
  /// timeout waits forever. Returns true when the run has finished.
  bool wait_for_finish(int timeout_ms);

  /// Stable lowercase name of a phase, for logs and status bars.
  [[nodiscard]] static const char* phase_name(AnalysisSnapshot::Phase p) noexcept;

 private:
  void run(StartOptions opts);

  mutable std::mutex mutex_;        ///< guards snap_ and metrics_
  AnalysisSnapshot snap_;
  diag::Metrics metrics_;
  /// When the current run started; written by start() before the thread exists
  /// and read by snapshot() to keep the UI's clock ticking.
  diag::Clock::time_point t0_{};
  std::thread worker_;              ///< joinable by construction
  std::atomic<bool> cancel_{false};
  std::atomic<bool> done_{false};
  std::mutex done_mutex_;
  std::condition_variable done_cv_;
};

}  // namespace stellar::tui
