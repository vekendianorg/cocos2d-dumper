// SPDX-License-Identifier: MIT
// The frame buffer every Stellar widget draws into.
//
// A Screen is a rows x cols grid of cells. A cell holds exactly one codepoint
// (plus the semantic style it should be painted with) and nothing else: no
// escape sequences, no colour names, no cursor bookkeeping. Widgets write cells,
// the Screen turns cells into a terminal frame. That split is the whole point —
// a widget can never leak a raw escape sequence, and re-rendering a frame costs
// one pass over the grid.
//
// Two ways out:
//
//   render()        the whole screen, one line per row.
//   render_diff()   only the rows that changed since another Screen.
//
// render_diff() is what makes a 24-row interface cheap to redraw at interactive
// rates. A progress tick usually touches three cells; without a diff the whole
// screen would be rewritten (and repainted with colour) thirty times a second.
//
// Every entry point is bounds-safe by construction. A terminal can be resized
// between the size query and the paint, so a widget may legitimately ask for a
// row that no longer exists; anything outside [0,rows) x [0,cols) is a silent
// no-op, never a crash and never a partial write.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "stellar/tui/theme.h"

namespace stellar::tui {

/// One grid cell.
///
/// An empty `text` is either a blank cell or the right-hand half of a
/// double-width codepoint; both render as "emit nothing", because the terminal
/// has already advanced past them.
struct ScreenCell {
  std::string text = " ";
  Style style = Style::kValue;
};

class Screen {
 public:
  Screen() = default;

  /// Resizes the grid and blanks it. Contents are discarded, so a resize is
  /// also a clear. Non-positive dimensions mean an empty screen, which every
  /// method tolerates.
  void resize(int cols, int rows);
  /// Blanks every cell without changing the size.
  void clear();

  [[nodiscard]] int cols() const noexcept { return cols_; }
  [[nodiscard]] int rows() const noexcept { return rows_; }
  /// True when there are no cells at all.
  [[nodiscard]] bool empty() const noexcept { return cells_.empty(); }

  // --- painting -------------------------------------------------------------
  //
  // All coordinates are 0-based, all rectangles are (row, col, w, h), and all
  // of them are clipped rather than rejected.

  /// Writes `text` at (row, col), advancing by display width. Stops at the
  /// right edge without ever splitting a codepoint. Zero-width codepoints
  /// (combining marks) attach to the cell before them.
  void put(int row, int col, std::string_view text, Style st = Style::kValue);

  /// Paints a `w` x `h` block of spaces, erasing whatever was underneath.
  void fill(int row, int col, int w, int h, Style st);

  /// `fill()` plus text in it, truncated to `w` and padded with spaces to `w`.
  void fill_text(int row, int col, int w, std::string_view text,
                 Style st = Style::kValue);

  /// Text clipped to `max_cols` columns. Cheaper than put() when the caller
  /// already knows the field width.
  void text_clipped(int row, int col, int max_cols, std::string_view text,
                    Style st = Style::kValue);

  /// Horizontal rule of `len` box-drawing cells (U+2500).
  void hline(int row, int col, int len, Style st = Style::kBorder);
  /// Vertical rule of `len` box-drawing cells (U+2502).
  void vline(int col, int row, int len, Style st = Style::kBorder);

  /// A single-line rectangular frame, optionally titled in the top border.
  ///
  /// The title starts one column in from the corner (one column of padding
  /// after the border cell) and is truncated so it cannot reach the far
  /// corner. Needs at least 2x2 cells; anything smaller is a no-op.
  void box(int row, int col, int w, int h, Style border,
           std::string_view title = {}, Style title_style = Style::kTitle);

  /// A progress bar `width_cells` wide. `fraction` is clamped to [0,1]; NaN and
  /// infinities count as 0. `glyph` is used for both halves of the bar — pass
  /// an empty string for the default solid/light block pair.
  void progress_bar(int row, int col, int width_cells, double fraction,
                    Style filled = Style::kProgressFilled,
                    Style empty = Style::kProgressEmpty,
                    std::string_view glyph = {});

  /// Records whether the caller wants the hardware cursor shown. The Screen
  /// only remembers it: cursor visibility is terminal state, and the emitter
  /// (terminal.h) owns it.
  void set_cursor_visible(bool on) noexcept { cursor_visible_ = on; }
  [[nodiscard]] bool cursor_visible() const noexcept { return cursor_visible_; }

  // --- inspection (used by tests and by the diff) ---------------------------

  /// The text of one cell: " " for a blank, "" for the tail of a wide glyph,
  /// " " when out of bounds (so callers can format it unconditionally).
  [[nodiscard]] std::string_view text_at(int row, int col) const;
  /// The style of one cell, or Style::kValue when out of bounds.
  [[nodiscard]] Style style_at(int row, int col) const;
  /// True when the cell holds exactly `text` in exactly `st`.
  [[nodiscard]] bool cell_matches(int row, int col, std::string_view text,
                                  Style st) const;
  /// True when both grids are the same size with identical contents.
  [[nodiscard]] bool same_as(const Screen& other) const;

  // --- emitting -------------------------------------------------------------

  /// The whole screen as one string: `rows` lines joined by CR LF, each padded
  /// to `cols` columns so a stale line can never survive underneath. Styling
  /// comes exclusively from Theme::paint(); a no-colour theme produces output
  /// containing no escape byte at all.
  [[nodiscard]] std::string render(const Theme& theme) const;

  /// Only the rows that differ from `prev`, each preceded by a cursor move and
  /// followed by an erase-to-end-of-line so a shorter row cannot leave debris.
  /// Returns "" when nothing changed.
  ///
  /// A grid whose size differs from `prev` has every row treated as changed.
  /// Rows that exist only in `prev` cannot be rewritten from here; the caller
  /// should do a full render() after a resize.
  ///
  /// This always emits cursor movement, so it is only meaningful when the
  /// terminal has ANSI enabled.
  [[nodiscard]] std::string render_diff(const Theme& theme,
                                        const Screen& prev) const;

 private:
  /// Shared row renderer: cells to a styled line, padded to `cols` columns.
  [[nodiscard]] std::string render_row(int row, const Theme& theme) const;
  /// True when row `row` of both grids is identical (sizes already checked).
  [[nodiscard]] bool row_equal(int row, const Screen& other) const;

  /// Bounds-checked cell access. Returns nullptr outside the grid, which is
  /// what makes every painter above safe without a test of its own.
  [[nodiscard]] ScreenCell* at(int row, int col) noexcept;
  [[nodiscard]] const ScreenCell* at(int row, int col) const noexcept;

  /// Writes one cell, blanking the orphaned half of a double-width glyph that
  /// used to live here. An empty `text` writes a continuation cell.
  void set_cell(int row, int col, std::string text, Style st);
  /// Writes a glyph of arbitrary width, claiming the cells it covers.
  void put_glyph(int row, int col, std::string_view glyph, Style st);

  std::vector<ScreenCell> cells_;
  int cols_ = 0;
  int rows_ = 0;
  bool cursor_visible_ = false;
};

}  // namespace stellar::tui
