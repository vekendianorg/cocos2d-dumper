// SPDX-License-Identifier: MIT
#include "stellar/tui/terminal.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace stellar::tui {

namespace ansi {

std::string cursor_to(int row, int col) {
  // 1-based, and clamped: a zero or negative coordinate makes some terminals
  // scroll, which would corrupt every subsequent frame.
  if (row < 1) row = 1;
  if (col < 1) col = 1;
  return "\033[" + std::to_string(row) + ';' + std::to_string(col) + 'H';
}

std::string spaces(std::size_t n) { return std::string(n, ' '); }

}  // namespace ansi

namespace {

/// Set once `leave()` has run, so the signal handler knows whether there is
/// anything to undo. Written with relaxed ordering: the handler only needs to
/// observe it eventually, and it must never block or allocate.
std::atomic<bool> g_restore_needed{false};

#if !defined(_WIN32)
/// The exact bytes the signal handler writes. A fixed string, no formatting, no
/// allocation: the only thing it is allowed to do is `write(2)`.
constexpr std::string_view kRestore =
    "\033[0m"          // drop any leaked attributes
    "\033[?25h"        // show the cursor
    "\033[?1049l"      // leave the alternate screen
    "\033[?1l"         // normal keypad mode, for terminals that switch it
    "\033[?7h";        // re-enable line wrapping, which raw mode disables

extern "C" void restore_signal_handler(int sig) {
  // write(2) is async-signal-safe. Everything else here would not be.
  if (g_restore_needed.load(std::memory_order_relaxed)) {
    const char* p = kRestore.data();
    std::size_t left = kRestore.size();
    while (left > 0) {
      const ssize_t n = ::write(STDOUT_FILENO, p, left);
      if (n <= 0) break;
      p += n;
      left -= static_cast<std::size_t>(n);
    }
  }
  // Re-raise with the default action so the exit status still reflects the
  // signal, rather than swallowing Ctrl-C into a clean exit 0.
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
#endif

}  // namespace

std::string_view Terminal::restore_sequence() {
#if defined(_WIN32)
  return "\033[0m\033[?25h\033[?1049l";
#else
  return kRestore;
#endif
}

void Terminal::install_signal_handlers() {
#if !defined(_WIN32)
  // SIGTSTP is included because Ctrl-Z is the other way a user can strand a
  // terminal: the process stops with the alternate screen active and the
  // cursor hidden, and `fg` brings back a screen nobody can see.
  for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT, SIGTSTP, SIGPIPE}) {
    if (sig == SIGPIPE) continue;  // a closed pipe is normal for `stellar | head`
    struct sigaction sa {};
    sa.sa_handler = restore_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(sig, &sa, nullptr);
  }
#endif
}

void Terminal::install_atexit() {
#if !defined(_WIN32)
  // Covers std::exit(), which skips destructors. Not strictly needed given the
  // signal handlers, but an alt screen left on by an early return() in a new
  // code path is exactly the kind of bug this prevents.
  std::atexit([] {
    if (g_restore_needed.load(std::memory_order_relaxed)) {
      const std::string_view seq = Terminal::restore_sequence();
      const char* p = seq.data();
      std::size_t left = seq.size();
      while (left > 0) {
        const ssize_t n = ::write(STDOUT_FILENO, p, left);
        if (n <= 0) break;
        p += n;
        left -= static_cast<std::size_t>(n);
      }
    }
  });
#endif
}

// --- platform state --------------------------------------------------------

struct Terminal::Impl {
  /// POSIX: the termios state to restore. Windows: the console modes.
#if defined(_WIN32)
  DWORD in_mode = 0;
  DWORD out_mode = 0;
#else
  struct termios saved {};
  bool have_saved = false;
#endif
  /// Outgoing bytes, flushed once per frame rather than per write().
  std::string out;
  /// Partially decoded UTF-8 sequence carried across reads.
  std::string pending;
};

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {}
Terminal::~Terminal() { leave(); }

bool Terminal::active() const noexcept {
#if defined(_WIN32)
  return impl_ && impl_->out_mode != 0;
#else
  return impl_ && impl_->have_saved;
#endif
}

