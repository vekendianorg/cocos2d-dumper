// SPDX-License-Identifier: MIT
// The TUI front-end: six screens, keyboard navigation and one frame loop.
//
// Three rules decide everything below.
//
//  1. Nothing is invented. Every number, path and verdict on screen comes out
//     of an AnalysisSnapshot that the core filled in. A fact the snapshot does
//     not carry renders as an em dash, because a plausible-looking wrong number
//     in an analysis tool is worse than no number at all.
//  2. Nothing is styled here. Widgets ask Theme for a semantic Style and the
//     theme decides what that looks like; there is not one colour escape
//     sequence in this file. The only control sequences written are the
//     screen/cursor ones from terminal.h, gated on the terminal having ANSI.
//  3. State is never carried by colour alone. A selected row also gets a marker
//     (the spec's "▶"), a success gets "✓", a warning "!" and an error "✗",
//     so the whole interface survives a monochrome terminal.
//
// The layouts are computed, not hard-coded: every screen asks the Region it was
// given how many rows it has, drops optional blocks when there is no room, and
// puts the footer on the bottom line. That is what makes a 20x6 terminal
// produce a valid (if sparse) frame instead of a wrapped, corrupt one.
#include "stellar/tui/app.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <string>
#include <vector>
#include <string_view>
#include <utility>

#include "stellar/tui/logo.h"
#include "stellar/util/bytes.h"

namespace stellar::tui {
namespace {

// --- the spec's symbols -----------------------------------------------------
// Text carries the state; colour only decorates it.

constexpr std::string_view kTick = "✓";    // ✓ success
constexpr std::string_view kCross = "✗";   // ✗ error
constexpr std::string_view kWarn = "!";    // ! warning
constexpr std::string_view kPoint = "▶";   // ▶ selected item
constexpr std::string_view kAbsent = "—";  // — not measured, never guessed

// --- small text helpers -----------------------------------------------------

/// Byte length of the UTF-8 sequence starting at `i`.
std::size_t utf8_len(std::string_view s, std::size_t i) noexcept {
  if (i >= s.size()) return 0;
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) return 2;
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) return 3;
  if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) return 4;
  return 1;  // malformed byte: consume it alone rather than looping
}

/// 1234567 -> "1,234,567".
///
/// Grouped counters are what the spec shows, and nine raw digits are markedly
/// harder to read at a glance.
std::string group(std::uint64_t v) {
  const std::string digits = std::to_string(v);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}


/// Seconds as "mm:ss.ss". A dump of a real binary takes tens of seconds and can
/// run for minutes, so the minutes are always present.
std::string clock_mm_ss(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
  const auto whole = static_cast<long long>(seconds);
  int hundredths = static_cast<int>((seconds - static_cast<double>(whole)) * 100.0 + 0.5);
  if (hundredths > 99) hundredths = 99;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02lld:%02lld.%02d", whole / 60, whole % 60,
                hundredths);
  return buf;
}

/// Seconds as the completion screen shows them: "32.60 seconds".
std::string seconds_text(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.2f seconds", seconds);
  return buf;
}

/// The measured value, or an em dash when there is none.
///
/// This is the honest-display primitive: a counter the core never declared, an
/// empty string from the readers, an invalid file. Zero is *not* treated as
/// absent, because a real zero is a real measurement.
std::string or_absent(bool have, std::string value) {
  if (!have || value.empty()) return std::string(kAbsent);
  return value;
}

/// The file name without its directory, for the analysis screen's subtitle.
std::string basename_of(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

/// One key hint in the footer: the key itself and what it does.
struct Hint {
  std::string_view key;
  std::string_view desc;
};

/// Footer row: keys in the key style, descriptions muted, two spaces between
/// hints -- the exact shape the spec draws.
void draw_hints(Screen& s, int row, int col, int width,
                std::initializer_list<Hint> hints) {
  if (width <= 0) return;
  std::vector<Hint> list(hints);
  auto need_for = [&](const std::vector<Hint>& v, int gap) {
    int need = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (i != 0) need += gap;
      need += static_cast<int>(display_width(v[i].key) + display_width(v[i].desc));
    }
    return need;
  };
  // Prefer the roomy separator, but close the gaps up before dropping a hint.
  // Losing "Q Quit" from the footer just to advertise one more key is worse than
  // tightening the spacing, and the spec draws the whole set at 80 columns --
  // which is exactly the width where one extra hint stops fitting.
  //
  // When even tight spacing is not enough, hints are dropped from the *middle*
  // working back from the end, never the last one: by convention the last hint
  // is the way out (Quit / Back), and a footer that cannot tell you how to leave
  // is the one that strands a user on a small terminal.
  while (list.size() > 2 && need_for(list, 1) > width) {
    list.erase(list.end() - 2);
  }
  const int gap = need_for(list, 2) <= width ? 2 : 1;
  int x = col;
  bool first = true;
  for (const Hint& h : list) {
    const int kw = static_cast<int>(display_width(h.key));
    const int dw = static_cast<int>(display_width(h.desc));
    if (!first) {
      if (x + gap > col + width) return;
      s.put(row, x, gap == 2 ? "  " : " ");
      x += gap;
    }
    if (x + kw + dw > col + width) return;
    s.put(row, x, h.key, Style::kKey);
    s.put(row, x + kw, h.desc, Style::kKeyDescription);
    x += kw + dw;
    first = false;
  }
}
/// A row cursor over the content region of a frame.
///
/// Every screen paints through one of these, so "there is no room" is a single
/// condition rather than a bounds check at each line. Once the cursor passes the
/// bottom the Pen is simply full and further calls are no-ops, which is what
/// makes a 6-row terminal safe for a screen designed for 24.
class Pen {
 public:
  Pen(Screen& s, int top, int bottom, int left, int width)
      : s_(s), bottom_(bottom), left_(left), width_(width), row_(top) {}

