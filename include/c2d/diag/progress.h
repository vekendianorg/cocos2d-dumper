// SPDX-License-Identifier: MIT
// In-place progress reporting.
//
// Long stages (walking 20M DIEs, building a 3M-node type graph, writing a
// 300 MB dump) look like a hang without feedback. This writes a single
// terminal line that is rewritten in place with a carriage return, e.g.
//
//   [ 24%] DIEs: 12,450/50,000 (24%) | Types: 1,203/4,800 | Stage: DWARF
//
// Design constraints:
//   * cheap when disabled -- the hot path is one predictable branch, so adding
//     progress to a 20M-iteration loop costs nothing measurable
//   * cheap when enabled -- the clock is read at most every few thousand
//     updates rather than per update, and rendering reuses one buffer
//   * generic -- counters are named by the caller, nothing here knows what a
//     DIE or a field is
//   * tidy -- the line is erased before any error or summary is printed
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace c2d::diag {

class Progress {
 public:
  /// A counter shown on the status line. `label` must outlive the reporter, so
  /// callers pass string literals rather than built strings.
  struct Counter {
    const char* label = nullptr;
    std::uint64_t value = 0;
    std::uint64_t total = 0;
    bool has_total = false;
  };

  static constexpr std::size_t kMaxCounters = 6;

  /// The process-wide reporter. A single instance keeps the hot paths free of
  /// plumbing while still being one self-contained, testable type.
  static Progress& instance();

  /// `auto` (the default) draws only when stderr is a terminal, so redirected
  /// output does not fill up with carriage returns.
  void configure(bool force, int min_interval_ms);
  void set_enabled(bool on) { enabled_ = on; }
  [[nodiscard]] bool enabled() const { return enabled_; }

  /// Selects which counter the percentage is taken from (the first one with a
  /// known total, unless named).
  void primary(const char* label);

  /// Sets the text shown after `Stage:`.
  void stage(std::string_view name);

  /// Declares a counter. Safe to call repeatedly with the same label.
  void declare(const char* label, std::uint64_t total, bool has_total = true);

  /// Adds to a counter and redraws if the interval has elapsed.
  void add(const char* label, std::uint64_t n = 1);
  /// Sets a counter outright and redraws if the interval has elapsed.
  void set(const char* label, std::uint64_t value);

  /// Free-form extra text appended after the counters, e.g. the current unit.
  void note(std::string_view text);

  /// Redraws immediately (used at stage boundaries, where a slower update is
  /// worth it so the user sees the change).
  void checkpoint();

  /// Erases the status line without printing anything else.
  void clear_line();

  /// Clears the line and prints `line` followed by a newline: the completion
  /// summary, or an error that interrupted the run.
  void finish_line(std::string_view line);
  void fail(std::string_view message);

  /// Composes the current status line (exposed for tests).
  [[nodiscard]] std::string render() const;

  /// Resets counters, stage and totals; used between runs and by tests.
  void reset();

 private:
  Progress() = default;
  [[nodiscard]] int find(const char* label) const;
  [[nodiscard]] bool primary_value(std::uint64_t& value, std::uint64_t& total) const;
  void draw();

  Counter counters_[kMaxCounters]{};
  std::size_t count_ = 0;
  const char* primary_ = nullptr;
  std::string stage_;
  std::string note_;
  /// Scratch space for render(), which is const so callers can ask for the
  /// current line without disturbing the reporter.
  mutable std::string buffer_;
  std::size_t last_len_ = 0;  ///< width of the last drawn line, for erasing
  std::uint64_t since_check_ = 0;
  std::int64_t last_draw_ms_ = 0;
  int interval_ms_ = 80;
  bool enabled_ = false;
  bool drawn_ = false;
  bool dirty_ = false;
};

/// Shorthand for the process-wide reporter, for the short call sites that sit
/// in hot loops and should not have to spell out `Progress::instance()`.
inline Progress& progress() { return Progress::instance(); }

}  // namespace c2d::diag
