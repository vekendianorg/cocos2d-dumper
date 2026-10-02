// SPDX-License-Identifier: MIT
#include "stellar/tui/theme.h"

#include <array>
#include <cstdlib>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#define STELLAR_ISATTY _isatty
#define STELLAR_FILENO _fileno
#endif

#if !defined(_WIN32)
#define STELLAR_ISATTY isatty
#define STELLAR_FILENO fileno
#endif

namespace stellar::tui {
namespace {

// --- the palette -----------------------------------------------------------
//
// Blue carries the structure (titles, focus, active borders) and light blue
// carries the data (values, shortcuts, progress). Keeping the two clearly
// distinct is what makes the interface read as "deep blue structure, light-blue
// technical accents" rather than as a monochrome terminal app.
constexpr Rgb kBlueRgb{0x3b, 0x82, 0xf6};       // #3B82F6 primary blue
constexpr Rgb kLightBlueRgb{0x7d, 0xd3, 0xfc};  // #7DD3FC light blue/cyan
constexpr Rgb kWhiteRgb{0xf8, 0xfa, 0xfc};      // #F8FAFC near-white
constexpr Rgb kGrayRgb{0x94, 0xa3, 0xb8};       // #94A3B8 secondary text
constexpr Rgb kDarkGrayRgb{0x47, 0x55, 0x69};   // #475569 borders/structure
constexpr Rgb kGreenRgb{0x4a, 0xd9, 0x77};      // success only
constexpr Rgb kYellowRgb{0xf5, 0xc2, 0x42};     // warning only
constexpr Rgb kRedRgb{0xf8, 0x71, 0x71};        // error only

/// How one semantic style looks. This table IS the design spec's style table,
/// transcribed once; widgets never restate it.
struct StyleSpec {
  Color fg;
  Bg bg;
  Attr attr;
};

constexpr Attr R = Attr::kRegular;

constexpr StyleSpec kSpec[] = {
    /* kLogo               */ {Color::kBlue, Bg::kNone, Attr::kBold},
    /* kTitle              */ {Color::kBlue, Bg::kNone, Attr::kBold},
    /* kSection            */ {Color::kBlue, Bg::kNone, Attr::kBold},
    /* kHeader             */ {Color::kLightBlue, Bg::kNone, Attr::kBold},
    /* kLabel              */ {Color::kGray, Bg::kNone, R},
    /* kValue              */ {Color::kWhite, Bg::kNone, R},
    /* kSelected           */ {Color::kWhite, Bg::kBlue, Attr::kBold},
    /* kFocused            */ {Color::kLightBlue, Bg::kNone, Attr::kBold},
    /* kAccent             */ {Color::kLightBlue, Bg::kNone, Attr::kBold},
    /* kSuccess            */ {Color::kGreen, Bg::kNone, Attr::kBold},
    /* kWarning            */ {Color::kYellow, Bg::kNone, Attr::kBold},
    /* kError              */ {Color::kRed, Bg::kNone, Attr::kBold},
    /* kMuted              */ {Color::kGray, Bg::kNone, Attr::kDim},
    /* kDim                */ {Color::kDefault, Bg::kNone, Attr::kDim},
    /* kKey                */ {Color::kLightBlue, Bg::kNone, Attr::kBold},
    /* kKeyDescription     */ {Color::kGray, Bg::kNone, R},
    /* kPath               */ {Color::kLightBlue, Bg::kNone, R},
    /* kNumber             */ {Color::kLightBlue, Bg::kNone, R},
    /* kProgressFilled     */ {Color::kBlue, Bg::kNone, R},
    /* kProgressEmpty      */ {Color::kDarkGray, Bg::kNone, R},
    /* kBorder             */ {Color::kDarkGray, Bg::kNone, R},
    /* kActiveBorder       */ {Color::kBlue, Bg::kNone, R},
    /* kCurrentOperation   */ {Color::kWhite, Bg::kNone, Attr::kBold},
    /* kCompletedOperation */ {Color::kLightBlue, Bg::kNone, R},
    /* kCheckboxOn         */ {Color::kLightBlue, Bg::kNone, Attr::kBold},
    /* kCheckboxOff        */ {Color::kGray, Bg::kNone, R},
    /* kButton             */ {Color::kWhite, Bg::kNone, R},
    /* kButtonSelected     */ {Color::kWhite, Bg::kBlue, Attr::kBold},
};

constexpr std::size_t kSpecCount = sizeof(kSpec) / sizeof(kSpec[0]);

static_assert(kSpecCount == static_cast<std::size_t>(Style::kButtonSelected) + 1,
              "kSpec must cover every Style value, in order");


/// The 16 standard ANSI colours, so kBasic can pick the nearest one.
constexpr Rgb kBasic[16] = {
    {0x00, 0x00, 0x00}, {0xaa, 0x00, 0x00}, {0x00, 0xaa, 0x00}, {0xaa, 0x55, 0x00},
    {0x00, 0x00, 0xaa}, {0xaa, 0x00, 0xaa}, {0x00, 0xaa, 0xaa}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0xff, 0x55, 0x55}, {0x55, 0xff, 0x55}, {0xff, 0xff, 0x55},
    {0x55, 0x55, 0xff}, {0xff, 0x55, 0xff}, {0x55, 0xff, 0xff}, {0xff, 0xff, 0xff},
};

/// xterm's 256-colour palette: 16 system colours, a 6x6x6 RGB cube at 16..231,
/// then a 24-step grey ramp at 232..255.
std::vector<Rgb> build_xterm256() {
  std::vector<Rgb> v;
  v.reserve(256);
  for (int i = 0; i < 16; ++i) v.push_back(kBasic[i]);
  static constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};
  for (int r = 0; r < 6; ++r) {
    for (int g = 0; g < 6; ++g) {
      for (int b = 0; b < 6; ++b) {
        v.push_back(Rgb{static_cast<std::uint8_t>(kLevels[r]),
                        static_cast<std::uint8_t>(kLevels[g]),
                        static_cast<std::uint8_t>(kLevels[b])});
      }
    }
  }
  for (int i = 0; i < 24; ++i) {
    const auto v8 = static_cast<std::uint8_t>(8 + i * 10);
    v.push_back(Rgb{v8, v8, v8});
  }
  return v;
}