  [[nodiscard]] bool full() const noexcept { return row_ >= bottom_; }
  [[nodiscard]] bool room(int n = 1) const noexcept { return row_ + n <= bottom_; }
  [[nodiscard]] int row() const noexcept { return row_; }
  [[nodiscard]] int left() const noexcept { return left_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int rows_left() const noexcept { return bottom_ - row_; }

  /// One line of text, clipped to the region.
  void text(std::string_view t, Style st = Style::kValue) {
    if (full()) return;
    s_.text_clipped(row_, left_, width_, t, st);
    ++row_;
  }

  /// `n` empty lines, or as many as are left.
  void blank(int n = 1) { row_ = std::min(bottom_, row_ + n); }

  void section(std::string_view name) { text(name, Style::kSection); }

  /// "Label      value", the workhorse of every results and settings block.
  ///
  /// On a narrow frame the two columns cannot both survive, so the pair
  /// collapses to one "label: value" line rather than being clipped into
  /// nonsense. `label_w` is the value column's offset from the indent.
  /// The label column is not a fixed constant: it is derived from the width
  /// actually available, so on a phone-sized terminal the value -- which carries
  /// the information -- keeps its room instead of being cut off after
  /// "20,454,". A clipped label is a cosmetic loss; a clipped count is a false
  /// statement about a number.
  ///
  /// `label_w` overrides the derivation for a caller that places its own value
  /// column (the settings screen does, because it edits values in place).
  void field(std::string_view label, std::string_view value,
             Style vs = Style::kValue, int label_w = 0) {
    if (full()) return;
    const int indent = 2;
    const int avail = width_ - indent;
    if (avail <= 0) {
      ++row_;
      return;
    }
    // Always leave a gap: a label clipped flush against its value reads as one
    // run-together word ("Compilati1,183"), which is worse than either column
    // giving up a character.
    constexpr int kGap = 2;
    constexpr int kLabelMax = 19;  // fits the longest label, incl. the gap
    const int value_w = static_cast<int>(display_width(value));
    int col = label_w;
    if (col <= 0) {
      // Start at the width the labels are authored to and only give ground when
      // this row's value actually needs it. Sizing the column to a fraction of
      // the row instead would clip every label on a narrow terminal -- turning
      // "Compilation Units" into "Compila" -- to buy space the value did not
      // ask for.
      col = kLabelMax;
      if (col > avail - (value_w + kGap)) col = avail - (value_w + kGap);
      col = std::clamp(col, 8, kLabelMax);
    }
    const int label_room = std::max(1, col - kGap);
    const bool fits = avail >= col + 6 && value_w <= avail - col;
    if (!fits) {
      // Too narrow for two columns, or the value would not survive the squeeze:
      // collapse to a single "label: value" line rather than clipping a number.
      std::string one(label);
      one += ": ";
      one += value;
      s_.text_clipped(row_, left_, width_, one, vs);
      ++row_;
      return;
    }
    s_.text_clipped(row_, left_ + indent, label_room, label, Style::kLabel);
    s_.text_clipped(row_, left_ + indent + col, avail - col, value, vs);
    ++row_;
  }


  /// A selectable row. The marker is drawn only on the selected row, which is
  /// how the selection stays legible with colour off.
  void item(std::string_view label, bool selected, std::string_view suffix = {},
            Style suffix_style = Style::kMuted) {
    if (full()) return;
    const int indent = 2;
    if (indent + 2 > width_) {
      ++row_;
      return;
    }
    s_.put(row_, left_ + indent, selected ? kPoint : "  ",
           selected ? Style::kSelected : Style::kDim);
    const int x = left_ + indent + 2;
    const int room = left_ + width_ - x;
    if (room > 0) {
      s_.text_clipped(row_, x, room, label,
                      selected ? Style::kSelected : Style::kValue);
      if (!suffix.empty()) {
        const int used = static_cast<int>(display_width(label));
        s_.text_clipped(row_, x + used, room - used, suffix, suffix_style);
      }
    }
    ++row_;
  }

  /// A checkbox row: marker, "[✓]"/"[ ]" and its label, each in its own style
  /// so the check state reads without colour.
  void checkbox(std::string_view label, bool on, bool selected,
                std::string_view suffix = {}, Style suffix_style = Style::kMuted) {
    if (full()) return;
    const int indent = 2;
    if (indent + 2 > width_) {
      ++row_;
      return;
    }
    s_.put(row_, left_ + indent, selected ? kPoint : "  ",
           selected ? Style::kSelected : Style::kDim);
    int x = left_ + indent + 2;
    s_.put(row_, x, on ? "[✓]" : "[ ]",
           on ? Style::kCheckboxOn : Style::kCheckboxOff);
    x += 3;
    if (x >= left_ + width_) {
      ++row_;
      return;
    }
    s_.put(row_, x, " ");
    ++x;
    const int room = left_ + width_ - x;
    if (room > 0) {
      s_.text_clipped(row_, x, room, label,
                      selected ? Style::kSelected : Style::kValue);
      if (!suffix.empty()) {
        const int used = static_cast<int>(display_width(label));
        s_.text_clipped(row_, x + used, room - used, suffix, suffix_style);
      }
    }
    ++row_;
  }

  /// A centred line, the shape the spec uses for buttons.
  void centered(std::string_view t, Style st) {
    if (full()) return;
    const int w = static_cast<int>(display_width(t));
    const int x = left_ + std::max(0, (width_ - w) / 2);
    s_.text_clipped(row_, x, left_ + width_ - x, t, st);
    ++row_;
  }

 private:
  Screen& s_;
  int bottom_;
  int left_;
  int width_;
  int row_;
};

/// "not applied" -- the honest suffix on a control this build cannot honour.
///
/// TODO.md asks the settings screen for a thread limit, a RAM limit and a
/// parallel mode. None of them exist: the core runs one worker thread over one
/// compilation unit at a time, and nothing in the process can be capped from
/// outside. Drawing them as if they worked would be the one genuinely
/// dishonest thing this UI could do, so they are shown muted and labelled.
constexpr std::string_view kNotApplied = " (not applied)";

}  // namespace

// --- the frame --------------------------------------------------------------

/// The outer box every screen shares: title, optional right-aligned subtitle,
/// the separator, and the footer hints.
///
/// Four rows are spent on this. That is the floor the layout assumes, so the
/// 6-row minimum size still has two content rows rather than a negative
/// height.
/// The box is deliberately `cols-1` wide and leaves the terminal's final column
/// untouched. Writing a character into the last column of a row arms the
/// pending-wrap flag, and on several emulators -- Android's included -- that
/// flag survives the carriage return that follows, so every subsequent line
/// shifts sideways and the frame visibly falls apart. The symptoms are a right
/// border that appears in fragments and content that looks as though it escaped
/// the box. Not writing the last cell removes the hazard; Screen::render_row
/// enforces the same rule, and the erase-to-end-of-line that follows each row
/// clears whatever previous frames left in the column we skip.
void draw_frame(Screen& s, int cols, int rows, std::string_view title,
                std::string_view right, std::initializer_list<Hint> hints) {
  const int frame_w = std::max(1, cols - 1);
  s.box(0, 0, frame_w, rows, Style::kBorder, title, Style::kTitle);
  if (frame_w > 2 && rows > 3) {
    // Subtitle, right-aligned one column inside the right border, and only
    // when it cannot collide with the title.
    const int w = static_cast<int>(display_width(right));
    const int at = frame_w - 2 - w;
    const int title_end = 2 + static_cast<int>(display_width(title));
    if (!right.empty() && at > title_end + 1) s.put(0, at, right, Style::kMuted);
    const int inner = frame_w - 2;
    s.hline(rows - 3, 1, inner, Style::kBorder);
    draw_hints(s, rows - 2, 1, inner, hints);
  }
}

/// Footer hints, per screen, exactly as the spec spells them.
constexpr std::initializer_list<Hint> kMainHints{
    {"↑↓", " Navigate"},   {"Enter", " Select"}, {"Tab", " Switch Panel"},
    {"R", " Run"},        {"S", " Settings"},   {"Q", " Quit"}};
/// With the input field focused every printable key is text, so Q/R/S are not
/// shortcuts there and advertising them would be a lie. Enter and Tab are the
/// only ways out of the field, and Ctrl-C always quits.
constexpr std::initializer_list<Hint> kMainInputHints{
    {"Enter", " Run"}, {"Tab", " Menu"}, {"^C", " Quit"}};
constexpr std::initializer_list<Hint> kEmitHints{
    {"↑↓", " Navigate"}, {"Space", " Toggle"}, {"Enter", " Edit"},
    {"Esc", " Back"},    {"R", " Run"}};
constexpr std::initializer_list<Hint> kAnalysisHints{{"Q", " Cancel"}};
constexpr std::initializer_list<Hint> kCompleteHints{
    {"↑↓", " Navigate"}, {"Enter", " Select"}, {"Esc", " Back"}, {"Q", " Quit"}};
constexpr std::initializer_list<Hint> kSettingsHints{
    {"↑↓", " Navigate"}, {"Enter", " Edit"}, {"Space", " Toggle"},
    {"Esc", " Back"},    {"S", " Save"}};
constexpr std::initializer_list<Hint> kInfoHints{{"Esc", " Back"}};

// --- construction -----------------------------------------------------------

App::App() = default;

App::App(Theme theme) : theme_(std::move(theme)) {}

App::~App() {
  // Nothing to undo here: Terminal restores itself in its own destructor and
  // Analysis joins its worker there. This body exists so the terminal is
  // released even if run() threw.
  term_.leave();
}

App::Region App::content_region(int cols, int rows) noexcept {
  Region r;
  r.left = 1;
  // The frame is one column narrower than the terminal (see draw_frame), so the
  // interior is cols-3 wide: one column for each border, plus the final column
  // that is deliberately never written.
  r.width = cols - 3;
  if (r.width < 1) r.width = 1;
  r.top = 1;
  r.bottom = rows - 3;  // the separator row
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

// --- painting ---------------------------------------------------------------

void App::paint(Screen& s) const {
  s.clear();
  // One size decision, made once, before any screen paints. Every screen below
  // may then assume it has at least kMinCols x kMinRows to work with.
  if (s.cols() < kMinCols || s.rows() < kMinRows) {
    paint_too_small(s);
    return;
  }
  const Region r = content_region(s.cols(), s.rows());
  switch (screen_) {
    case ScreenId::kMain: paint_main(s, r); break;
    case ScreenId::kEmit: paint_emit(s, r); break;
    case ScreenId::kAnalysis: paint_analysis(s, r); break;
    case ScreenId::kComplete: paint_complete(s, r); break;
    case ScreenId::kSettings: paint_settings(s, r); break;
    case ScreenId::kInfo: paint_info(s, r); break;
  }
}

void App::paint_too_small(Screen& s) const {
  // Wording tiers, longest first; the first one that fits the width wins. The
  // final column is never written (see draw_frame), so the usable width is
  // cols-1.
  const int avail = std::max(1, s.cols() - 1);
  const std::string have = std::to_string(s.cols()) + "x" + std::to_string(s.rows());
  const std::string need = std::to_string(kMinCols) + "x" + std::to_string(kMinRows);
  const std::string_view titles[] = {"Terminal too small", "Too small", "Small", "!"};
  const std::string details[] = {"need " + need + ", have " + have,
                                 need + " / " + have, have};
  std::string title = "!";
  for (std::string_view t : titles) {
    if (static_cast<int>(display_width(t)) <= avail) {
      title = std::string(t);
      break;
    }
  }
  std::vector<std::string> lines{title};
  for (const std::string& d : details) {
    if (static_cast<int>(display_width(d)) <= avail) {
      lines.push_back(d);
      break;
    }
  }
  if (static_cast<int>(lines.size()) > s.rows()) lines.resize(static_cast<std::size_t>(s.rows()));
  const int top = std::max(0, (s.rows() - static_cast<int>(lines.size())) / 2);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const int w = static_cast<int>(display_width(lines[i]));
    const int x = std::max(0, (avail - w) / 2);
    s.text_clipped(top + static_cast<int>(i), x, avail - x, lines[i],
                   i == 0 ? Style::kWarning : Style::kMuted);
  }
}

