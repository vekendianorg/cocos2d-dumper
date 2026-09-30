// SPDX-License-Identifier: MIT
#include "stellar/tui/screen.h"

#include <cmath>
#include <utility>

#include "stellar/tui/terminal.h"

namespace stellar::tui {
namespace {

// --- the box-drawing alphabet -------------------------------------------------
//
// The frame glyphs named in the design spec. They are all narrow (one column),
// which is what keeps the layout arithmetic in display_width() honest.
constexpr std::string_view kTopLeft = "\xe2\x94\x8c";      // U+250C
constexpr std::string_view kTopRight = "\xe2\x94\x90";     // U+2510
constexpr std::string_view kBottomLeft = "\xe2\x94\x94";   // U+2514
constexpr std::string_view kBottomRight = "\xe2\x94\x98";  // U+2518
constexpr std::string_view kHorizontal = "\xe2\x94\x80";   // U+2500
constexpr std::string_view kVertical = "\xe2\x94\x82";     // U+2502

// Default progress glyphs: solid block for the filled part, light shade for the
// remainder. Together they read as one bar at any terminal that has them.
constexpr std::string_view kBlockFull = "\xe2\x96\x88";    // U+2588
constexpr std::string_view kBlockLight = "\xe2\x96\x91";   // U+2591

constexpr std::string_view kBlank = " ";

// The one raw control sequence this file is allowed to emit. It is a low-level
// emitter concern (there is no semantic style for "erase"), not a widget
// choice: every colour and attribute still goes through Theme::paint().
constexpr std::string_view kEraseToEndOfLine = "\033[K";

/// Byte length of the codepoint starting at `i`, never running past the end and
/// never claiming a continuation byte that is not one. Malformed input yields 1,
/// so the walk in put() always terminates.
std::size_t codepoint_len(std::string_view s, std::size_t i) noexcept {
  const auto b0 = static_cast<unsigned char>(s[i]);
  std::size_t n = 1;
  if ((b0 & 0xE0u) == 0xC0u) {
    n = 2;
  } else if ((b0 & 0xF0u) == 0xE0u) {
    n = 3;
  } else if ((b0 & 0xF8u) == 0xF0u) {
    n = 4;
  }
  if (i + n > s.size()) return 1;
  for (std::size_t k = 1; k < n; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) return 1;
  }
  return n;
}

/// Guards the style enum. A widget that passes a garbage value gets plain text
/// rather than an out-of-range palette lookup (theme.paint() would return "",
/// but silently losing colour is worse than falling back to white).
Style sanitize(Style st) noexcept {
  const auto i = static_cast<unsigned>(st);
  return i <= static_cast<unsigned>(Style::kButtonSelected) ? st
                                                           : Style::kValue;
}

}  // namespace

void Screen::resize(int cols, int rows) {
  cols_ = cols > 0 ? cols : 0;
  rows_ = rows > 0 ? rows : 0;
  cells_.assign(static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_),
                ScreenCell{});
}

void Screen::clear() {
  cells_.assign(cells_.size(), ScreenCell{});
}

ScreenCell* Screen::at(int row, int col) noexcept {
  if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return nullptr;
  return &cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols_) +
                 static_cast<std::size_t>(col)];
}

const ScreenCell* Screen::at(int row, int col) const noexcept {
  if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return nullptr;
  return &cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols_) +
                 static_cast<std::size_t>(col)];
}

void Screen::set_cell(int row, int col, std::string text, Style st) {
  ScreenCell* c = at(row, col);
  if (c == nullptr) return;
  // A double-width glyph owns the cell to its right. Overwriting one half of
  // such a pair has to blank the other half, or the terminal keeps showing a
  // stale column and the row drifts.
  if (!text.empty() && !c->text.empty() && display_width(c->text) == 2) {
    if (ScreenCell* tail = at(row, col + 1); tail != nullptr) {
      tail->text = kBlank;
      tail->style = c->style;
    }
  }
  c->text = std::move(text);
  c->style = st;
}

void Screen::put_glyph(int row, int col, std::string_view glyph, Style st) {
  const std::size_t w = display_width(glyph);
  if (w == 0) return;
  // A glyph that would straddle the right edge is dropped whole: a wrapped
  // double-width character desynchronises every column after it.
  if (col < 0 || col + static_cast<int>(w) > cols_) return;
  set_cell(row, col, std::string(glyph), st);
  for (std::size_t k = 1; k < w; ++k) {
    if (ScreenCell* tail = at(row, col + static_cast<int>(k)); tail != nullptr) {
      // The right-hand half of a wide glyph holds no bytes of its own; the
      // terminal has already advanced past it.
      tail->text.clear();
      tail->style = st;
    }
  }
}