bool Terminal::enter(bool allow_ansi) {
  install_signal_handlers();
  install_atexit();
  refresh_size();
  last_width_ = width_;
  last_height_ = height_;

#if defined(_WIN32)
  HANDLE hin = ::GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout = ::GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD im = 0, om = 0;
  if (!::GetConsoleMode(hin, &im) || !::GetConsoleMode(hout, &om)) return false;
  impl_->in_mode = im;
  impl_->out_mode = om;
  DWORD want_in = im | ENABLE_PROCESSED_INPUT;
  ::SetConsoleMode(hin, want_in);
  // ANSI sequences only work on a Windows console once VT processing is on.
  if (allow_ansi) ::SetConsoleMode(hout, om | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  ansi_ = allow_ansi;
#else
  if (::isatty(STDIN_FILENO) == 0) return false;
  if (::tcgetattr(STDIN_FILENO, &impl_->saved) != 0) return false;
  impl_->have_saved = true;
  struct termios raw = impl_->saved;
  // Disable canonical mode and signal generation: we want every keypress as it
  // happens, and Ctrl-C has to arrive as a key, not as SIGINT.
  raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  // VMIN=0/VTIME=1 makes read() a 100 ms poll, so the event loop can redraw
  // progress even when nobody is typing.
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    impl_->have_saved = false;
    return false;
  }
  ansi_ = allow_ansi;
#endif

  g_restore_needed.store(true, std::memory_order_relaxed);
  if (ansi_) {
    write(ansi::kAltScreenOn);
    write(ansi::kHideCursor);
    write(ansi::kClearScreen);
    write(ansi::kHome);
    flush();
  }
  return true;
}

void Terminal::leave() noexcept {
  if (!active()) return;
  if (ansi_) {
    // Order matters: reset attributes, show the cursor, then leave the screen.
    // Leaving the screen first would discard the reset on some terminals.
    write(ansi::kReset);
    write(ansi::kShowCursor);
    write(ansi::kAltScreenOff);
#if !defined(_WIN32)
    write("\033[?1l");
    write("\033[?7h");  // raw mode turned wrapping off; give it back
#endif
    impl_->out += '\n';  // the shell prompt should not land mid-line
    flush();
  }
#if defined(_WIN32)
  ::SetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), impl_->out_mode);
  ::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), impl_->in_mode);
  impl_->out_mode = 0;
  impl_->in_mode = 0;
#else
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &impl_->saved);
  impl_->have_saved = false;
#endif
  g_restore_needed.store(false, std::memory_order_relaxed);
}

void Terminal::refresh_size() noexcept {
  width_ = 80;
  height_ = 24;
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    const int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (w > 0) width_ = w;
    if (h > 0) height_ = h;
  }
#else
  struct winsize ws {};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_col > 0) width_ = ws.ws_col;
    if (ws.ws_row > 0) height_ = ws.ws_row;
  }
#endif
  // A terminal narrower than the layout minimum is clamped so the box-drawing
  // maths never goes negative.
  if (width_ < 20) width_ = 20;
  if (height_ < 6) height_ = 6;
}

bool Terminal::size_changed() {
  const int prev_w = width_, prev_h = height_;
  refresh_size();
  const bool changed = (width_ != prev_w) || (height_ != prev_h);
  if (changed) dirty_size_ = true;
  if (dirty_size_ && width_ == last_width_ && height_ == last_height_) dirty_size_ = false;
  last_width_ = width_;
  last_height_ = height_;
  return changed || dirty_size_;
}

void Terminal::write(std::string_view s) { impl_->out.append(s); }

void Terminal::flush() {
  if (impl_->out.empty()) return;
#if defined(_WIN32)
  ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), impl_->out.data(),
              static_cast<DWORD>(impl_->out.size()), nullptr, nullptr);