void App::draw_logo(Screen& s, int row, int col) const {
  // render_logo() owns the artwork and the two-tone treatment; this walks the
  // string it returns and paints each glyph as a cell, tracking which of the
  // two styles is in force. Going through render_logo (rather than drawing
  // kLogoLines directly) is the point: the banner bytes and its colours are
  // decided in one place, and a no-colour theme emits no escape sequence at
  // all, so this loop copies plain glyphs.
  const std::string art = render_logo(theme_);
  Style st = Style::kLogo;
  int y = row;
  int x = col;  // running column, advanced per glyph
  for (std::size_t i = 0; i < art.size();) {
    if (art[i] == '\033') {
      const std::size_t end = art.find('m', i);
      if (end == std::string::npos) break;
      const std::string_view esc(art.data() + i, end - i + 1);
      if (esc == theme_.paint(Style::kAccent)) {
        st = Style::kAccent;
      } else if (esc == theme_.paint(Style::kLogo)) {
        st = Style::kLogo;
      }
      i = end + 1;
      continue;
    }
    if (art[i] == '\n') {
      ++y;
      x = col;  // each banner row starts back at the left margin
      ++i;
      continue;
    }
    const std::size_t len = utf8_len(art, i);
    const std::string_view glyph(art.data() + i, len);
    s.put(y, x, glyph, st);
    // Advance by the glyph's *display* width, not its byte length: the banner is
    // full of three-byte box-drawing characters, and a fixed step would smear
    // the artwork across the row.
    x += static_cast<int>(display_width(glyph));
    i += len;
  }
}

void App::draw_field(Screen& s, int row, int col, int width, const Field& f,
                     std::string_view prompt, bool caret) const {
  if (width <= 0) return;
  s.fill(row, col, width, 1, Style::kValue);
  int x = col;
  const int pw = static_cast<int>(display_width(prompt));
  if (pw > 0 && pw < width) {
    s.put(row, x, prompt, Style::kAccent);
    x += pw;
  }
  const int text_w = col + width - x;
  if (text_w <= 0) return;

  // One codepoint at a time, so the character under the cursor can be painted
  // differently from the rest of the value.
  bool cursor_drawn = false;
  int cell = 0;
  for (std::size_t i = 0; i < f.text.size();) {
    const std::size_t len = utf8_len(f.text, i);
    if (cell >= f.scroll) {
      const int cx = x + (cell - f.scroll);
      if (cx >= col + width) break;
      const bool at_cursor = caret && i == f.cursor;
      s.put(row, cx, f.text.substr(i, len),
            at_cursor ? Style::kSelected : Style::kPath);
      if (at_cursor) cursor_drawn = true;
    }
    cell += static_cast<int>(display_width(f.text.substr(i, len)));
    i += len;
  }
  // A cursor past the last character sits on a reversed blank: the alternate
  // screen hides the hardware cursor, so the block is the caret.
  if (caret && !cursor_drawn) {
    const int cx = x + (cell - f.scroll);
    if (cx >= col && cx < col + width) s.put(row, cx, " ", Style::kSelected);
  }
}


/// The main screen: banner, input field, file facts, action menu, output path.
///
/// The row budget is decided up front. Four blocks compete for the content
/// rows, and when the frame is short they are given up in reverse order of
/// usefulness: the banner is identity, the file block and the output path are
/// reference, and the input field plus the menu are the two things the user
/// cannot do without. A 24-row terminal keeps everything but the banner, which
/// needs 57 columns and six rows it does not have next to the rest.
void App::paint_main(Screen& s, const Region& r) const {
  constexpr int kInputRows = 4;  // section + a three-row framed field
  constexpr int kMenuRows = 5;   // section + four actions
  constexpr int kFileRows = 6;   // section + five facts
  constexpr int kOutRows = 2;    // section + the path
  const int logo_rows = static_cast<int>(kLogoRows);
  const int h = r.bottom - r.top;

  const bool wide_enough = s.cols() >= static_cast<int>(kLogoWidth) + 4;
  const bool show_logo =
      wide_enough && h >= kInputRows + kFileRows + kMenuRows + kOutRows + logo_rows;
  const bool show_file = h - (show_logo ? logo_rows : 0) >=
                         kInputRows + kFileRows + kMenuRows + kOutRows;
  const int used = kInputRows + kMenuRows + (show_file ? kFileRows + kOutRows : 0) +
                   (show_logo ? logo_rows : 0);
  // The banner already spells the name, so the border title would only repeat
  // it. When the banner does not fit (small terminal) the title comes back,
  // because then nothing else on screen says what this is.
  draw_frame(s, s.cols(), s.rows(), show_logo ? std::string_view{} : "STELLAR",
             "Native ELF / DWARF Analysis",
             main_panel_ == 0 ? kMainInputHints : kMainHints);

  // Spare rows become breathing room between the blocks, capped at two so a
  // tall terminal does not turn into a column of white.
  const int gap = show_file ? std::clamp((h - used) / 4, 0, 2) : 0;

  Pen pen(s, r.top, r.bottom, r.left, r.width);
  const FileFacts& f = snap_.file;

  if (show_logo) {
    // Centred on the interior, by the banner's real display width (measured, not
    // assumed): the widest row decides, so the art keeps its shape and only
    // moves as a whole. Floor division puts any odd spare column on the right.
    int art_w = 0;
    for (const std::string_view row : kLogoLines) {
      art_w = std::max(art_w, static_cast<int>(display_width(row)));
    }
    const int x = r.left + std::max(0, (r.width - art_w) / 2);
    draw_logo(s, pen.row(), x);
    pen.blank(logo_rows);
  }
  if (gap) pen.blank(gap);

  // --- INPUT ---------------------------------------------------------------
  pen.section("INPUT");
  if (pen.room(3) && r.width >= 8) {
    s.box(pen.row(), r.left, r.width, 3,
          main_panel_ == 0 ? Style::kActiveBorder : Style::kBorder);
    draw_field(s, pen.row() + 1, r.left + 1, r.width - 2, input_, "> ",
               main_panel_ == 0);
    pen.blank(3);
  } else {
    // Too small for a frame: the value still needs to be visible and editable.
    draw_field(s, pen.row(), r.left, r.width, input_, "> ", main_panel_ == 0);
    pen.blank(1);
  }


  // --- FILE INFORMATION ----------------------------------------------------
  if (show_file) {
    if (gap) pen.blank(gap);
    pen.section("FILE INFORMATION");
    pen.field("Format", or_absent(f.valid, f.format));
    pen.field("Architecture", or_absent(f.valid, f.machine));
    pen.field("Size", or_absent(f.valid, f.size_text));
    if (!f.valid) {
      pen.field("DWARF", std::string(kAbsent));
    } else {
      pen.field("DWARF", f.has_dwarf ? "✓ Available" : std::string("✗ Not available"),
                f.has_dwarf ? Style::kSuccess : Style::kWarning);
    }
    pen.field("Compilation Units", or_absent(f.valid, group(f.unit_total)),
              Style::kNumber);
    if (!f.valid) {
      // The readers publish these facts only as part of a run, and a fake
      // zero here would be the single most misleading cell in the interface.
      pen.text("no analysis has read this file yet", Style::kMuted);
    }
    if (gap) pen.blank(gap);
  }

  // --- ANALYSIS ------------------------------------------------------------
  pen.section("ANALYSIS");
  static constexpr std::string_view kActions[] = {
      "Inspect ELF / DWARF", "Browse compilation units", "Scan DWARF",
      "Generate C# dump"};
  for (int i = 0; i < 4; ++i) {
    // The marker tracks the highlighted action, not which panel holds the
    // keyboard focus. The spec's main screen shows the input box filled in *and*
    // the first action marked, and keeping the marker visible while the field
    // is being typed into is also the honest thing: it never lets the user lose
    // sight of what Enter will run.
    pen.item(kActions[i], main_item_ == i);
  }

  // --- OUTPUT --------------------------------------------------------------
  if (show_file) {
    if (gap) pen.blank(gap);
    pen.section("OUTPUT");
    pen.field("Path", or_absent(!out_path_.text.empty(), out_path_.text),
              Style::kPath);
  }
}