void Screen::put(int row, int col, std::string_view text, Style st) {
  if (text.empty()) return;
  if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
  const Style style = sanitize(st);
  int x = col;
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t len = codepoint_len(text, i);
    const std::string_view cp = text.substr(i, len);
    const int w = static_cast<int>(display_width(cp));
    if (w == 0) {
      // A combining mark or a stray control byte. Appending it to the cell on
      // the left is what makes accents work without a second pass.
      if (x > 0) {
        if (ScreenCell* prev = at(row, x - 1);
            prev != nullptr && !prev->text.empty()) {
          prev->text.append(cp);
        }
      }
      i += len;
      continue;
    }
    if (x + w > cols_) break;  // right edge: stop, never split a codepoint
    put_glyph(row, x, cp, style);
    x += w;
    i += len;
  }
}

void Screen::fill(int row, int col, int w, int h, Style st) {
  if (w <= 0 || h <= 0) return;
  const Style style = sanitize(st);
  for (int r = row; r < row + h; ++r) {
    if (r < 0) continue;
    if (r >= rows_) break;
    for (int c = col; c < col + w; ++c) {
      set_cell(r, c, std::string(kBlank), style);
    }
  }
}

void Screen::fill_text(int row, int col, int w, std::string_view text,
                       Style st) {
  if (w <= 0) return;
  // Blank first, then draw: the padding then has the field's own style, and the
  // text is clipped to the field rather than to the screen.
  fill(row, col, w, 1, st);
  put(row, col, truncate_to_width(text, static_cast<std::size_t>(w)), st);
}

void Screen::text_clipped(int row, int col, int max_cols, std::string_view text,
                          Style st) {
  if (max_cols <= 0) return;
  put(row, col, truncate_to_width(text, static_cast<std::size_t>(max_cols)), st);
}

void Screen::hline(int row, int col, int len, Style st) {
  if (len <= 0) return;
  for (int i = 0; i < len; ++i) {
    put_glyph(row, col + i, kHorizontal, sanitize(st));
  }
}

void Screen::vline(int col, int row, int len, Style st) {
  if (len <= 0) return;
  for (int i = 0; i < len; ++i) {
    put_glyph(row + i, col, kVertical, sanitize(st));
  }
}

void Screen::box(int row, int col, int w, int h, Style border,
                 std::string_view title, Style title_style) {
  // A frame needs its four corners; anything smaller would draw a shape that is
  // not a box, so it is refused rather than drawn crooked.
  if (w < 2 || h < 2) return;
  const Style b = sanitize(border);
  const int right = col + w - 1;
  const int bottom = row + h - 1;

  put_glyph(row, col, kTopLeft, b);
  put_glyph(row, right, kTopRight, b);
  put_glyph(bottom, col, kBottomLeft, b);
  put_glyph(bottom, right, kBottomRight, b);
  hline(row, col + 1, w - 2, b);
  hline(bottom, col + 1, w - 2, b);
  for (int r = row + 1; r < bottom; ++r) {
    put_glyph(r, col, kVertical, b);
    put_glyph(r, right, kVertical, b);
  }

  if (title.empty()) return;
  // One column of padding after the corner, and the far corner left intact:
  // the title gets w - 3 columns, or nothing at all in a very narrow frame.
  const int room = w - 3;
  if (room <= 0) return;
  put(row, col + 2, truncate_to_width(title, static_cast<std::size_t>(room)),
      sanitize(title_style));
}

void Screen::progress_bar(int row, int col, int width_cells, double fraction,
                          Style filled, Style empty, std::string_view glyph) {
  if (width_cells <= 0) return;
  if (!std::isfinite(fraction)) fraction = 0.0;
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;

  const std::string_view full = glyph.empty() ? kBlockFull : glyph;
  const std::string_view rest = glyph.empty() ? kBlockLight : glyph;
  int unit = static_cast<int>(display_width(full));
  if (unit <= 0) unit = 1;

  // Whole glyphs only, with any leftover columns padded: a bar that ends half a
  // cell short of the requested width is fine, one that overflows is not.
  const int units = width_cells / unit;
  int done = static_cast<int>(std::lround(fraction * static_cast<double>(units)));
  if (done < 0) done = 0;
  if (done > units) done = units;

  for (int i = 0; i < units; ++i) {
    put_glyph(row, col + i * unit, i < done ? full : rest,
              i < done ? sanitize(filled) : sanitize(empty));
  }
  for (int i = units * unit; i < width_cells; ++i) {
    set_cell(row, col + i, std::string(kBlank), sanitize(empty));
  }
}

