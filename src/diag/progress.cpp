// SPDX-License-Identifier: MIT
#include "c2d/diag/progress.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#define C2D_ISATTY _isatty
#define C2D_FILENO _fileno
#else
#include <unistd.h>
#define C2D_ISATTY isatty
#define C2D_FILENO fileno
#endif

#include "c2d/diag/metrics.h"

namespace c2d::diag {
namespace {

/// How many `add` calls may pass before the clock is consulted. Reading the
/// clock on every one of twenty million updates would be wasteful; doing it once
/// per few thousand is free and still smooth.
constexpr std::uint64_t kCheckEvery = 2048;

void append_thousands(std::string& out, std::uint64_t v) {
  char digits[24];
  int n = std::snprintf(digits, sizeof(digits), "%llu",
                        static_cast<unsigned long long>(v));
  for (int i = 0; i < n; ++i) {
    if (i != 0 && (n - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
}

}  // namespace

Progress& Progress::instance() {
  static Progress p;
  return p;
}

void Progress::configure(bool force, int min_interval_ms) {
  interval_ms_ = min_interval_ms > 0 ? min_interval_ms : 80;
  // Only draw over a terminal unless asked: a redirected log would otherwise
  // be one enormous line of carriage returns.
  enabled_ = force || C2D_ISATTY(C2D_FILENO(stderr)) != 0;
  draw_enabled_ = true;
  last_draw_ms_ = 0;
  since_check_ = kCheckEvery;
  drawn_ = false;
  dirty_ = false;
}

void Progress::reset() {
  count_ = 0;
  primary_ = nullptr;
  stage_.clear();
  note_.clear();
  std::memset(counters_, 0, sizeof(counters_));
  since_check_ = 0;
  dirty_ = false;
}

int Progress::find(const char* label) const {
  for (std::size_t i = 0; i < count_; ++i) {
    if (counters_[i].label == label) return static_cast<int>(i);
  }
  return -1;
}

void Progress::declare(const char* label, std::uint64_t total, bool has_total) {
  const int i = find(label);
  if (i >= 0) {
    counters_[i].total = total;
    counters_[i].has_total = has_total;
    dirty_ = true;
    return;
  }
  if (count_ >= kMaxCounters) return;
  Counter& c = counters_[count_++];
  c.label = label;
  c.value = 0;
  c.total = total;
  c.has_total = has_total;
  dirty_ = true;
}

void Progress::primary(const char* label) {
  primary_ = label;
  dirty_ = true;
}

void Progress::stage(std::string_view name) {
  stage_.assign(name);
  dirty_ = true;
  checkpoint();  // stage changes are worth showing immediately
}

void Progress::note(std::string_view text) {
  note_.assign(text);
  dirty_ = true;
}

void Progress::set(const char* label, std::uint64_t value) {
  const int i = find(label);
  if (i < 0) {
    declare(label, 0, false);
  }
  const int j = find(label);
  if (j >= 0) counters_[j].value = value;
  dirty_ = true;
}

void Progress::add(const char* label, std::uint64_t n) {
  if (!enabled_) return;  // the hot path: one predictable branch
  int i = find(label);
  if (i < 0) {
    if (count_ >= kMaxCounters) return;
    i = static_cast<int>(count_++);
    counters_[i].label = label;
  }
  counters_[i].value += n;
  dirty_ = true;
  if (++since_check_ < kCheckEvery) return;
  since_check_ = 0;
  const auto now = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
          .count());
  if (now - last_draw_ms_ >= interval_ms_) draw();
}

bool Progress::primary_value(std::uint64_t& value, std::uint64_t& total) const {
  if (primary_ != nullptr) {
    const int i = find(primary_);
    if (i >= 0 && counters_[i].has_total && counters_[i].total != 0) {
      value = counters_[i].value;
      total = counters_[i].total;
      return true;
    }
  }
  // Otherwise the first counter that knows its total drives the percentage.
  for (std::size_t i = 0; i < count_; ++i) {
    if (counters_[i].has_total && counters_[i].total != 0) {
      value = counters_[i].value;
      total = counters_[i].total;
      return true;
    }
  }
  return false;
}

std::string Progress::render() const {
  buffer_.clear();
  std::uint64_t pv = 0, pt = 0;
  if (primary_value(pv, pt)) {
    const unsigned pct = pt != 0 ? static_cast<unsigned>((pv * 100) / pt) : 0;
    char head[32];
    std::snprintf(head, sizeof(head), "[%3u%%] ", pct);
    buffer_ += head;
  }
  bool first = true;
  for (std::size_t i = 0; i < count_; ++i) {
    const Counter& c = counters_[i];
    if (!first) buffer_ += " | ";
    first = false;
    buffer_ += c.label;
    buffer_ += ": ";
    append_thousands(buffer_, c.value);
    if (c.has_total && c.total != 0) {
      buffer_ += '/';
      append_thousands(buffer_, c.total);
    }
  }
  if (!stage_.empty()) {
    buffer_ += first ? "Stage: " : " | Stage: ";
    buffer_ += stage_;
  }
  if (!note_.empty()) {
    buffer_ += " | ";
    buffer_ += note_;
  }
  return buffer_;
}

void Progress::draw() {
  if (!enabled_ || !draw_enabled_) return;
  const std::string line = render();
  // Pad to erase whatever the previous, longer, line left behind.
  const std::size_t pad = drawn_ && buffer_.size() < last_len_ ? last_len_ - buffer_.size() : 0;
  last_len_ = buffer_.size();
  std::fprintf(stderr, "\r%s%*s", line.c_str(), static_cast<int>(pad), "");
  std::fflush(stderr);
  last_draw_ms_ = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
          .count());
  drawn_ = true;
  dirty_ = false;
}

void Progress::checkpoint() {
  if (!enabled_) return;
  since_check_ = 0;
  draw();
}

void Progress::clear_line() {
  if (!drawn_) return;
  std::fprintf(stderr, "\r%*s\r", static_cast<int>(last_len_), "");
  std::fflush(stderr);
  drawn_ = false;
  last_len_ = 0;
}

void Progress::finish_line(std::string_view line) {
  clear_line();
  if (line.empty()) return;
  std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
  std::fflush(stderr);
}

void Progress::fail(std::string_view message) {
  finish_line(message);
}

}  // namespace c2d::diag