/// The emit screen: what is about to be run, the options that shape it, and
/// the one button that starts it.
void App::paint_emit(Screen& s, const Region& r) const {
  draw_frame(s, s.cols(), s.rows(), "STELLAR / EMIT", {}, kEmitHints);

  // INPUT(2) + OUTPUT(2) + OPTIONS(1 + 4) = 9 rows before the button. Below
  // that total the button would push the options off the frame, so the screen
  // degrades to the options alone and the button is left to Enter/R.
  constexpr int kSectionRows = 9;
  const int h = r.bottom - r.top;
  const bool show_button = h >= kSectionRows + 1;
  const int button_row = r.bottom - 1;
  Pen pen(s, r.top, show_button ? button_row - 1 : r.bottom, r.left, r.width);

  pen.section("INPUT");
  draw_field(s, pen.row(), r.left, r.width, input_, "", false);
  pen.blank(1);

  pen.section("OUTPUT");
  draw_field(s, pen.row(), r.left, r.width, out_path_, "  Path: ", editing_out_);
  pen.blank(1);

  pen.section("OPTIONS");
  pen.checkbox("Include methods", opt_methods_,
               emit_item_ == static_cast<int>(EmitItem::kMethods));
  pen.checkbox("Synthesize padding fields", opt_pad_,
               emit_item_ == static_cast<int>(EmitItem::kPadding));
  // Analysis::StartOptions has no build-units field, so this toggle cannot
  // reach the emitter. It is shown, it says so, and it is not pretended with.
  pen.checkbox("Build compilation-unit information", opt_units_,
               emit_item_ == static_cast<int>(EmitItem::kUnits), kNotApplied);
  pen.item("Maximum output lines: " +
               (max_lines_ != 0 ? group(max_lines_) : std::string("unlimited")),
           emit_item_ == static_cast<int>(EmitItem::kMaxLines));

  if (show_button) {
    pen.blank(std::clamp(h - (kSectionRows + 1), 0, 2));
    static constexpr std::string_view kStart = "[ START EMIT ]";
    const int w = static_cast<int>(display_width(kStart));
    const bool on = emit_item_ == static_cast<int>(EmitItem::kStart);
    s.text_clipped(button_row, r.left + std::max(0, (r.width - w) / 2), r.width,
                   kStart, on ? Style::kButtonSelected : Style::kButton);
  }
}

/// The progress screen. One in-place display, redrawn in place by the frame
/// loop -- never a scrolling log.
///
/// Every number here is live from the snapshot. The two totals that DWARF does
/// not carry (there is no DIE count in the format) are shown as an em dash
/// rather than as a zero, and a run with no known total gets an empty bar and a
/// dash instead of a percentage nobody can compute.
void App::paint_analysis(Screen& s, const Region& r) const {
  using Phase = AnalysisSnapshot::Phase;
  draw_frame(s, s.cols(), s.rows(), "STELLAR / ANALYSIS", basename_of(input_.text),
             kAnalysisHints);
  Pen pen(s, r.top, r.bottom, r.left, r.width);
  const AnalysisSnapshot& k = snap_;

  pen.section("STAGE");
  // The mode is real information the core decided (dwarf vs dwarfless), and it
  // belongs next to the stage: which reader produced these numbers changes how
  // much they are worth.
  if (!k.mode_label.empty()) {
    // Shown upper-case: it names a mode, not a running status, and the spec
    // sets it as a heading. The value itself stays the core's lower-case word.
    std::string mode = k.mode_label;
    for (char& ch : mode) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    pen.text("  " + mode, Style::kAccent);
  }
  pen.text("  " + (k.stage.empty() ? std::string(Analysis::phase_name(k.phase))
                                   : k.stage),
           Style::kFocused);
  if (k.phase == Phase::kFailed) {
    pen.text("  " + std::string(kCross) + " " + k.error, Style::kError);
  } else if (k.phase == Phase::kCancelled) {
    pen.text("  " + std::string(kWarn) + " cancelled by request", Style::kWarning);
  }

  // --- progress ------------------------------------------------------------
  pen.blank(1);
  if (!pen.full()) {
    const int row = pen.row();
    const int left = r.left + 2;
    const int label_w = 12;  // "Units" in the label column
    const int bar_col = left + label_w;
    const bool have_total = k.units_total != 0;
    const std::string counts =
        group(k.units) + " / " +
        (have_total ? group(k.units_total) : std::string(kAbsent));
    std::string pct(kAbsent);
    if (have_total) {
      const double frac = static_cast<double>(k.units) / static_cast<double>(k.units_total);
      int v = static_cast<int>(frac * 100.0 + 0.5);
      if (v > 100) v = 100;
      pct = std::to_string(v) + "%";
    }
    const int pct_w = static_cast<int>(display_width(pct));
    const int counts_w = static_cast<int>(display_width(counts));
    const int tail = 2 + pct_w + 4 + counts_w;
    const int bar_w = r.width - (label_w + 2) - tail - 1;
    s.put(row, left, "Units", Style::kLabel);
    if (bar_w >= 4) {
      const double frac =
          have_total ? static_cast<double>(k.units) / static_cast<double>(k.units_total)
                     : 0.0;
      s.progress_bar(row, bar_col, bar_w, frac);
      s.text_clipped(row, bar_col + bar_w + 2, pct_w, pct,
                     have_total ? Style::kFocused : Style::kMuted);
      s.text_clipped(row, bar_col + bar_w + 2 + pct_w + 4, counts_w, counts,
                     Style::kNumber);
    } else {
      // A frame too narrow for a bar still gets the counts: the number is the
      // information, the bar is only the picture of it.
      s.text_clipped(row, bar_col, r.left + r.width - bar_col, counts,
                     Style::kNumber);
    }
    pen.blank(1);
  }

  // --- counters ------------------------------------------------------------
  pen.field("DIEs", group(k.dies), Style::kNumber);
  pen.field("Types", group(k.types), Style::kNumber);
  pen.field("Fields", group(k.fields), Style::kNumber);
  pen.field("Methods", group(k.methods), Style::kNumber);

  // --- current operation ---------------------------------------------------
  pen.blank(1);
  pen.section("CURRENT");
  pen.text("  " + (k.current_note.empty()
                       ? (k.stage.empty() ? std::string(kAbsent) : k.stage)
                       : k.current_note),
           Style::kCurrentOperation);

  // --- memory and clock ----------------------------------------------------
  // Pinned to the bottom of the frame so the numbers below them do not jump
  // while the counters above them tick.
  if (pen.rows_left() >= 4) {
    Pen tail(s, r.bottom - 2, r.bottom, r.left, r.width);
    tail.field("Memory",
               k.rss_bytes != 0 ? util::human_size(k.rss_bytes) : std::string(kAbsent));
    tail.field("Elapsed", clock_mm_ss(k.elapsed_seconds));
  }
}