std::string_view Screen::text_at(int row, int col) const {
  const ScreenCell* c = at(row, col);
  return c != nullptr ? std::string_view(c->text) : kBlank;
}

Style Screen::style_at(int row, int col) const {
  const ScreenCell* c = at(row, col);
  return c != nullptr ? c->style : Style::kValue;
}

bool Screen::cell_matches(int row, int col, std::string_view text,
                          Style st) const {
  const ScreenCell* c = at(row, col);
  if (c == nullptr) return false;
  return std::string_view(c->text) == text && c->style == sanitize(st);
}

bool Screen::row_equal(int row, const Screen& other) const {
  if (row < 0 || row >= rows_ || row >= other.rows_) return false;
  if (cols_ != other.cols_) return false;
  for (int col = 0; col < cols_; ++col) {
    const ScreenCell* a = at(row, col);
    const ScreenCell* b = other.at(row, col);
    if (a == nullptr || b == nullptr) return false;
    if (a->text != b->text || a->style != b->style) return false;
  }
  return true;
}

bool Screen::same_as(const Screen& other) const {
  if (cols_ != other.cols_ || rows_ != other.rows_) return false;
  for (int row = 0; row < rows_; ++row) {
    if (!row_equal(row, other)) return false;
  }
  return true;
}

std::string Screen::render_row(int row, const Theme& theme) const {
  std::string out;
  if (row < 0 || row >= rows_) return out;
  out.reserve(static_cast<std::size_t>(cols_) * 2 + 16);

  Style current = Style::kValue;
  bool styled = false;   // a style has been selected for this row
  bool painted = false;  // ...and it actually emitted bytes
  int used = 0;

  for (int col = 0; col < cols_; ++col) {
    const ScreenCell* c = at(row, col);
    // An empty cell is a blank or the tail of a wide glyph: either way the
    // terminal has already moved past this column, so nothing is emitted.
    if (c == nullptr || c->text.empty()) continue;
    const int w = static_cast<int>(display_width(c->text));
    if (w <= 0) continue;
    if (used + w > cols_) break;  // defensive: a row must never wrap
    if (!styled || c->style != current) {
      const std::string_view esc = theme.paint(c->style);
      out.append(esc);
      if (!esc.empty()) painted = true;
      current = c->style;
      styled = true;
    }
    out.append(c->text);
    used += w;
  }

  // Close the style before padding, so the pad spaces sit on the terminal's own
  // background instead of extending a selection highlight to the right edge.
  if (painted) out.append(theme.reset());
  // Every row is padded to its full width: a blank row still has to overwrite
  // whatever the terminal left there from the previous frame.
  out.append(static_cast<std::size_t>(cols_ - used), ' ');
  return out;
}

std::string Screen::render(const Theme& theme) const {
  std::string out;
  out.reserve(static_cast<std::size_t>(rows_) *
                  (static_cast<std::size_t>(cols_) * 2 + 8) +
              16);
  for (int row = 0; row < rows_; ++row) {
    if (row != 0) out.append("\r\n");
    out.append(render_row(row, theme));
  }
  return out;
}

std::string Screen::render_diff(const Theme& theme, const Screen& prev) const {
  // After a resize no row can be assumed to match, so the whole grid is treated
  // as changed. Rows that only exist in `prev` are the caller's problem: only a
  // full render() can clear territory this grid does not own.
  const bool resized = (prev.cols_ != cols_ || prev.rows_ != rows_);
  std::string out;
  for (int row = 0; row < rows_; ++row) {
    if (!resized && row_equal(row, prev)) continue;
    out.append(ansi::cursor_to(row + 1, 1));
    out.append(render_row(row, theme));
    // The row may now be shorter than it was, so clear the remainder rather
    // than trusting the pad to cover it.
    out.append(kEraseToEndOfLine);
  }
  return out;
}

}  // namespace stellar::tui
