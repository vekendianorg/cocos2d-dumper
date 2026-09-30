// SPDX-License-Identifier: MIT
#include "stellar/tui/logo.h"

#include "stellar/tui/theme.h"

namespace stellar::tui {
namespace {

/// Decodes the codepoint at `i`; `len` receives its byte length.
std::uint32_t codepoint_at(std::string_view s, std::size_t i, std::size_t& len) noexcept {
  const auto b0 = static_cast<unsigned char>(s[i]);
  len = 1;
  if (b0 < 0x80) return b0;
  const auto cont = [&](std::size_t k) {
    return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
  };
  if ((b0 & 0xE0) == 0xC0 && cont(1)) {
    len = 2;
    return static_cast<std::uint32_t>(((b0 & 0x1Fu) << 6) |
                                      (static_cast<unsigned char>(s[i + 1]) & 0x3Fu));
  }
  if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    len = 3;
    return static_cast<std::uint32_t>(
        ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
        (static_cast<unsigned char>(s[i + 2]) & 0x3Fu));
  }
  if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    len = 4;
    return static_cast<std::uint32_t>(
        ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
        ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
        (static_cast<unsigned char>(s[i + 3]) & 0x3Fu));
  }
  return 0xFFFD;
}

/// True for the solid block glyphs (U+2588 FULL BLOCK and its neighbours).
///
/// These are the letter *masses* in the banner: the horizontal strokes of every
/// S, T, E, L, A and R. Treating them as the accent colour gives the mark its
/// two-tone treatment -- light-blue letterforms inside blue box-drawing
/// structure -- while leaving every character byte-identical.
bool is_letter_mass(std::uint32_t cp) noexcept {
  return (cp >= 0x2588 && cp <= 0x258F) ||   // full/left/right/lower blocks
         (cp >= 0x2590 && cp <= 0x2593) ||   // shade blocks
         (cp >= 0x2581 && cp <= 0x2587);     // one-eighth..seven-eighths
}

}  // namespace

bool logo_is_canonical() {
  // The artwork is transcribed from the design specification. These invariants
  // are what the tests assert, and what a careless edit to logo.h would break:
  // six rows, each starting and ending with a glyph (no stray leading or
  // trailing spaces), and no row wider than kLogoWidth.
  if (kLogoLines.size() != kLogoRows) return false;
  for (std::size_t i = 0; i < kLogoRows; ++i) {
    const std::string_view row = kLogoLines[i];
    if (row.empty()) return false;
    if (row.front() == ' ' || row.back() == ' ') return false;
    if (display_width(row) > kLogoWidth) return false;
  }
  // The banner as a whole must contain letter mass, otherwise the two-tone
  // treatment would collapse to a single colour and the mark would lose its
  // shape. This is deliberately a whole-banner check and not a per-row one: the
  // bottom row of the font is drawn entirely in double-line box glyphs and
  // legitimately has no solid blocks at all.
  bool any_mass = false;
  for (std::size_t i = 0; i < kLogoRows && !any_mass; ++i) {
    for (std::size_t j = 0; j < kLogoLines[i].size();) {
      std::size_t len = 1;
      if (is_letter_mass(codepoint_at(kLogoLines[i], j, len))) {
        any_mass = true;
        break;
      }
      j += len;
    }
  }
  return any_mass;
}

std::string render_logo(const Theme& theme) {
  std::string out;
  if (theme.color_enabled()) {
    // Two-tone: structure in Stellar blue, letter mass in light blue. Both are
    // bold, per the spec; the mark must never render as plain white when the
    // terminal can do colour, and must never use purple.
    out += theme.paint(Style::kLogo);
  }
  for (std::size_t i = 0; i < kLogoRows; ++i) {
    const std::string_view row = kLogoLines[i];
    // The reset has to land *before* the newline, or the terminal carries the
    // colour into the next row's first cell. It is emitted only when colour is
    // actually on: in no-colour mode not a single escape sequence may reach the
    // terminal, and that is a hard requirement of the spec, not a nicety.
    if (i != 0) out += theme.color_enabled() ? "\033[0m\n" : "\n";
    for (std::size_t j = 0; j < row.size();) {
      std::size_t len = 1;
      const std::uint32_t cp = codepoint_at(row, j, len);
      if (theme.color_enabled()) {
        out += is_letter_mass(cp) ? theme.paint(Style::kAccent) : theme.paint(Style::kLogo);
      }
      out.append(row.substr(j, len));
      j += len;
    }
  }
  if (theme.color_enabled()) out += theme.reset();
  return out;
}

}  // namespace stellar::tui