/// The completion screen: the verdict, the final counts, and three buttons.
///
/// A run that failed or was cancelled gets the same screen with a different
/// first line and no results block, because there are no results. Reporting
/// partial counters as a finished analysis is the failure mode this file exists
/// to avoid.
void App::paint_complete(Screen& s, const Region& r) const {
  using Phase = AnalysisSnapshot::Phase;
  draw_frame(s, s.cols(), s.rows(), "STELLAR / COMPLETE", {}, kCompleteHints);
  const AnalysisSnapshot& k = snap_;
  const bool ok = k.phase == Phase::kDone;
  const int h = r.bottom - r.top;

  // The button block is pinned to the bottom of the frame so it is always in
  // the same place; the report above it takes what is left.
  const bool pin = h >= 17;
  const int needed = 1 + 2 + (ok ? 6 : 0) + 3 + 2;
  const int blocks = ok ? 5 : 4;
  const int gap = std::clamp((h - needed - (pin ? 3 : 0)) / blocks, 0, 1);
  Pen pen(s, r.top, pin ? r.bottom - 3 : r.bottom, r.left, r.width);

  if (ok) {
    pen.text("  " + std::string(kTick) + " Analysis complete.", Style::kSuccess);
  } else if (k.phase == Phase::kFailed) {
    pen.text("  ✗ Analysis failed.", Style::kError);
    if (!k.error.empty()) pen.text("    " + k.error, Style::kError);
  } else {
    pen.text("  ! Analysis cancelled.", Style::kWarning);
  }

  if (gap) pen.blank(gap);
  pen.section("Input");
  pen.text("  " + or_absent(!input_.text.empty(), input_.text), Style::kPath);

  if (ok) {
    if (gap) pen.blank(gap);
    pen.section("Results");
    pen.field("Compilation Units", group(k.units), Style::kNumber);
    pen.field("DIEs", group(k.dies), Style::kNumber);
    pen.field("Types", group(k.types), Style::kNumber);
    pen.field("Fields", group(k.fields_total != 0 ? k.fields_total : k.fields),
              Style::kNumber);
    pen.field("Methods", group(k.methods_total != 0 ? k.methods_total : k.methods),
              Style::kNumber);
  }

  if (gap) pen.blank(gap);
  pen.section("Output");
  pen.text("  " + or_absent(!k.out_path.empty(), k.out_path), Style::kPath);
  // out_bytes is the emitter's own counter, final once kDone is reached.
  pen.text("  " + (k.out_bytes != 0 ? util::human_size(k.out_bytes)
                                   : std::string(kAbsent)),
           Style::kValue);

  if (gap) pen.blank(gap);
  pen.section("Time");
  pen.field("Elapsed", seconds_text(k.elapsed_seconds));

  static constexpr std::string_view kButtons[] = {"[ OPEN OUTPUT ]",
                                                 "[ BACK TO MAIN ]", "[ QUIT ]"};
  // Centre the group by its widest member rather than each label on its own.
  // Centring them individually puts labels of different widths on different
  // columns, which reads as a staircase; the spec draws all three starting at
  // one column with the block as a whole centred.
  int widest = 0;
  for (const std::string_view b : kButtons) {
    widest = std::max(widest, static_cast<int>(display_width(b)));
  }
  const int start = r.left + std::max(0, (r.width - widest) / 2);
  const int room = r.left + r.width - start;
  for (int i = 0; i < 3 && room > 0; ++i) {
    const int row = pin ? r.bottom - 3 + i : pen.row();
    if (row >= r.bottom) break;
    s.text_clipped(row, start, room, kButtons[i],
                   complete_item_ == i ? Style::kButtonSelected : Style::kButton);
  }
}


/// The settings screen.
///
/// TODO.md asks for a thread limit, a RAM limit and a parallel mode. This build
/// has none of the three: the core runs a single worker over one compilation
/// unit at a time, and nothing in the process can be capped from the UI. Those
/// four rows are therefore drawn muted and labelled "(not applied)" -- a switch
/// that looks live but does nothing is the one thing a binary-analysis tool must
/// not ship. The three rows that *are* real (progress mode, redraw interval,
/// output directory) change this session's behaviour immediately.
void App::paint_settings(Screen& s, const Region& r) const {
  draw_frame(s, s.cols(), s.rows(), "STELLAR / SETTINGS", {}, kSettingsHints);
  Pen pen(s, r.top, r.bottom, r.left, r.width);
  const int indent = 2;
  // Derived from the row that is actually available, not a fixed 18: on a
  // phone-width terminal an 18-column label squeezes the value into a sliver
  // ("[ AUTO " instead of "[ AUTO ]"). The column is the width the labels are
  // authored to, given up only as far as the value column genuinely needs --
  // these values are edited in place, so the field has to stay usable.
  constexpr int kValueNeed = 12;  // "[ 8192 MB ]" plus a little
  constexpr int kLabelMax = 19;
  const int avail = r.width - indent;
  const int label_w = std::clamp(avail - kValueNeed - 2, 8, kLabelMax);
  const int value_x = r.left + indent + label_w;
  const int value_room = r.left + r.width - value_x;
  const bool two_column = avail >= label_w + 8;

  /// One "Label   [ value ]" row, with the value in its own style and an
  /// optional muted suffix hanging off the end of it.
  const auto setting = [&](std::string_view label, const std::string& value,
                           Style vs, std::string_view suffix = {}) {
    if (pen.full()) return;
    const int row = pen.row();
    pen.field(label, value, vs, label_w);
    if (two_column && !suffix.empty() && value_room > 0) {
      const int used = std::min(value_room, static_cast<int>(display_width(value)));
      s.text_clipped(row, value_x + used, value_room - used, suffix, Style::kMuted);
    }
  };
  const auto sel = [this](Setting which) {
    return setting_ == static_cast<int>(which);
  };

  // --- PERFORMANCE ---------------------------------------------------------
  pen.section("PERFORMANCE");
  setting("Threads Limit", "[ " + std::to_string(threads_limit_) + " ]", Style::kMuted,
          kNotApplied);
  setting("RAM Limit", "[ " + std::to_string(ram_limit_mb_) + " MB ]", Style::kMuted,
          kNotApplied);
  // Two more switches that the core cannot honour. They keep their checkboxes
  // because the spec asks for them, and they keep the label that says so.
  pen.checkbox("Adaptive resource usage", adaptive_, sel(Setting::kAdaptive),
               kNotApplied);
  pen.checkbox("Parallel analysis", parallel_, sel(Setting::kParallel), kNotApplied);

  // --- PROGRESS ------------------------------------------------------------
  pen.blank(1);
  pen.section("PROGRESS");
  static constexpr std::string_view kModes[] = {"AUTO", "ALWAYS", "NEVER"};
  setting("Progress Mode",
          "[ " + std::string(kModes[progress_mode_ < 0 || progress_mode_ > 2
                                       ? 0
                                       : progress_mode_]) +
              " ]",
          sel(Setting::kProgressMode) ? Style::kSelected : Style::kValue);
  if (editing_interval_) {
    if (pen.room() && value_room > 0) {
      draw_field(s, pen.row(), value_x, value_room, interval_edit_, "", true);
      pen.blank(1);
    }
  } else {
    setting("Redraw Interval", "[ " + std::to_string(redraw_ms_) + " ms ]",
            sel(Setting::kInterval) ? Style::kSelected : Style::kValue);
  }

  // --- OUTPUT --------------------------------------------------------------
  pen.blank(1);
  pen.section("OUTPUT");
  if (editing_dir_) {
    if (pen.room() && value_room > 0) {
      draw_field(s, pen.row(), value_x, value_room, out_dir_, "", true);
      pen.blank(1);
    }
  } else {
    setting("Output Directory", "[ " + or_absent(!out_dir_.text.empty(), out_dir_.text) + " ]",
            sel(Setting::kDir) ? Style::kSelected : Style::kPath);
  }

  pen.blank(1);
  pen.text("NOTE: Large ELF/DWARF inputs may require substantial memory.",
           Style::kMuted);
}