const std::vector<Rgb>& xterm256() {
  static const std::vector<Rgb> table = build_xterm256();
  return table;
}

int squared_distance(Rgb a, Rgb b) {
  const int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
  const int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
  const int db = static_cast<int>(a.b) - static_cast<int>(b.b);
  // Weighted towards green, which the eye resolves best. A plain Euclidean
  // distance picks a muddy grey over a clean blue far too often.
  return dr * dr * 3 + dg * dg * 6 + db * db;
}

}  // namespace

Rgb rgb_of(Color color) noexcept {
  switch (color) {
    case Color::kBlue: return kBlueRgb;
    case Color::kLightBlue: return kLightBlueRgb;
    case Color::kWhite: return kWhiteRgb;
    case Color::kGray: return kGrayRgb;
    case Color::kDarkGray: return kDarkGrayRgb;
    case Color::kGreen: return kGreenRgb;
    case Color::kYellow: return kYellowRgb;
    case Color::kRed: return kRedRgb;
    case Color::kDefault: break;
  }
  return Rgb{0xff, 0xff, 0xff};
}

int rgb_to_depth(std::uint8_t r, std::uint8_t g, std::uint8_t b, ColorDepth depth) noexcept {
  const Rgb want{r, g, b};
  if (depth == ColorDepth::kTrueColor) {
    return (static_cast<int>(r) << 16) | (static_cast<int>(g) << 8) | static_cast<int>(b);
  }
  if (depth == ColorDepth::kAnsi256) {
    const std::vector<Rgb>& table = xterm256();
    int best = 0;
    int best_d = 0x7fffffff;
    for (std::size_t i = 0; i < table.size(); ++i) {
      const int d = squared_distance(want, table[i]);
      if (d < best_d) {
        best_d = d;
        best = static_cast<int>(i);
      }
    }
    return best;
  }
  // kBasic: only the bright half (8..15) is considered for foreground text,
  // because the dim half is unreadable as text on most terminal themes.
  int best = 15;
  int best_d = 0x7fffffff;
  for (int i = 8; i < 16; ++i) {
    const int d = squared_distance(want, kBasic[i]);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

Color color_of(Style style) noexcept {
  const auto i = static_cast<std::size_t>(style);
  return i < kSpecCount ? kSpec[i].fg : Color::kDefault;
}

bool env_requests_no_color() {
  // https://no-color.org: any non-empty value disables colour.
  const char* v = std::getenv("NO_COLOR");
  return v != nullptr && *v != '\0';
}

namespace {

/// Terminal capability probe. Ordered most-specific first.
ColorDepth probe_depth() {
  const char* term = std::getenv("TERM");
  // "dumb" means no capabilities at all: no colour, no cursor addressing.
  if (term == nullptr || std::string_view(term) == "dumb") return ColorDepth::kNone;

  // COLORTERM=truecolor|24bit is the near-universal signal for direct RGB.
  const char* colorterm = std::getenv("COLORTERM");
  if (colorterm != nullptr) {
    const std::string_view ct(colorterm);
    if (ct == "truecolor" || ct == "24bit") return ColorDepth::kTrueColor;
  }

  // Explicit opt-outs that only claim 256.
  if (std::getenv("TERM_PROGRAM") != nullptr) {
    // iTerm2, vscode and friends understand 256 even when TERM says xterm.
    return ColorDepth::kAnsi256;
  }

  const std::string_view t(term);
  if (t.find("256color") != std::string_view::npos) return ColorDepth::kAnsi256;
  if (t.find("direct") != std::string_view::npos) return ColorDepth::kTrueColor;
  if (t.find("color") != std::string_view::npos) return ColorDepth::kBasic;
  // "xterm", "screen", "vt100", "ansi": assume the original 16 colours.
  return ColorDepth::kBasic;
}

}  // namespace

Theme Theme::for_depth(ColorDepth depth) noexcept {
  Theme t;
  t.depth_ = depth;
  t.ansi_ = depth != ColorDepth::kNone;
  return t;
}

/// Whether the terminal understands escape sequences at all. This is a separate
/// question from colour: a terminal can move the cursor perfectly well while
/// being asked for monochrome, and conflating the two would strip a capable
/// terminal of its cursor addressing just because NO_COLOR was set.
static bool probe_ansi() {
  const char* term = std::getenv("TERM");
  if (term != nullptr && std::string_view(term) == "dumb") return false;
  return STELLAR_ISATTY(STELLAR_FILENO(stdout)) != 0;
}

Theme Theme::detect(bool force_no_color) {
  Theme t;
  t.ansi_ = probe_ansi();
  // NO_COLOR and --no-color turn off colour only. The interface still draws
  // with cursor addressing on a capable terminal; what it must never do is
  // emit a colour sequence, and on a terminal that cannot address the cursor at
  // all nothing is emitted either.
  if (force_no_color || env_requests_no_color()) {
    t.depth_ = ColorDepth::kNone;
    return t;
  }
  if (!t.ansi_) {
    t.depth_ = ColorDepth::kNone;
    return t;
  }
  t.depth_ = probe_depth();
  return t;
}

void Theme::append_sgr(std::string& out, Style style) const {
  const auto i = static_cast<std::size_t>(style);
  if (i >= kSpecCount) return;
  const StyleSpec& s = kSpec[i];

  std::vector<int> params;
  if (has_attr(s.attr, Attr::kBold)) params.push_back(1);
  if (has_attr(s.attr, Attr::kDim)) params.push_back(2);
  if (has_attr(s.attr, Attr::kItalic)) params.push_back(3);
  if (has_attr(s.attr, Attr::kUnderline)) params.push_back(4);
  if (has_attr(s.attr, Attr::kReverse)) params.push_back(7);

  // Attributes first, then colour, so a terminal that only understands one of
  // the two still applies the other.
  const auto emit_color = [&](Color c, bool background) {
    if (c == Color::kDefault && !background) return;
    int v;
    switch (depth_) {
      case ColorDepth::kTrueColor: {
        const Rgb rgb = rgb_of(c);
        v = rgb_to_depth(rgb.r, rgb.g, rgb.b, depth_);
        params.push_back(background ? 48 : 38);
        params.push_back(2);
        params.push_back((v >> 16) & 0xff);
        params.push_back((v >> 8) & 0xff);
        params.push_back(v & 0xff);
        break;
      }
      case ColorDepth::kAnsi256:
        v = rgb_to_depth(rgb_of(c).r, rgb_of(c).g, rgb_of(c).b, depth_);
        params.push_back(background ? 48 : 38);
        params.push_back(5);
        params.push_back(v);
        break;
      case ColorDepth::kBasic:
        v = rgb_to_depth(rgb_of(c).r, rgb_of(c).g, rgb_of(c).b, depth_);
        params.push_back(background ? (40 + v) : (30 + v - 8));
        break;
      case ColorDepth::kNone:
        return;
    }
  };

  emit_color(s.fg, /*background=*/false);
  if (s.bg == Bg::kBlue) emit_color(Color::kBlue, /*background=*/true);
  if (params.empty()) return;

  out += "\033[";
  for (std::size_t k = 0; k < params.size(); ++k) {
    if (k != 0) out += ';';
    out += std::to_string(params[k]);
  }
  out += 'm';
}

std::string_view Theme::reset() const {
  // SGR 0. Emitted even in no-colour mode: a terminal we may have styled before
  // a crash still needs telling, and on a no-colour terminal this is harmless.
  static constexpr std::string_view kReset = "\033[0m";
  return kReset;
}

std::string_view Theme::paint(Style style) const {
  if (depth_ == ColorDepth::kNone) return {};
  // One cached buffer per (depth, style): paint() runs for every styled run on
  // every frame, so it must not allocate. The depth is part of the key because
  // the tests build several themes at once and a style-only cache would hand
  // one theme's escapes to another.
  thread_local std::array<std::array<std::string, kSpecCount>, 4> cache{};
  thread_local std::array<std::array<bool, kSpecCount>, 4> valid{};
  const auto i = static_cast<std::size_t>(style);
  const auto d = static_cast<std::size_t>(depth_);
  if (i >= kSpecCount || d >= cache.size()) return {};
  if (!valid[d][i]) {
    cache[d][i].clear();
    append_sgr(cache[d][i], style);
    valid[d][i] = true;
  }
  return cache[d][i];
}

// --- UTF-8 width -----------------------------------------------------------

namespace {

/// Decodes one codepoint. `len` receives the byte length. Malformed input is
/// consumed one byte at a time and reported as U+FFFD, so a corrupt string can
/// never make this loop non-terminating.
std::uint32_t decode_utf8(std::string_view s, std::size_t i, std::size_t& len) noexcept {
  const auto b0 = static_cast<unsigned char>(s[i]);
  len = 1;
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if ((b1 & 0xC0) == 0x80) {
      len = 2;
      return static_cast<std::uint32_t>(((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu));
    }
    return 0xFFFD;
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    const auto b2 = static_cast<unsigned char>(s[i + 2]);
    if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
      len = 3;
      return static_cast<std::uint32_t>(((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) |
                                        (b2 & 0x3Fu));
    }
    return 0xFFFD;
  }
  if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    const auto b2 = static_cast<unsigned char>(s[i + 2]);
    const auto b3 = static_cast<unsigned char>(s[i + 3]);
    if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
      len = 4;
      return static_cast<std::uint32_t>(((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                                        ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu));
    }
    return 0xFFFD;
  }
  return 0xFFFD;
}

/// Columns occupied by one codepoint. Box drawing and block glyphs are narrow
/// (width 1) despite being above U+2000; the wide ranges are CJK and emoji.
std::size_t codepoint_width(std::uint32_t cp) noexcept {
  if (cp == 0) return 0;                                 // combining mark
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;   // C0/C1 control
  if ((cp >= 0x1100 && cp <= 0x115F) ||                    // Hangul Jamo
      (cp >= 0x2E80 && cp <= 0x303E) ||                    // CJK radicals/punct
      (cp >= 0x3041 && cp <= 0x33FF) ||                    // kana, CJK compat
      (cp >= 0x3400 && cp <= 0x4DBF) ||                    // CJK ext A
      (cp >= 0x4E00 && cp <= 0x9FFF) ||                    // CJK unified
      (cp >= 0xA000 && cp <= 0xA4CF) ||                    // Yi
      (cp >= 0xAC00 && cp <= 0xD7A3) ||                    // Hangul syllables
      (cp >= 0xF900 && cp <= 0xFAFF) ||                    // CJK compat ideo
      (cp >= 0xFF00 && cp <= 0xFF60) ||                    // fullwidth forms
      (cp >= 0x1F300 && cp <= 0x1F64F) ||                  // emoji
      (cp >= 0x1F900 && cp <= 0x1F9FF)) {
    return 2;
  }
  return 1;
}

}  // namespace

std::size_t display_width(std::string_view s) noexcept {
  std::size_t w = 0;
  for (std::size_t i = 0; i < s.size();) {
    std::size_t len = 1;
    const std::uint32_t cp = decode_utf8(s, i, len);
    w += codepoint_width(cp);
    i += len;
  }
  return w;
}

std::string truncate_to_width(std::string_view s, std::size_t cols) {
  std::size_t w = 0;
  for (std::size_t i = 0; i < s.size();) {
    std::size_t len = 1;
    const std::uint32_t cp = decode_utf8(s, i, len);
    const std::size_t cw = codepoint_width(cp);
    if (w + cw > cols) return std::string(s.substr(0, i));
    w += cw;
    i += len;
  }
  return std::string(s);
}

}  // namespace stellar::tui

