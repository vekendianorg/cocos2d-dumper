// SPDX-License-Identifier: MIT
// Raw-mode terminal ownership: alternate screen, cursor, key decoding, size
// tracking and guaranteed restoration.
//
// The whole point of this file is the guarantee at the bottom of it. A TUI that
// exits with the cursor hidden, the alternate screen active, or the terminal in
// raw mode leaves the user's shell unusable until they run `reset`. So terminal
// state is owned by a single RAII type, restoration is idempotent, and it also
// runs from the signal handlers for SIGINT/SIGTERM/SIGHUP/SIGSEGV/SIGABRT.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace stellar::tui {

/// A decoded key press. `kChar` carries UTF-8 text in `text`; the named keys
/// carry nothing.
enum class Key : std::uint8_t {
  kNone,
  kChar,
  kUp,
  kDown,
  kLeft,
  kRight,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
  kDelete,
  kInsert,
  kEnter,
  kTab,
  kBackTab,     ///< Shift-Tab
  kBackspace,
  kEscape,
  kF1, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
};

struct Event {
  Key key = Key::kNone;
  /// The characters typed, when `key == Key::kChar`. May be more than one byte
  /// (UTF-8), and may hold a pasted run of text.
  std::string text;
  /// True when `key` is one of the character keys.
  [[nodiscard]] bool is_char() const { return key == Key::kChar; }
};

/// Cursor and screen control sequences, in one place so no widget invents its
/// own. Every one of these is a no-op when ANSI is disabled.
namespace ansi {
inline constexpr std::string_view kAltScreenOn = "\033[?1049h";
inline constexpr std::string_view kAltScreenOff = "\033[?1049l";
inline constexpr std::string_view kHideCursor = "\033[?25l";
inline constexpr std::string_view kShowCursor = "\033[?25h";
inline constexpr std::string_view kClearScreen = "\033[2J";
inline constexpr std::string_view kHome = "\033[H";
inline constexpr std::string_view kEraseLine = "\033[2K";
inline constexpr std::string_view kEraseToEnd = "\033[J";
inline constexpr std::string_view kReset = "\033[0m";
/// Move the cursor to a 1-based row/column.
std::string cursor_to(int row, int col);
/// Write `n` spaces, for blanking without an erase sequence.
std::string spaces(std::size_t n);
}  // namespace ansi

/// Owns the terminal for the lifetime of a TUI session.
class Terminal {
 public:
  Terminal();
  ~Terminal();

  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  /// Enters the alternate screen, hides the cursor and switches stdin to raw
  /// mode. Returns false when stdin is not a terminal, in which case the TUI
  /// must refuse to start rather than run half-blind.
  bool enter(bool allow_ansi);

  /// Restores everything `enter()` changed. Idempotent, and safe to call when
  /// `enter()` was never called or failed partway.
  void leave() noexcept;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool ansi() const noexcept { return ansi_; }

  /// Terminal size in columns/rows, re-read from the OS. Falls back to 80x24
  /// when the terminal cannot report one (a bare pty, some CI runners).
  void refresh_size() noexcept;
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  /// True once, after the size has changed since the last call.
  [[nodiscard]] bool size_changed();

  /// Waits up to `timeout_ms` for input. Returns false on timeout, which the
  /// event loop uses as its tick to keep redrawing progress.
  bool poll(Event& out, int timeout_ms);

  /// Queues output. The terminal keeps its own buffer so a frame is written in
  /// as few syscalls as possible.
  void write(std::string_view s);
  /// Queues output that must not be styled (used by the no-colour path).
  void write_raw(std::string_view s) { write(s); }
  void flush();

  /// Number of frames drawn, exposed for the tests.
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

  /// Installs handlers for SIGINT/SIGTERM/SIGHUP/SIGSEGV/SIGABRT/SIGTSTP that
  /// restore the terminal before the process dies. The handler does nothing but
  /// write(2) a fixed restore string, which is async-signal-safe.
  static void install_signal_handlers();
  /// The exact sequence written by the signal handler.
  static std::string_view restore_sequence();

  /// Pushes a pending terminal-restore onto the process exit path. Belt and
  /// braces: the destructor and the signal handler normally both fire.
  static void install_atexit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool ansi_ = false;
  int width_ = 80;
  int height_ = 24;
  bool dirty_size_ = false;
  std::uint64_t frames_ = 0;
  /// The last size handed to the renderer, so size_changed() can be edge
  /// triggered rather than polled.
  int last_width_ = -1;
  int last_height_ = -1;
};

}  // namespace stellar::tui