/// The info screen: what the ELF/DWARF readers know about the input.
///
/// This is the TUI face of `stellar info`, so it renders FileFacts as-is. The
/// readers only publish those facts as part of a run; before one has happened
/// every field is a dash rather than a guess.
void App::paint_info(Screen& s, const Region& r) const {
  draw_frame(s, s.cols(), s.rows(), "STELLAR / INFO", {}, kInfoHints);
  Pen pen(s, r.top, r.bottom, r.left, r.width);
  const FileFacts& f = snap_.file;

  pen.section("FILE");
  pen.field("Path", or_absent(f.valid, f.valid ? f.path : input_.text), Style::kPath);
  pen.field("Format", or_absent(f.valid, f.format));
  pen.field("Architecture", or_absent(f.valid, f.machine));
  std::string endian;
  if (f.endianness == "little") {
    endian = "Little Endian";
  } else if (f.endianness == "big") {
    endian = "Big Endian";
  } else if (!f.endianness.empty()) {
    endian = f.endianness;  // whatever the reader called it
  }
  pen.field("Endianness", or_absent(!endian.empty(), endian));
  pen.field("File Size", or_absent(f.valid, f.size_text));

  pen.blank(1);
  pen.section("DWARF");
  pen.field("Compilation Units", or_absent(f.valid, group(f.unit_total)),
            Style::kNumber);
  // DWARF carries no DIE count, so the total is unknowable before a scan; the
  // walked count is real but absent until one has run, hence the dash at zero.
  pen.field("DIEs", snap_.dies != 0 ? group(snap_.dies) : std::string(kAbsent),
            Style::kNumber);
  if (!f.has_dwarf && f.valid) {
    pen.text("  " + std::string(kCross) + " no .debug_info/.debug_abbrev in this file",
             Style::kWarning);
  }

  pen.blank(1);
  pen.section("SECTIONS");
  if (f.debug_sections.empty()) {
    pen.text("  " + std::string(kAbsent) + " no .debug_* sections reported",
             Style::kMuted);
  } else {
    for (const auto& sec : f.debug_sections) {
      pen.field(sec.first, util::human_size(sec.second), Style::kNumber, 20);
    }
  }
}


// --- field editing ----------------------------------------------------------
//
// One implementation for every editable value in the UI. Each step moves over
// whole codepoints, because splitting a UTF-8 character would leave a value
// that no longer names the file the user meant.

void App::field_insert(Field& f, std::string_view utf8) {
  const std::size_t at = std::min(f.cursor, f.text.size());
  f.text.insert(at, utf8);
  f.cursor = at + utf8.size();
}

void App::field_backspace(Field& f) {
  if (f.cursor == 0) return;
  std::size_t at = f.cursor - 1;
  while (at > 0 && (static_cast<unsigned char>(f.text[at]) & 0xC0) == 0x80) --at;
  f.text.erase(at, f.cursor - at);
  f.cursor = at;
}

void App::field_delete_forward(Field& f) {
  if (f.cursor >= f.text.size()) return;
  std::size_t end = f.cursor + 1;
  while (end < f.text.size() &&
         (static_cast<unsigned char>(f.text[end]) & 0xC0) == 0x80) {
    ++end;
  }
  f.text.erase(f.cursor, end - f.cursor);
}

void App::field_move(Field& f, int delta) {
  if (delta < 0) {
    if (f.cursor == 0) return;
    std::size_t at = f.cursor - 1;
    while (at > 0 && (static_cast<unsigned char>(f.text[at]) & 0xC0) == 0x80) --at;
    f.cursor = at;
    return;
  }
  if (f.cursor >= f.text.size()) return;
  std::size_t at = f.cursor + 1;
  while (at < f.text.size() && (static_cast<unsigned char>(f.text[at]) & 0xC0) == 0x80) {
    ++at;
  }
  f.cursor = at;
}

void App::field_home_end(Field& f, bool home) {
  f.cursor = home ? 0 : f.text.size();
}

void App::field_ensure_visible(Field& f, int view_cols) const {
  if (view_cols < 1) view_cols = 1;
  if (f.cursor > f.text.size()) f.cursor = f.text.size();
  while (f.cursor > 0 &&
         (static_cast<unsigned char>(f.text[f.cursor]) & 0xC0) == 0x80) {
    --f.cursor;
  }
  const int at =
      static_cast<int>(display_width(std::string_view(f.text).substr(0, f.cursor)));
  if (at < f.scroll) {
    f.scroll = at;
  } else if (at >= f.scroll + view_cols) {
    f.scroll = at - view_cols + 1;
  }
  if (f.scroll < 0) f.scroll = 0;
}

bool App::field_key(Field& f, const Event& e) {
  switch (e.key) {
    case Key::kChar: {
      // A paste arrives as one event holding a run of characters, and it may
      // carry control bytes. Only printable ones belong in a path.
      std::string clean;
      for (const char ch : e.text) {
        const auto b = static_cast<unsigned char>(ch);
        if (b >= 0x20 && b != 0x7F) clean.push_back(ch);
      }
      if (!clean.empty()) field_insert(f, clean);
      return true;
    }
    case Key::kBackspace: field_backspace(f); return true;
    case Key::kDelete: field_delete_forward(f); return true;
    case Key::kLeft: field_move(f, -1); return true;
    case Key::kRight: field_move(f, 1); return true;
    case Key::kHome: field_home_end(f, true); return true;
    case Key::kEnd: field_home_end(f, false); return true;
    default: return false;
  }
}

void App::sync_view() {
  // How many columns each field actually has to show its value. These are the
  // same numbers the painters use, so the caret cannot be scrolled off.
  const int cols = cur_.cols();
  field_ensure_visible(input_, std::max(4, cols - 6));
  field_ensure_visible(out_path_, std::max(4, cols - 10));
  field_ensure_visible(max_lines_edit_, 16);
  field_ensure_visible(interval_edit_, 8);
  field_ensure_visible(out_dir_, std::max(4, cols - 24));
}


// --- key handling -----------------------------------------------------------

void App::handle_key(const Event& e) {
  dirty_ = true;
  // Raw mode disables ISIG, so Ctrl-C arrives as a byte instead of a signal
  // and has to be honoured here or the user cannot stop the program.
  if (e.key == Key::kChar && e.text.find('\x03') != std::string::npos) {
    quit_ = true;
    return;
  }
  if (e.key == Key::kEscape || e.key == Key::kEnter) {
    // A transient message has been read by the time the user acts on it.
    status_.clear();
    status_kind_ = 0;
  }
  switch (screen_) {
    case ScreenId::kMain: handle_main_key(e); break;
    case ScreenId::kEmit: handle_emit_key(e); break;
    case ScreenId::kSettings: handle_settings_key(e); break;
    case ScreenId::kComplete: handle_complete_key(e); break;
    case ScreenId::kInfo:
      if (e.key == Key::kEscape) set_screen(ScreenId::kMain);
      break;
    case ScreenId::kAnalysis:
      // Q cancels. Esc does too: the key every terminal offers as "get me out
      // of here" must not leave a user watching a 30-second dump.
      if (e.key == Key::kEscape) {
        analysis_.request_cancel();
      } else if (e.key == Key::kChar && (e.text == "q" || e.text == "Q")) {
        analysis_.request_cancel();
      }
      break;
  }
}

void App::handle_main_key(const Event& e) {
  // Single-letter shortcuts (Q quit, R run, S settings) belong to the *menu*
  // panel only. In the input field every printable key is text: a path is full
  // of 'r', 'q' and 's' ("/storage/emulated/0/..."), and a shortcut that steals
  // them makes the field impossible to type into. Enter runs from either panel,
  // Tab/Esc moves focus to the menu, and Ctrl-C quits from anywhere.
  if (e.key == Key::kChar && main_panel_ == 1) {
    if (e.text == "q" || e.text == "Q") {
      quit_ = true;
      return;
    }
    if (e.text == "r" || e.text == "R") {
      run_main_action(main_item_);
      return;
    }
    if (e.text == "s" || e.text == "S") {
      set_screen(ScreenId::kSettings);
      return;
    }
  }
  switch (e.key) {
    case Key::kTab:
    case Key::kBackTab:
      main_panel_ = main_panel_ == 0 ? 1 : 0;
      return;
    case Key::kUp:
    case Key::kDown:
      if (main_panel_ == 1) {
        main_item_ = (main_item_ + (e.key == Key::kDown ? 1 : 3)) % 4;
        return;
      }
      break;  // in the text field, up/down move to no row at all
    case Key::kEnter:
      run_main_action(main_item_);
      return;
    case Key::kEscape:
      // Nothing is behind the first screen, so Esc is free to mean "leave the
      // text field": it is the way to reach the menu shortcuts without Tab.
      main_panel_ = 1;
      return;
    default:
      break;
  }
  if (main_panel_ == 0 && field_key(input_, e)) sync_output_path();
}

void App::run_main_action(int item) {
  switch (item) {
    case 0:
      // "Inspect ELF / DWARF" is the TUI's face of `stellar info`.
      set_screen(ScreenId::kInfo);
      if (!snap_.file.valid) {
        set_status("no analysis has read this file yet — run one, or use "
                   "`stellar info <elf>`");
      }
      break;
    case 1:
      // The core exposes no entry point for enumeration to the TUI, and a
      // screen that listed nothing would be a lie. Say where the real one is.
      set_status("browse compilation units: `stellar units <elf>` — the TUI has "
                 "no view for it");
      break;
    case 2:
      set_status("scan DWARF: `stellar scan <elf>` — the TUI has no view for it");
      break;
    default:
      set_screen(ScreenId::kEmit);
      break;
  }
}


