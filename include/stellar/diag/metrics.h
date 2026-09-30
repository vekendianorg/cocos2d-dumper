// SPDX-License-Identifier: MIT
// Measurement primitives: wall-clock timers, phase accounting, counters and
// process resource usage (peak RSS).
//
// The project treats performance as a first-class requirement, so every phase
// reports measured numbers rather than estimates. Counters are plain relaxed
// atomics: they are updated on hot paths and must not serialise threads.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stellar::diag {

using Clock = std::chrono::steady_clock;

inline double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

inline double ms_since(Clock::time_point t0) { return seconds_since(t0) * 1000.0; }

/// Peak resident set size of this process, in bytes (0 if unavailable).
/// Read from /proc/self/status VmHWM, which is the kernel's own high-water
/// mark and therefore unaffected by later freeing.
[[nodiscard]] std::uint64_t peak_rss_bytes();

/// Current resident set size in bytes (0 if unavailable).
[[nodiscard]] std::uint64_t current_rss_bytes();

/// One named phase with its measured duration and optional counters.
struct PhaseStat {
  std::string name;
  double seconds = 0.0;
  std::uint64_t count = 0;      ///< items processed
  std::uint64_t bytes = 0;      ///< bytes consumed, if meaningful
};

/// Aggregated run statistics. Passed around by value/reference; cheap to copy
/// at the handful of phase boundaries where it is read.
class Metrics {
 public:
  void set_label(std::string label) { label_ = std::move(label); }
  [[nodiscard]] const std::string& label() const { return label_; }

  /// Record a completed phase. Times are measured by the caller so that a phase
  /// can include arbitrary setup/teardown.
  void add_phase(std::string name, double seconds, std::uint64_t count = 0,
                 std::uint64_t bytes = 0);

  /// Accumulate a value into a named counter (used for unit/DIE/type totals).
  void add_count(const std::string& key, std::uint64_t delta = 1) {
    counters_[key] += delta;
  }
  void set_count(const std::string& key, std::uint64_t value) { counters_[key] = value; }
  [[nodiscard]] std::uint64_t count(const std::string& key) const {
    auto it = counters_.find(key);
    return it == counters_.end() ? 0 : it->second;
  }

  [[nodiscard]] const std::map<std::string, PhaseStat>& phases() const { return phases_; }
  [[nodiscard]] const std::map<std::string, std::uint64_t>& counters() const { return counters_; }
  void add_peak_rss(std::uint64_t bytes);
  void add_bytes_read(std::uint64_t bytes) { bytes_read_ += bytes; }
  [[nodiscard]] std::uint64_t bytes_read() const { return bytes_read_; }
  [[nodiscard]] std::uint64_t peak_rss() const { return peak_rss_ ? peak_rss_ : peak_rss_bytes(); }

  /// Human-readable multi-line report.
  [[nodiscard]] std::string report() const;

 private:
  std::string label_;
  std::map<std::string, PhaseStat> phases_;
  std::map<std::string, std::uint64_t> counters_;
  std::uint64_t bytes_read_ = 0;
  std::uint64_t peak_rss_ = 0;
};

/// RAII timer that reports its elapsed time into a Metrics on destruction.
class ScopedTimer {
 public:
  ScopedTimer(Metrics& m, std::string name)
      : metrics_(&m), name_(std::move(name)), t0_(Clock::now()) {}
  ~ScopedTimer() {
    if (metrics_) metrics_->add_phase(std::move(name_), seconds_since(t0_));
  }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  [[nodiscard]] double elapsed() const { return seconds_since(t0_); }

 private:
  Metrics* metrics_;
  std::string name_;
  Clock::time_point t0_;
};

/// Process-wide resource snapshot, for the final summary.
struct ResourceReport {
  std::uint64_t peak_rss_bytes = 0;
  std::uint64_t current_rss_bytes = 0;
  double wall_seconds = 0.0;
  double user_seconds = 0.0;
  double system_seconds = 0.0;
};

[[nodiscard]] ResourceReport resource_report(double wall_seconds);

}  // namespace stellar::diag
