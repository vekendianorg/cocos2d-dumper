// SPDX-License-Identifier: MIT
#include "c2d/diag/progress.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define C2D_ISATTY _isatty
#define C2D_FILENO _fileno
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define C2D_ISATTY isatty
#define C2D_FILENO fileno
#endif

#include <cstdlib>

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

/// Columns available on the attached terminal, or 0 when it cannot tell.
///
/// Used to clamp the status line so it never wraps.
int terminal_width() {
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_ERROR_HANDLE), &csbi)) {
    const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return w > 0 ? w : 0;
  }
  return 0;
#else
  struct winsize ws {};
  if (::ioctl(C2D_FILENO(stderr), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return ws.ws_col;
  }
  return 0;
#endif
}

/// Whether the terminal can erase a line for us. A dumb terminal, or a
/// redirected stream, gets the space-padding fallback instead.
bool terminal_supports_ansi() {
  if (C2D_ISATTY(C2D_FILENO(stderr)) == 0) return false;
  const char* term = std::getenv("TERM");
  if (term != nullptr) {
    const std::string_view t(term);
    if (t == "dumb") return false;
  }
#if defined(_WIN32)
  return false;  // the Windows console handles the rewrite itself
#else
  return true;
#endif
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
  // Only worth detecting when something will actually be drawn.
  ansi_ = enabled_ && terminal_supports_ansi();
  width_ = enabled_ ? terminal_width() : 0;
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
  const bool changed = stage_ != name;
  stage_.assign(name);
  dirty_ = true;
  if (!changed) return;
  // A stage change is a milestone worth keeping: finish the in-progress row and
  // start the new stage on a line of its own, rather than overwriting it.
  if (drawn_) {
    std::fputc('\n', stderr);
    std::fflush(stderr);
    drawn_ = false;
    last_len_ = 0;
  }
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
  std::string line = render();

  // Clamp to the terminal so the line can never wrap. A wrapped status line is
  // the root cause of the old duplicated text: once the cursor has wrapped onto
  // a second row, a later carriage return only returns to the start of that row
  // and the first fragment stays on screen.
  //
  // When the width cannot be determined (a pty that reports none, for
  // instance) fall back to the conventional 80 rather than writing a line of
  // unbounded length and hoping.
  const int cols = width_ > 1 ? width_ : 80;
  if (line.size() > static_cast<std::size_t>(cols - 1)) {
    line.resize(static_cast<std::size_t>(cols - 1));
  }

  // Wipe the row first, then write exactly one line. No newline: the next update
  // replaces this one.
  clear_line();
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
  last_len_ = line.size();
  drawn_ = true;
  dirty_ = false;
  last_draw_ms_ = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
          .count());
}

void Progress::checkpoint() {
  if (!enabled_) return;
  since_check_ = 0;
  draw();
}

void Progress::clear_line() {
  if (!drawn_) return;
  if (ansi_) {
    // Erase the whole row, whatever is on it, then return to column 0.
    std::fputs("\x1b[2K\r", stderr);
  } else {
    // Fallback for terminals without ANSI, and for output that is redirected:
    // overwrite the known number of columns with spaces.
    std::fprintf(stderr, "\r%*s\r", static_cast<int>(last_len_), "");
  }
  std::fflush(stderr);
  drawn_ = false;
  last_len_ = 0;
}

void Progress::finish() {
  if (!drawn_) return;
  // Close the row before returning, so following output is not written over it.
  std::fputc('\n', stderr);
  std::fflush(stderr);
  clear_line();
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