void App::handle_emit_key(const Event& e) {
  const int count = static_cast<int>(EmitItem::kCount);

  // While a field has the caret it swallows everything except commit/cancel,
  // which is what makes typing a path that contains "r" or a space possible.
  if (editing_out_) {
    if (e.key == Key::kEnter) {
      editing_out_ = false;
      return;
    }
    if (e.key == Key::kEscape) {
      editing_out_ = false;
      return;
    }
    field_key(out_path_, e);
    return;
  }
  if (editing_max_lines_) {
    if (e.key == Key::kEnter) {
      commit_max_lines();
      return;
    }
    if (e.key == Key::kEscape) {
      editing_max_lines_ = false;
      return;
    }
    if (e.key == Key::kChar) {
      for (const char ch : e.text) {
        if (ch >= '0' && ch <= '9') max_lines_edit_.text.push_back(ch);
      }
      return;
    }
    if (e.key == Key::kBackspace) {
      if (!max_lines_edit_.text.empty()) max_lines_edit_.text.pop_back();
      return;
    }
    return;
  }

  if (e.key == Key::kChar) {
    if (e.text == "r" || e.text == "R") {
      start_analysis();
      return;
    }
    if (e.text == " ") {
      toggle_emit_item(emit_item_);
      return;
    }
  }
  switch (e.key) {
    case Key::kUp: emit_item_ = (emit_item_ + count - 1) % count; return;
    case Key::kDown: emit_item_ = (emit_item_ + 1) % count; return;
    case Key::kHome: emit_item_ = 0; return;
    case Key::kEnd: emit_item_ = count - 1; return;
    case Key::kEscape: set_screen(ScreenId::kMain); return;
    case Key::kEnter:
      switch (static_cast<EmitItem>(emit_item_)) {
        case EmitItem::kMethods: opt_methods_ = !opt_methods_; break;
        case EmitItem::kPadding: opt_pad_ = !opt_pad_; break;
        case EmitItem::kUnits:
          opt_units_ = !opt_units_;
          set_status("compilation-unit information is not applied — "
                     "Analysis::StartOptions has no field for it", 1);
          break;
        case EmitItem::kMaxLines: begin_edit_max_lines(); break;
        case EmitItem::kOutPath: editing_out_ = true; break;
        case EmitItem::kStart: start_analysis(); break;
        case EmitItem::kCount: break;
      }
      return;
    default: break;
  }
}

void App::handle_complete_key(const Event& e) {
  if (e.key == Key::kChar) {
    if (e.text == "q" || e.text == "Q") {
      quit_ = true;
      return;
    }
    if (e.text == "r" || e.text == "R") {
      start_analysis();
      return;
    }
  }
  switch (e.key) {
    case Key::kUp: complete_item_ = (complete_item_ + 2) % 3; return;
    case Key::kDown: complete_item_ = (complete_item_ + 1) % 3; return;
    case Key::kEscape: set_screen(ScreenId::kMain); return;
    case Key::kEnter:
      switch (complete_item_) {
        case 0:
          // There is no editor launcher and no clipboard facility in the TUI,
          // and shelling out would tear down the alternate screen. So this
          // reports exactly what was written and where, which is the part the
          // user cannot get from the screen itself.
          set_status("output: " + or_absent(!snap_.out_path.empty(), snap_.out_path) +
                         " (" + (snap_.out_bytes != 0 ? util::human_size(snap_.out_bytes)
                                                     : std::string("size unknown")) +
                         ") — open it outside Stellar", 1);
          break;
        case 1: set_screen(ScreenId::kMain); break;
        default: quit_ = true; break;
      }
      return;
    default: break;
  }
}


void App::handle_settings_key(const Event& e) {
  const int count = static_cast<int>(Setting::kCount);

  if (editing_interval_) {
    if (e.key == Key::kEnter) {
      commit_interval();
      return;
    }
    if (e.key == Key::kEscape) {
      editing_interval_ = false;
      return;
    }
    if (e.key == Key::kChar) {
      for (const char ch : e.text) {
        if (ch >= '0' && ch <= '9' && interval_edit_.text.size() < 4) {
          interval_edit_.text.push_back(ch);
        }
      }
      return;
    }
    if (e.key == Key::kBackspace && !interval_edit_.text.empty()) {
      interval_edit_.text.pop_back();
      return;
    }
    return;
  }
  if (editing_dir_) {
    if (e.key == Key::kEnter) {
      editing_dir_ = false;
      sync_output_path();
      set_status("output directory: " + or_absent(!out_dir_.text.empty(), out_dir_.text),
                 1);
      return;
    }
    if (e.key == Key::kEscape) {
      // A cancelled edit puts the value back exactly as it was.
      out_dir_.text = dir_saved_;
      editing_dir_ = false;
      return;
    }
    field_key(out_dir_, e);
    return;
  }

  const auto which = static_cast<Setting>(setting_);
  if (e.key == Key::kChar) {
    if (e.text == "s" || e.text == "S") {
      apply_settings();
      return;
    }
    if (e.text == " ") {
      switch (which) {
        case Setting::kAdaptive: adaptive_ = !adaptive_; break;
        case Setting::kParallel: parallel_ = !parallel_; break;
        case Setting::kProgressMode: cycle_progress_mode(1); break;
        default: return;
      }
      return;
    }
  }
  switch (e.key) {
    case Key::kUp: setting_ = (setting_ + count - 1) % count; return;
    case Key::kDown: setting_ = (setting_ + 1) % count; return;
    case Key::kHome: setting_ = 0; return;
    case Key::kEnd: setting_ = count - 1; return;
    case Key::kEscape: set_screen(ScreenId::kMain); return;
    case Key::kLeft:
    case Key::kRight: {
      const int dir = e.key == Key::kRight ? 1 : -1;
      if (which == Setting::kProgressMode) {
        cycle_progress_mode(dir);
      } else if (which == Setting::kInterval) {
        redraw_ms_ = std::clamp(redraw_ms_ + dir * 10, 10, 1000);
        set_status("redraw interval: " + std::to_string(redraw_ms_) + " ms", 1);
      }
      return;
    }
    case Key::kEnter:
      switch (which) {
        case Setting::kThreads:
          set_status("the core runs one worker thread; this limit is not applied",
                     2);
          break;
        case Setting::kRam:
          set_status("the process cannot be capped from the UI; this limit is not "
                     "applied", 2);
          break;
        case Setting::kAdaptive:
          adaptive_ = !adaptive_;
          set_status("adaptive resource usage is not applied — there is no "
                     "resource governor to adapt", 2);
          break;
        case Setting::kParallel:
          parallel_ = !parallel_;
          set_status("parallel analysis is not applied — the emitter is "
                     "single-threaded", 2);
          break;
        case Setting::kProgressMode: cycle_progress_mode(1); break;
        case Setting::kInterval: begin_edit_interval(); break;
        case Setting::kDir:
          dir_saved_ = out_dir_.text;
          editing_dir_ = true;
          break;
        case Setting::kCount: break;
      }
      return;
    default: break;
  }
}


// --- actions ----------------------------------------------------------------

void App::set_screen(ScreenId id) {
  if (screen_ == id) return;
  screen_ = id;
  // Focus belongs to the screen being entered, not to the one being left.
  if (id == ScreenId::kComplete) complete_item_ = 0;
  if (id == ScreenId::kEmit) {
    editing_out_ = false;
    editing_max_lines_ = false;
  }
  if (id == ScreenId::kSettings) {
    editing_interval_ = false;
    editing_dir_ = false;
  }
  // A screen switch can change the number of rows each field has; a full paint
  // is cheaper than reasoning about which cells that moved.
  force_full_ = true;
  dirty_ = true;
}

void App::set_status(std::string message, int kind) {
  status_ = std::move(message);
  status_kind_ = kind;
  dirty_ = true;
}

void App::sync_output_path() {
  // The output name is fixed; the directory is a setting, so the path is
  // re-derived whenever either the input or the directory changes.
  out_path_.text = output_path();
  out_path_.cursor = out_path_.text.size();
  out_path_.scroll = 0;
}

