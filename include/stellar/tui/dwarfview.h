// SPDX-License-Identifier: MIT
// Backends for the "Browse compilation units" and "Scan DWARF" screens.
//
// Like analysis.h, this holds no DWARF knowledge of its own: it drives the same
// reader entry points `stellar units` and `stellar scan` use (UnitIterator,
// UnitWalker, WalkStats), so what the TUI shows is what the CLI would print.
//
//   * list_units() is a header-only walk -- it never touches a DIE -- and is
//     cheap enough to run on the calling thread.
//   * ScanJob walks every DIE, which takes tens of seconds on a real binary, so
//     it runs on one joinable worker and publishes a copyable snapshot.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stellar::tui {

/// One compilation unit's header, as read from .debug_info.
struct UnitRow {
  std::uint64_t index = 0;
  std::uint64_t offset = 0;
  std::uint64_t abbrev_offset = 0;
  std::uint32_t version = 0;
  std::uint32_t address_size = 0;
};

struct UnitList {
  bool ok = false;
  std::string error;               ///< set when !ok
  std::uint64_t total = 0;         ///< units in .debug_info
  std::uint64_t unparsable = 0;    ///< headers the reader had to skip
  bool truncated = false;          ///< stopped at max_rows
  std::vector<UnitRow> rows;
};

/// Lists the unit headers of `path`. Never walks DIEs.
[[nodiscard]] UnitList list_units(const std::string& path,
                                  std::uint64_t max_rows = 500000);

/// Counts the DIEs of the single unit `index` (walks that unit only).
[[nodiscard]] bool count_unit_dies(const std::string& path, std::uint64_t index,
                                   std::uint64_t& dies, std::string* error = nullptr);

struct ScanSnapshot {
  enum class Phase : std::uint8_t { kIdle, kRunning, kDone, kFailed, kCancelled };
  Phase phase = Phase::kIdle;
  std::string error;
  std::uint64_t units_total = 0;
  std::uint64_t units = 0;
  std::uint64_t dies = 0;
  std::uint64_t max_depth = 0;
  std::uint64_t bytes = 0;
  std::uint64_t failed_units = 0;
  std::uint64_t skipped_units = 0;
  double elapsed_seconds = 0.0;
  /// "DW_TAG_..." -> count, most frequent first. Filled when the scan ends.
  std::vector<std::pair<std::string, std::uint64_t>> tags;
};

/// Owns at most one running scan; the thread is always joinable.
class ScanJob {
 public:
  ScanJob() = default;
  ~ScanJob() noexcept;
  ScanJob(const ScanJob&) = delete;
  ScanJob& operator=(const ScanJob&) = delete;

  /// Starts a scan (reaping a finished one). False when one is still running.
  bool start(const std::string& path);
  void request_cancel() noexcept { cancel_.store(true); }
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] ScanSnapshot snapshot() const;
  void join() noexcept;

 private:
  void run(std::string path);

  mutable std::mutex mutex_;
  ScanSnapshot snap_;
  std::thread worker_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
};

}  // namespace stellar::tui
