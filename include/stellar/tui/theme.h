// SPDX-License-Identifier: MIT
// The single source of truth for Stellar's colour and text-style vocabulary.
//
// Every widget asks the theme for a *semantic* style (TITLE, LABEL, KEY, ...)
// and the theme decides how that looks on the terminal it is talking to. No
// widget is allowed to emit a raw escape sequence: that is what keeps the
// palette consistent and makes "no colour" a single switch instead of a search
// through every draw call.
//
// Stellar's identity is blue + light blue + white. Purple is deliberately not
// in the palette at all, so it cannot creep in as an "accent".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace stellar::tui {

/// What the attached terminal can actually render.
enum class ColorDepth {
  kNone,       ///< no colour: never emit an escape sequence
  kBasic,      ///< the original 8/16 ANSI colours
  kAnsi256,    ///< the xterm 256-colour cube
  kTrueColor,  ///< direct 24-bit RGB
};

/// Foreground colours in the Stellar palette. The names are semantic, not
/// numeric, so the theme can remap them per colour depth.
enum class Color : std::uint8_t {
  kDefault,
  kBlue,        ///< primary branding: titles, active elements, focus
  kLightBlue,   ///< technical accents: values, shortcuts, progress
  kWhite,       ///< primary readable content
  kGray,        ///< secondary labels and metadata
  kDarkGray,    ///< borders, separators, inactive progress
  kGreen,       ///< success only
  kYellow,      ///< warning only
  kRed,         ///< error only
};

/// Text attributes. Regular is the default and is not listed, because "no
/// attribute" is how you spell it.
enum class Attr : std::uint8_t {
  kRegular = 0,   ///< the default: no attribute set
  kBold = 1u << 0,
  kDim = 1u << 1,
  kItalic = 1u << 2,
  kUnderline = 1u << 3,
  kReverse = 1u << 4,
};

[[nodiscard]] constexpr Attr operator|(Attr a, Attr b) noexcept {
  return static_cast<Attr>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
[[nodiscard]] constexpr bool has_attr(Attr set, Attr one) noexcept {
  return (static_cast<unsigned>(set) & static_cast<unsigned>(one)) != 0;
}


/// The semantic styles named in the design spec. Widgets speak this vocabulary
/// and never name a colour directly.
enum class Style : std::uint8_t {
  kLogo,               ///< Blue + Bold
  kTitle,              ///< Blue + Bold
  kSection,            ///< Blue + Bold
  kHeader,             ///< Light Blue + Bold
  kLabel,              ///< Gray + Regular
  kValue,              ///< White + Regular
  kSelected,           ///< White on Blue background + Bold
  kFocused,            ///< Light Blue + Bold
  kAccent,             ///< Light Blue + Bold
  kSuccess,            ///< Green + Bold
  kWarning,            ///< Yellow + Bold
  kError,              ///< Red + Bold
  kMuted,              ///< Gray + Dim
  kDim,                ///< Dim + Regular
  kKey,                ///< Light Blue + Bold
  kKeyDescription,     ///< Gray + Regular
  kPath,               ///< Light Blue + Regular
  kNumber,             ///< Light Blue + Regular
  kProgressFilled,     ///< Blue
  kProgressEmpty,      ///< Dark Gray
  kBorder,             ///< Dark Gray
  kActiveBorder,       ///< Blue
  kCurrentOperation,   ///< White + Bold
  kCompletedOperation, ///< Light Blue + Regular
  kCheckboxOn,         ///< Light Blue + Bold  ([x])
  kCheckboxOff,        ///< Gray + Regular      ([ ])
  kButton,             ///< White + Regular
  kButtonSelected,     ///< Bold light blue, drawn between ▶ ◀ markers (no background)
  kCaret,              ///< White on Blue, ONE cell: the text cursor, the only background in the UI
};

/// Background colours a style can request. Only the selection highlight uses
/// one; everything else paints on the terminal's own background.
enum class Bg : std::uint8_t { kNone, kBlue };

/// Resolves styles to escape sequences for one terminal.
class Theme {
 public:
  /// Probes the terminal. Honours NO_COLOR and `TERM=dumb`.
  /// `force_no_color` is the `--no-color` flag.
  static Theme detect(bool force_no_color);

  /// Explicit construction, for the tests.
  Theme() = default;
  static Theme for_depth(ColorDepth depth) noexcept;

  [[nodiscard]] ColorDepth depth() const noexcept { return depth_; }
  [[nodiscard]] bool color_enabled() const noexcept { return depth_ != ColorDepth::kNone; }

  /// True when the terminal should be trusted with cursor-movement and
  /// alternate-screen sequences.
  [[nodiscard]] bool ansi_enabled() const noexcept { return ansi_; }
  void set_ansi_enabled(bool on) noexcept { ansi_ = on; }

  /// The escape sequence that selects `style`, or "" when colour is off.
  [[nodiscard]] std::string_view paint(Style style) const;

  /// Resets to terminal defaults (SGR 0).
  [[nodiscard]] std::string_view reset() const;

 private:
  /// Builds the SGR parameter list for a style.
  void append_sgr(std::string& out, Style style) const;

  ColorDepth depth_ = ColorDepth::kNone;
  bool ansi_ = false;
};

/// True when the environment asks for no colour (NO_COLOR set and non-empty).
/// https://no-color.org
[[nodiscard]] bool env_requests_no_color();

/// The colour a style resolves to, without needing a Theme. Used by the logo
/// renderer, which colours individual glyphs.
[[nodiscard]] Color color_of(Style style) noexcept;

/// Maps an RGB triple onto the given depth. Exposed so the palette can be
/// tested without a terminal: the same input must produce the same output
/// every time, and the 256/basic mappings are pure functions.
[[nodiscard]] int rgb_to_depth(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                               ColorDepth depth) noexcept;

/// The canonical Stellar palette as 24-bit RGB. Single source of truth: the
/// theme derives its basic and 256-colour approximations from these values, so
/// "blue" is the same blue at every depth.
struct Rgb {
  std::uint8_t r, g, b;
};
[[nodiscard]] Rgb rgb_of(Color color) noexcept;

/// Width of a UTF-8 string in terminal columns, counting double-width
/// codepoints as two. The banner and the box-drawing glyphs are all in this
/// range, so layout maths has to agree with the terminal.
[[nodiscard]] std::size_t display_width(std::string_view s) noexcept;

/// Truncates to `cols` display columns, preserving valid UTF-8 (never splits a
/// codepoint). No ellipsis is added; the caller decides on that.
[[nodiscard]] std::string truncate_to_width(std::string_view s, std::size_t cols);

}  // namespace stellar::tui