std::string App::output_path() const {
  std::string dir = out_dir_.text;
  if (dir.empty()) dir = "output/";
  if (dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
  return dir + "dump.cs";
}

void App::toggle_emit_item(int item) {
  switch (static_cast<EmitItem>(item)) {
    case EmitItem::kMethods: opt_methods_ = !opt_methods_; break;
    case EmitItem::kPadding: opt_pad_ = !opt_pad_; break;
    case EmitItem::kUnits:
      opt_units_ = !opt_units_;
      set_status("compilation-unit information is not applied — "
                 "Analysis::StartOptions has no field for it", 1);
      break;
    case EmitItem::kStart: start_analysis(); break;
    case EmitItem::kMaxLines:
    case EmitItem::kOutPath:
    case EmitItem::kCount: break;
  }
}

void App::begin_edit_max_lines() {
  editing_max_lines_ = true;
  max_lines_edit_.text = max_lines_ != 0 ? std::to_string(max_lines_) : std::string();
  max_lines_edit_.cursor = max_lines_edit_.text.size();
  max_lines_edit_.scroll = 0;
}

void App::commit_max_lines() {
  editing_max_lines_ = false;
  const std::string& t = max_lines_edit_.text;
  if (!t.empty()) {
    try {
      max_lines_ = std::stoull(t);
    } catch (const std::exception&) {
      max_lines_ = 0;  // unparseable: unlimited, and say so
    }
  } else {
    max_lines_ = 0;
  }
  set_status("maximum output lines: " +
                 (max_lines_ != 0 ? group(max_lines_) : std::string("unlimited")),
             1);
}

void App::begin_edit_interval() {
  editing_interval_ = true;
  interval_edit_.text = std::to_string(redraw_ms_);
  interval_edit_.cursor = interval_edit_.text.size();
  interval_edit_.scroll = 0;
}

void App::commit_interval() {
  editing_interval_ = false;
  int value = redraw_ms_;
  if (!interval_edit_.text.empty()) {
    try {
      value = std::stoi(interval_edit_.text);
    } catch (const std::exception&) {
      value = redraw_ms_;
    }
  }
  redraw_ms_ = std::clamp(value, 10, 1000);
  set_status("redraw interval: " + std::to_string(redraw_ms_) + " ms", 1);
}

void App::cycle_progress_mode(int delta) {
  progress_mode_ = (progress_mode_ + delta + 3) % 3;
  static constexpr std::string_view kNames[] = {"AUTO", "ALWAYS", "NEVER"};
  set_status("progress mode: " + std::string(kNames[progress_mode_]) +
                 (progress_mode_ == 0
                      ? " — repaint when the snapshot changes"
                      : progress_mode_ == 1 ? " — repaint every frame"
                                            : " — repaint only on a key press"),
             1);
}

void App::apply_settings() {
  // The three live settings already took effect as they were changed; Save
  // confirms that and states plainly that nothing is written to disk, because
  // Stellar has no configuration file to write them to.
  set_status("settings applied to this session (Stellar writes no config file)",
             1);
}


void App::start_analysis() {
  if (input_.text.empty()) {
    set_status("enter an input path first", 2);
    return;
  }
  if (analysis_.running()) {
    set_status("an analysis is already running", 2);
    return;
  }
  // Exactly the options `stellar emit` takes, and nothing invented: the mode
  // is "auto", so the core picks dwarfless for an input with no .debug_info
  // just as the CLI does.
  Analysis::StartOptions o;
  o.input_path = input_.text;
  o.out_path = out_path_.text.empty() ? output_path() : out_path_.text;
  o.target_name = basename_of(input_.text);
  o.pad_layout = opt_pad_;
  o.emit_methods = opt_methods_;
  o.max_lines = max_lines_;
  o.mode = "auto";
  if (!analysis_.start(o)) {
    set_status("the analysis could not be started", 2);
    return;
  }
  snap_ = analysis_.snapshot();
  watching_analysis_ = true;
  status_.clear();
  status_kind_ = 0;
  screen_ = ScreenId::kAnalysis;
  force_full_ = true;
  dirty_ = true;
}

int App::redraw_interval() const noexcept {
  // The loop's timeout is this value, and it is bounded on both sides: a dump
  // of a real binary runs for tens of seconds with no input, and a 0 ms poll
  // would spin a core for nothing.
  return std::clamp(redraw_ms_, 10, 1000);
}

void App::layout() {
  int cols = term_.width();
  int rows = term_.height();
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  cur_.resize(cols, rows);
  prev_.resize(cols, rows);
  // Rows the old grid had and the new one does not can only be cleared by
  // clearing the terminal, so a resize forces a full repaint.
  need_clear_ = true;
  force_full_ = true;
  dirty_ = true;
}

void App::draw() {
  if (progress_mode_ == 2 /*NEVER*/ && !dirty_) return;  // only repaint on a key
  if (progress_mode_ == 1 /*ALWAYS*/) force_full_ = true;  // repaint every frame

  const int cols = cur_.cols();
  const int rows = cur_.rows();
  if (cols < 1 || rows < 1) return;

  paint(cur_);
  // render_diff() is what makes an 80 ms refresh cheap: a progress tick touches
  // three rows, and only those are written. A terminal with no cursor movement
  // gets whole frames separated by newlines instead, because there is no way to
  // address a row in place.
  if (force_full_ || !term_.ansi()) {
    if (need_clear_ && term_.ansi()) term_.write(ansi::kClearScreen);
    term_.write(cur_.render(theme_));
    prev_ = cur_;
  } else {
    const std::string frame = cur_.render_diff(theme_, prev_);
    if (!frame.empty()) {
      term_.write(frame);
      prev_ = cur_;
    }
  }
  need_clear_ = false;
  force_full_ = false;
  dirty_ = false;
  term_.flush();
}


// --- the session ------------------------------------------------------------

int App::run(const Options& options) {
  theme_ = Theme::detect(options.no_color);
  if (!options.initial_path.empty()) {
    input_.text = options.initial_path;
    input_.cursor = input_.text.size();
    sync_output_path();
  }
  if (!term_.enter(theme_.ansi_enabled())) {
    // Refusing is the documented behaviour of Terminal::enter: a full-screen UI
    // with no cursor addressing, driven over a pipe, would eat the output it is
    // supposed to show. The CLI is the interface for that case.
    std::fputs("stellar: the interactive interface needs a terminal\n"
               "         use `stellar help` for the command line\n",
               stderr);
    return 1;
  }
  // The terminal knows what it can do; the theme's guess is only a starting
  // point, so the two are reconciled once, here.
  theme_.set_ansi_enabled(term_.ansi());

  layout();
  sync_view();
  draw();

  while (!quit_) {
    // One poll with a bounded timeout, always. The timeout is the redraw
    // interval, which is why a 30-second dump with nobody at the keyboard still
    // animates: no key means a timeout, and a timeout means a frame.
    if (watching_analysis_) {
      snap_ = analysis_.snapshot();
      if (!analysis_.running()) {
        watching_analysis_ = false;
        complete_item_ = 0;
        set_screen(ScreenId::kComplete);
        if (snap_.phase == AnalysisSnapshot::Phase::kFailed) exit_code_ = 1;
      }
    }
    if (term_.size_changed()) layout();
    sync_view();
    draw();

    Event e;
    if (term_.poll(e, redraw_interval())) handle_key(e);
  }

  // A run still in flight is asked to stop before the terminal is given back:
  // ~Analysis has to join its worker, and a cancel makes that wait short.
  if (analysis_.running()) analysis_.request_cancel();
  term_.leave();
  return exit_code_;
}

// --- test seams -------------------------------------------------------------
//
// These drive exactly the painting code run() uses. Nothing is duplicated for
// the test, which is the only reason a layout assertion is worth anything.

std::string App::render_frame_for_test(int cols, int rows,
                                       const AnalysisSnapshot& snap, ScreenId id) {
  snap_ = snap;
  const ScreenId saved = screen_;
  screen_ = id;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  cur_.resize(cols, rows);
  prev_.resize(cols, rows);
  sync_view();
  paint(cur_);
  screen_ = saved;
  return cur_.render(theme_);
}

void App::set_input_path_for_test(std::string path) {
  input_.text = std::move(path);
  input_.cursor = input_.text.size();
  input_.scroll = 0;
  sync_output_path();
}

void App::set_emit_options_for_test(bool methods, bool pad_layout, bool build_units) {
  opt_methods_ = methods;
  opt_pad_ = pad_layout;
  opt_units_ = build_units;
}

}  // namespace stellar::tui