#else
  std::size_t off = 0;
  while (off < impl_->out.size()) {
    // EINTR is expected here: the signal handler can interrupt a partial write.
    const ssize_t n = ::write(STDOUT_FILENO, impl_->out.data() + off,
                              impl_->out.size() - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
#endif
  impl_->out.clear();
  ++frames_;
}

// --- input ----------------------------------------------------------------

namespace {

/// Decodes one escape sequence's final byte. `params` holds the numeric
/// parameters (already stripped of the leading '[' or 'O').
Key csi_key(char final_byte, const std::vector<int>& params, Event& ev) {
  switch (final_byte) {
    case 'A': ev.key = Key::kUp; return ev.key;
    case 'B': ev.key = Key::kDown; return ev.key;
    case 'C': ev.key = Key::kRight; return ev.key;
    case 'D': ev.key = Key::kLeft; return ev.key;
    case 'H': ev.key = Key::kHome; return ev.key;
    case 'F': ev.key = Key::kEnd; return ev.key;
    case 'Z': ev.key = Key::kBackTab; return ev.key;
    case '~': break;
    default: ev.key = Key::kNone; return ev.key;
  }
  // Final '~' forms: ESC [ <n> ~
  const int n = params.empty() ? 0 : params[0];
  switch (n) {
    case 1: ev.key = Key::kHome; break;
    case 2: ev.key = Key::kInsert; break;
    case 3: ev.key = Key::kDelete; break;
    case 4: ev.key = Key::kEnd; break;
    case 5: ev.key = Key::kPageUp; break;
    case 6: ev.key = Key::kPageDown; break;
    case 11: ev.key = Key::kF1; break;
    case 12: ev.key = Key::kF2; break;
    case 13: ev.key = Key::kF3; break;
    case 14: ev.key = Key::kF4; break;
    case 15: ev.key = Key::kF5; break;
    case 17: ev.key = Key::kF6; break;
    case 18: ev.key = Key::kF7; break;
    case 19: ev.key = Key::kF8; break;
    case 20: ev.key = Key::kF9; break;
    case 21: ev.key = Key::kF10; break;
    case 23: ev.key = Key::kF11; break;
    case 24: ev.key = Key::kF12; break;
    default: ev.key = Key::kNone; break;
  }
  return ev.key;
}

}  // namespace

bool Terminal::poll(Event& out, int timeout_ms) {
  out = Event{};
  std::string bytes;

#if defined(_WIN32)
  // Wait for a keypress with a bounded timeout so progress keeps animating.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (::_kbhit()) {
      const int c = ::_getch();
      if (c == 0 || c == 0xE0) {  // extended: a second byte carries the key
        const int c2 = ::_getch();
        bytes.push_back('\033');
        bytes.push_back('[');
        bytes.push_back(static_cast<char>(c2));
      } else {
        bytes.push_back(static_cast<char>(c));
      }
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    Sleep(5);
  }
#else
  struct pollfd pfd {};
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  const int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc <= 0) return false;  // timeout, or an error we treat as "nothing yet"
  char buf[256];
  const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) {
    if (n < 0 && errno == EINTR) return false;  // a signal, not EOF
    return false;
  }
  bytes.assign(buf, static_cast<std::size_t>(n));
#endif

  if (bytes.empty()) return false;

  // --- escape sequences -------------------------------------------------
  if (bytes[0] == '\033') {
    // A lone ESC (no following byte) is the Escape key; a longer run is a
    // sequence. Ambiguity is resolved the way readline does: if nothing else is
    // buffered, a bare ESC is Escape.
    if (bytes.size() == 1) {
      out.key = Key::kEscape;
      return true;
    }
    std::size_t i = 1;
    if (bytes[i] == '[' || bytes[i] == 'O') {
      const char intro = bytes[i];
      ++i;
      std::vector<int> params;
      std::string param_text;
      while (i < bytes.size() && ((bytes[i] >= '0' && bytes[i] <= '9') || bytes[i] == ';')) {
        if (bytes[i] == ';') {
          params.push_back(param_text.empty() ? 0 : std::atoi(param_text.c_str()));
          param_text.clear();
        } else {
          param_text.push_back(bytes[i]);
        }
        ++i;
      }
      if (!param_text.empty()) params.push_back(std::atoi(param_text.c_str()));
      if (i < bytes.size()) {
        csi_key(bytes[i], params, out);
        (void)intro;
        if (out.key != Key::kNone) return true;
      }
      return false;
    }
    // ESC followed by a printable byte: treat as Alt+key, which the TUI does
    // not bind, so consume both and report nothing.
    return false;
  }

  // --- control keys ------------------------------------------------------
  switch (static_cast<unsigned char>(bytes[0])) {
    case '\r':
    case '\n':
      out.key = Key::kEnter;
      return true;
    case '\t':
      out.key = Key::kTab;
      return true;
    case 0x7F:
    case 0x08:
      out.key = Key::kBackspace;
      return true;
    // Ctrl-C: raw mode has ISIG off, so this arrives as a key rather than a
    // signal, and the TUI handles it like any other quit request.
    case 0x03:
      out.key = Key::kChar;
      out.text = "\x03";
      return true;
    default:
      break;
  }

  // --- UTF-8 text --------------------------------------------------------
  // Accumulate so a multi-byte codepoint split across two reads still decodes.
  impl_->pending.append(bytes);
  const auto b0 = static_cast<unsigned char>(impl_->pending[0]);
  std::size_t need = 1;
  if ((b0 & 0xE0) == 0xC0) need = 2;
  else if ((b0 & 0xF0) == 0xE0) need = 3;
  else if ((b0 & 0xF8) == 0xF0) need = 4;
  if (impl_->pending.size() < need) return false;
  out.key = Key::kChar;
  out.text = impl_->pending.substr(0, need);
  impl_->pending.erase(0, need);
  return true;
}

}  // namespace stellar::tui
