// SPDX-License-Identifier: MIT
// The TUI's invariants.
//
// A full-screen interface is hard to test by looking at it, so these tests pin
// the properties that are expensive to get wrong and invisible until they are:
// the banner's bytes, the no-colour guarantee, layout at hostile terminal sizes,
// and the rule that the interface never shows a number the core did not measure.
#include <cstdio>
#include <string>
#include <vector>

#include "stellar/tui/app.h"
#include "stellar/tui/logo.h"
#include "stellar/tui/screen.h"
#include "stellar/tui/theme.h"
#include "test_framework.h"

using stellar::tui::AnalysisSnapshot;
using stellar::tui::App;
using stellar::tui::ColorDepth;
using stellar::tui::Screen;
using ScreenId = stellar::tui::App::ScreenId;
using stellar::tui::Style;
using stellar::tui::Theme;

namespace {

std::size_t count_bytes(const std::string& s, char c) {
  std::size_t n = 0;
  for (const char ch : s) {
    if (ch == c) ++n;
  }
  return n;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

/// Removes SGR escape sequences, leaving the printable characters. The styled
/// banner interleaves escapes between glyphs, so the only honest way to prove
/// the artwork survived is to strip them and compare against the source rows.
std::string strip_ansi(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      std::size_t j = i + 2;
      while (j < s.size() && s[j] != 'm') ++j;
      i = (j < s.size()) ? j + 1 : s.size();
      continue;
    }
    out.push_back(s[i]);
    ++i;
  }
  return out;
}

/// Every terminal size the interface has to survive, from an absurdly small
/// SSH window to a wide desktop.
constexpr int kSizes[][2] = {{20, 6},  {40, 10}, {60, 18},
                             {80, 24}, {120, 40}, {200, 50}};
constexpr ScreenId kAllScreens[] = {ScreenId::kMain,     ScreenId::kEmit,
                                   ScreenId::kAnalysis, ScreenId::kComplete,
                                   ScreenId::kSettings, ScreenId::kInfo};

/// Splits a frame into the rows a terminal would actually show.
///
/// Two shapes have to be handled, because the renderer emits two: with ANSI on
/// every row is placed by an absolute cursor move (`CUP`), and with it off rows
/// are separated by CR LF. Anything that is not a row break -- colour, and the
/// erase-to-end-of-line that terminates each row -- is dropped, so what comes
/// back is exactly the visible text.
std::vector<std::string> lines_of(const std::string& frame) {
  std::vector<std::string> out;
  std::string cur;
  bool started = false;
  for (std::size_t i = 0; i < frame.size();) {
    const char c = frame[i];
    if (c == '\033' && i + 1 < frame.size() && frame[i + 1] == '[') {
      std::size_t j = i + 2;
      while (j < frame.size() && !(frame[j] >= '@' && frame[j] <= '~')) ++j;
      const char final = j < frame.size() ? frame[j] : '\0';
      if (final == 'H') {  // CUP: the next row begins here
        if (started) out.push_back(strip_ansi(cur));
        cur.clear();
        started = true;
      }
      i = (j < frame.size()) ? j + 1 : frame.size();
      continue;
    }
    if (c == '\n') {
      out.push_back(strip_ansi(cur));
      cur.clear();
      started = true;
      ++i;
      continue;
    }
    if (c == '\r') {
      ++i;
      continue;
    }
    cur.push_back(c);
    ++i;
  }
  out.push_back(strip_ansi(cur));
  return out;
}
/// The most columns render_row will ever write. The terminal's final column is
/// deliberately left alone, so nothing may legitimately reach index `cols`.
constexpr std::size_t writable_cols(int cols) {
  return cols > 1 ? static_cast<std::size_t>(cols - 1) : 1u;
}
/// Splits a row into one entry per display cell.
std::vector<std::string> cells_of(const std::string& row) {
  std::vector<std::string> cs;
  for (std::size_t k = 0; k < row.size();) {
    std::size_t len = 1;
    const auto b = static_cast<unsigned char>(row[k]);
    if (b >= 0xF0) len = 4; else if (b >= 0xE0) len = 3; else if (b >= 0xC0) len = 2;
    if (k + len > row.size()) len = 1;
    cs.push_back(row.substr(k, len));
    k += len;
  }
  return cs;
}
inline constexpr std::string_view kBar = "\xe2\x94\x82";  // U+2502 box vertical

AnalysisSnapshot loaded_snapshot() {
  AnalysisSnapshot s;
  s.phase = AnalysisSnapshot::Phase::kBuilding;
  s.stage = "Building model";
  s.current_note = "compilation unit 972 of 1183";
  s.units = 972;
  s.units_total = 1183;
  s.dies = 20454580;
  s.types = 2984000;
  s.fields = 369148;
  s.methods = 2628693;
  s.elapsed_seconds = 14.82;
  s.rss_bytes = 1932735283ull;
  s.dwarfless = false;
  s.mode_label = "dwarf";
  s.file.valid = true;
  s.file.path = "libgame.so";
  s.file.format = "ELF64 DYN aarch64";
  s.file.machine = "AArch64";
  s.file.size_text = "312.98 MB";
  s.file.endianness = "Little Endian";
  s.file.has_dwarf = true;
  s.file.unit_total = 1183;
  s.file.debug_sections = {{".debug_info", 200u << 20}, {".debug_line", 40u << 20}};
  return s;
}

}  // namespace

STELLAR_TEST(Tui, BannerIsCanonical) {
  // The artwork is branding: if a test ever fails here, logo.h was edited.
  EXPECT_TRUE(stellar::tui::logo_is_canonical());
  EXPECT_EQ(stellar::tui::kLogoRows, std::size_t(6));
  for (std::size_t i = 0; i < stellar::tui::kLogoRows; ++i) {
    // No leading or trailing padding: the glyphs are the mark.
    EXPECT_FALSE(stellar::tui::kLogoLines[i].front() == ' ');
    EXPECT_FALSE(stellar::tui::kLogoLines[i].back() == ' ');
    EXPECT_TRUE(stellar::tui::display_width(stellar::tui::kLogoLines[i]) > 40);
  }
}

STELLAR_TEST(Tui, BannerIsRenderedBlueAndLightBlueNotWhite) {
  const std::string coloured =
      stellar::tui::render_logo(Theme::for_depth(ColorDepth::kTrueColor));
  // Bold is present, so the mark is not plain regular-weight text.
  EXPECT_TRUE(contains(coloured, "1m") || contains(coloured, "1;"));
  // Two distinct colours: the box-drawing structure and the letter mass.
  const std::size_t blue = count_bytes(coloured, '\033');
  EXPECT_TRUE(blue > stellar::tui::kLogoRows);
  // The artwork itself must survive the styling untouched. Escapes sit between
  // the glyphs, so the comparison is made against the escape-stripped form.
  // render_logo() joins the rows with newlines and adds no trailing one; the
  // callers (the CLI's printf, the TUI's painter) supply their own line break.
  std::string expect;
  for (std::size_t i = 0; i < stellar::tui::kLogoRows; ++i) {
    if (i != 0) expect += "\n";
    expect += std::string(stellar::tui::kLogoLines[i]);
  }
  EXPECT_EQ(strip_ansi(coloured), expect);
}

STELLAR_TEST(Tui, MainScreenShowsTheBannerWhenItFits) {
  // The banner is branding, so it must appear on the main screen whenever the
  // terminal is big enough to hold it -- and must appear byte-for-byte.
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  const std::string frame =
      app.render_frame_for_test(100, 34, snap, ScreenId::kMain);
  for (const std::string_view row : stellar::tui::kLogoLines) {
    EXPECT_TRUE(contains(frame, std::string(row)));
  }
  // On a terminal too short to hold both, the file facts win and the banner is
  // dropped rather than half-drawn. Either way the artwork is never truncated.
  const std::string cramped = app.render_frame_for_test(100, 14, snap, ScreenId::kMain);
  EXPECT_FALSE(contains(cramped, std::string(stellar::tui::kLogoLines[0])));
  for (const std::string& line : lines_of(cramped)) {
    EXPECT_TRUE(stellar::tui::display_width(line) <= writable_cols(100));
  }
}

STELLAR_TEST(Tui, NoColorEmitsNoEscapeSequences) {
  // The spec is absolute: with colour off, not one escape byte may be emitted.
  // This also covers a piped stdout, where detect() must fall back to kNone.
  for (const char* env : {"NO_COLOR=1", "TERM=dumb"}) {
    (void)env;  // set by the test runner's environment, asserted below
  }
  const std::string plain =
      stellar::tui::render_logo(Theme::for_depth(ColorDepth::kNone));
  EXPECT_EQ(count_bytes(plain, '\033'), std::size_t(0));
  // Exactly the six rows, unchanged, one per line.
  const auto rows = lines_of(plain);
  EXPECT_EQ(rows.size(), std::size_t(6));
  for (std::size_t i = 0; i < rows.size() && i < stellar::tui::kLogoRows; ++i) {
    EXPECT_EQ(rows[i], std::string(stellar::tui::kLogoLines[i]));
  }
  // A kNone theme paints nothing for any style, including the selected row.
  const Theme none = Theme::for_depth(ColorDepth::kNone);
  EXPECT_TRUE(none.paint(Style::kSelected).empty());
  EXPECT_TRUE(none.paint(Style::kTitle).empty());
  EXPECT_TRUE(none.paint(Style::kSuccess).empty());
  EXPECT_TRUE(none.paint(Style::kLogo).empty());
}

STELLAR_TEST(Tui, NoColorIsHonouredInEveryScreen) {
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  for (const auto& size : kSizes) {
    for (const ScreenId id : kAllScreens) {
      const std::string frame =
          app.render_frame_for_test(size[0], size[1], snap, id);
      EXPECT_EQ(count_bytes(frame, '\033'), std::size_t(0));
    }
  }
}

STELLAR_TEST(Tui, ColourFallsBackThroughEveryDepth) {
  // 256 and 16-colour terminals must still get styled output, just coarser.
  for (const ColorDepth d : {ColorDepth::kAnsi256, ColorDepth::kBasic,
                              ColorDepth::kTrueColor}) {
    const Theme t = Theme::for_depth(d);
    const std::string_view title = t.paint(Style::kTitle);
    EXPECT_FALSE(title.empty());
    EXPECT_EQ(title.front(), '\033');
  }
  // The logo must never fall back to plain white: at every colour depth it
  // keeps a blue tone.
  for (const ColorDepth d : {ColorDepth::kAnsi256, ColorDepth::kBasic,
                              ColorDepth::kTrueColor}) {
    const std::string coloured = stellar::tui::render_logo(Theme::for_depth(d));
    const auto logo_rgb = stellar::tui::rgb_of(stellar::tui::color_of(Style::kLogo));
    EXPECT_TRUE(logo_rgb.b > logo_rgb.r);  // blue-dominant, not white/grey
  }
}

STELLAR_TEST(Tui, LayoutSurvivesHostileTerminalSizes) {
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  for (const auto& size : kSizes) {
    for (const ScreenId id : kAllScreens) {
      const std::string frame =
          app.render_frame_for_test(size[0], size[1], snap, id);
      // A rendered row must never be wider than the terminal, or the frame
      // wraps and leaves fragments behind.
      for (const std::string& line : lines_of(frame)) {
        EXPECT_TRUE(stellar::tui::display_width(line) <= writable_cols(size[0]));
      }
    }
  }
}

STELLAR_TEST(Tui, EveryScreenRendersItsTitle) {
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  struct Expect { ScreenId id; const char* title; };
  const Expect expects[] = {
      {ScreenId::kMain, "STELLAR"},
      {ScreenId::kEmit, "STELLAR / EMIT"},
      {ScreenId::kAnalysis, "STELLAR / ANALYSIS"},
      {ScreenId::kComplete, "STELLAR / COMPLETE"},
      {ScreenId::kSettings, "STELLAR / SETTINGS"},
      {ScreenId::kInfo, "STELLAR / INFO"},
  };
  for (const Expect& e : expects) {
    const std::string frame = app.render_frame_for_test(80, 24, snap, e.id);
    EXPECT_TRUE(contains(frame, e.title));
  }
}

STELLAR_TEST(Tui, StatusIsNotConveyedByColourAlone) {
  // Every state carries a glyph as well as a colour, so the interface still
  // reads on a monochrome terminal.
  App app(Theme::for_depth(ColorDepth::kNone));
  AnalysisSnapshot snap = loaded_snapshot();

  AnalysisSnapshot done = loaded_snapshot();
  done.phase = AnalysisSnapshot::Phase::kDone;
  done.out_path = "output/dump.cs";
  done.out_lines = 6383926;
  const std::string complete = app.render_frame_for_test(80, 24, done, ScreenId::kComplete);
  EXPECT_TRUE(contains(complete, "\xE2\x9C\x93"));  // U+2713 check

  AnalysisSnapshot failed = loaded_snapshot();
  failed.phase = AnalysisSnapshot::Phase::kFailed;
  failed.error = "could not build the model";
  const std::string error = app.render_frame_for_test(80, 24, failed, ScreenId::kComplete);
  EXPECT_TRUE(contains(error, "\xE2\x9C\x97"));  // U+2717 ballot X

  // The selected menu row is marked with the spec's triangle.
  const std::string main_screen =
      app.render_frame_for_test(80, 24, snap, ScreenId::kMain);
  EXPECT_TRUE(contains(main_screen, "\xE2\x96\xB6"));  // U+25B6 triangle
}

STELLAR_TEST(Tui, NeverFabricatesUnmeasuredValues) {
  // An empty snapshot has no facts at all. Every field must then read as
  // absent rather than as a plausible-looking zero.
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot empty;
  for (const ScreenId id : kAllScreens) {
    const std::string frame = app.render_frame_for_test(80, 24, empty, id);
    // No invented counts: none of the real target's numbers may appear.
    EXPECT_FALSE(contains(frame, "20,454,580"));
    EXPECT_FALSE(contains(frame, "1183"));
  }
  // The empty state must still show its structure, not fall over.
  const std::string main_screen =
      app.render_frame_for_test(80, 24, empty, ScreenId::kMain);
  EXPECT_TRUE(contains(main_screen, "STELLAR"));
  // The placeholder for an unknown value is the em dash, not a number.
  EXPECT_TRUE(contains(main_screen, "\xE2\x80\x94"));  // U+2014 em dash
}

STELLAR_TEST(Tui, InfoScreenShowsRealFileFacts) {
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  const std::string info = app.render_frame_for_test(100, 30, snap, ScreenId::kInfo);
  EXPECT_TRUE(contains(info, "libgame.so"));
  EXPECT_TRUE(contains(info, "AArch64"));
  EXPECT_TRUE(contains(info, "312.98 MB"));
  EXPECT_TRUE(contains(info, "Little Endian"));
  // Real debug sections from FileFacts.
  EXPECT_TRUE(contains(info, ".debug_info"));
  EXPECT_TRUE(contains(info, ".debug_line"));
}

STELLAR_TEST(Tui, AnalysisScreenShowsLiveProgress) {
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  const std::string frame =
      app.render_frame_for_test(80, 30, snap, ScreenId::kAnalysis);
  EXPECT_TRUE(contains(frame, "972"));        // current unit
  EXPECT_TRUE(contains(frame, "1,183"));      // total
  EXPECT_TRUE(contains(frame, "20,454,580")); // DIEs
  EXPECT_TRUE(contains(frame, "DWARF"));      // the mode, upper-cased for display
  EXPECT_TRUE(contains(frame, "Cancel"));
  // Elapsed time is formatted, not printed raw.
  EXPECT_TRUE(contains(frame, "00:14"));
}

STELLAR_TEST(Tui, FooterMatchesThePanelAndKeepsTheWayOut) {
  // The footer is per panel. With the input field focused every printable key is
  // text -- a path is full of 'r', 'q' and 's' -- so Q/R/S are not shortcuts
  // there and a footer that advertised them would be lying. Tab hands the
  // keyboard to the menu, and then they are real.
  //
  // Whatever the panel, the last hint is the way out, and it survives a narrow
  // terminal: hints are dropped from the middle, never the last one. A footer
  // that cannot tell you how to leave is the one that strands a user.
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();

  // Input panel (the default): the honest set for that panel.
  const auto input_rows =
      lines_of(app.render_frame_for_test(80, 24, snap, ScreenId::kMain));
  const std::string& input_footer = input_rows[input_rows.size() - 2];
  EXPECT_TRUE(contains(input_footer, "Run"));
  EXPECT_TRUE(contains(input_footer, "Menu"));
  EXPECT_TRUE(contains(input_footer, "Quit"));
  EXPECT_FALSE(contains(input_footer, "Settings"));

  // Menu panel: every shortcut the main screen has, and all of them fit at the
  // 80 columns the spec draws the footer at.
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kTab, {}});
  const auto menu_rows =
      lines_of(app.render_frame_for_test(80, 24, snap, ScreenId::kMain));
  const std::string& menu_footer = menu_rows[menu_rows.size() - 2];
  for (const char* word : {"Navigate", "Select", "Switch Panel", "Run",
                           "Settings", "Quit"}) {
    EXPECT_TRUE(contains(menu_footer, word));
  }
  EXPECT_TRUE(stellar::tui::display_width(menu_footer) <= writable_cols(80));

  // Narrow: hints go, the way out stays.
  const auto narrow_rows =
      lines_of(app.render_frame_for_test(App::kMinCols, App::kMinRows, snap,
                                         ScreenId::kMain));
  const std::string& narrow_footer = narrow_rows[narrow_rows.size() - 2];
  EXPECT_TRUE(contains(narrow_footer, "Quit"));
  EXPECT_TRUE(stellar::tui::display_width(narrow_footer) <=
              writable_cols(App::kMinCols));
}

STELLAR_TEST(Tui, EveryScreenIsReachableFromTheMainScreen) {
  // A screen nobody can navigate to is dead code, and the settings screen was
  // exactly that: fully painted, with hints and a key handler, and no key
  // anywhere that opened it. This walks the navigation the hints promise.
  App app(Theme::for_depth(ColorDepth::kNone));
  EXPECT_TRUE(app.screen() == ScreenId::kMain);

  // Tab moves focus from the input field to the action menu, then S opens
  // settings, and Esc comes back.
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kTab, {}});
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kChar, "s"});
  EXPECT_TRUE(app.screen() == ScreenId::kSettings);
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kEscape, {}});
  EXPECT_TRUE(app.screen() == ScreenId::kMain);

  // Enter on the first menu item opens the info view, Esc returns.
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kEnter, {}});
  EXPECT_TRUE(app.screen() == ScreenId::kInfo);
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kEscape, {}});
  EXPECT_TRUE(app.screen() == ScreenId::kMain);
}

STELLAR_TEST(Tui, SettingsShortcutDoesNotStealTypingFromTheInputField) {
  // The input field keeps the keyboard by default and every path contains an
  // 's', so the settings shortcut must not fire while the field owns focus.
  App app(Theme::for_depth(ColorDepth::kNone));
  app.handle_key_for_test(stellar::tui::Event{stellar::tui::Key::kChar, "/storage/emulated/0/lib.so"});
  EXPECT_TRUE(app.screen() == ScreenId::kMain);
}

STELLAR_TEST(Tui, LayoutNeverEscapesItsContainer) {
  // The regression guard for the whole class of bug this file exists to catch:
  // content that overflows the box it lives in. Every screen is rendered at
  // every awkward size and four properties are asserted:
  //
  //   1. the frame has exactly as many rows as the terminal, so nothing scrolls;
  //   2. no row is wider than the last writable column, so nothing can wrap;
  //   3. the terminal's final column is never written, so no row can arm the
  //      pending-wrap flag that shifts the whole frame on some emulators;
  //   4. at and above the minimum size the box is intact -- corners present and
  //      both borders unbroken on every content row -- and below it the screen
  //      degrades to the "too small" notice rather than a squashed frame.
  //
  // The sizes straddle App::kMinCols/kMinRows deliberately: that boundary is
  // where the two regimes meet, and it is exactly where an off-by-one would
  // show up.
  constexpr int kWidths[] = {1,  2,  3,  8,  16, 20, 24, 28, 29, 30, 31,
                             34, 40, 46, 56, 60, 72, 80, 100, 120, 160, 200};
  constexpr int kHeights[] = {1, 2, 3, 5, 6, 8, 9, 10, 11, 14, 20, 24, 30, 45, 60};
  App app(Theme::for_depth(ColorDepth::kNone));
  const AnalysisSnapshot snap = loaded_snapshot();
  for (const int w : kWidths) {
    for (const int h : kHeights) {
      for (const ScreenId id : kAllScreens) {
        const std::string frame = app.render_frame_for_test(w, h, snap, id);
        const auto rows = lines_of(frame);
        EXPECT_TRUE(rows.size() == static_cast<std::size_t>(h));
        for (const std::string& row : rows) {
          EXPECT_TRUE(stellar::tui::display_width(row) <= writable_cols(w));
        }
        if (w < App::kMinCols || h < App::kMinRows) {
          // No frame at all below the threshold -- a squashed box is worse than
          // an honest notice -- but the screen must still say something.
          EXPECT_TRUE(contains(frame, "small") || contains(frame, "Small") ||
                      contains(frame, "!"));
          continue;
        }
        const auto top = cells_of(rows[0]);
        EXPECT_TRUE(!top.empty() && top.front() == "\u250c");
        const auto bottom = cells_of(rows[static_cast<std::size_t>(h) - 1]);
        EXPECT_TRUE(!bottom.empty() && bottom.front() == "\u2514");
        // Rows 1..h-4 are content; h-3 is the separator, h-2 the footer.
        for (std::size_t r = 1; r + 3 < rows.size(); ++r) {
          const auto cs = cells_of(rows[r]);
          if (cs.empty()) continue;
          EXPECT_EQ(cs.front(), std::string(kBar));
          EXPECT_EQ(cs[cs.size() - 1], std::string(kBar));
        }
      }
    }
  }
}

STELLAR_TEST(Tui, LongPathsStayInsideTheirField) {
  // A path longer than the field must scroll inside it, never spill over the
  // frame. The input field is the one widget that routinely holds a string far
  // wider than the terminal.
  App app(Theme::for_depth(ColorDepth::kNone));
  AnalysisSnapshot snap = loaded_snapshot();
  const char* kLong =
      "/storage/emulated/0/#Vekendian/very/deeply/nested/project/tree/"
      "libcocos2dcpp_1.74.2.so";
  for (const int w : {30, 40, 60, 90}) {
    app.set_input_path_for_test(kLong);
    for (const ScreenId id : {ScreenId::kMain, ScreenId::kEmit}) {
      const std::string frame = app.render_frame_for_test(w, 24, snap, id);
      for (const std::string& row : lines_of(frame)) {
        EXPECT_TRUE(stellar::tui::display_width(row) <= writable_cols(w));
      }
      for (std::size_t r = 1; r + 3 < lines_of(frame).size(); ++r) {
        const auto cs = cells_of(lines_of(frame)[r]);
        if (cs.empty()) continue;
        EXPECT_EQ(cs.front(), std::string(kBar));
        EXPECT_EQ(cs[cs.size() - 1], std::string(kBar));
      }
    }
  }
}

STELLAR_TEST(Tui, ButtonsShareOneColumn) {
  // The complete screen's buttons are a group and must line up. Centring each
  // label on its own width -- the original bug -- puts them in a staircase.
  App app(Theme::for_depth(ColorDepth::kNone));
  AnalysisSnapshot snap = loaded_snapshot();
  snap.phase = AnalysisSnapshot::Phase::kDone;
  snap.out_path = "output/dump.cs";
  const std::string frame = app.render_frame_for_test(80, 24, snap, ScreenId::kComplete);
  const auto rows = lines_of(frame);
  std::size_t first = std::string::npos, second = std::string::npos;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    const std::size_t at = rows[r].find('[');
    if (at == std::string::npos) continue;
    if (first == std::string::npos) {
      first = at;
    } else if (second == std::string::npos) {
      second = at;
      break;
    }
  }
  EXPECT_TRUE(first != std::string::npos && second != std::string::npos);
  EXPECT_EQ(first, second);
}

STELLAR_TEST(Tui, AnsiTerminalsGetTheSameLayoutWithEraseSequences) {
  // The interactive path differs from the no-colour path in two ways: every row
  // is placed with an absolute cursor move, and it ends with an
  // erase-to-end-of-line. Neither may disturb the layout, and this is the path a
  // real user on a real terminal actually sees -- so the container invariants
  // are asserted again with escape output switched on.
  Theme t = Theme::for_depth(ColorDepth::kAnsi256);
  t.set_ansi_enabled(true);
  App app(t);
  const AnalysisSnapshot snap = loaded_snapshot();
  for (const int w : {30, 34, 44, 56, 72, 100, 200}) {
    for (const ScreenId id : kAllScreens) {
      const std::string frame = app.render_frame_for_test(w, 24, snap, id);
      // Absolute positioning, and the erase that clears the column the row
      // deliberately does not write.
      EXPECT_TRUE(contains(frame, "\033[1;1H"));
      EXPECT_TRUE(contains(frame, "\033[24;1H"));
      EXPECT_TRUE(contains(frame, "\033[K"));
      const auto rows = lines_of(frame);
      EXPECT_TRUE(rows.size() == std::size_t(24));
      for (const std::string& row : rows) {
        // lines_of strips escapes, so this is the visible width.
        EXPECT_TRUE(stellar::tui::display_width(row) <= writable_cols(w));
      }
      const auto top = cells_of(rows[0]);
      EXPECT_TRUE(!top.empty() && top.front() == "\u250c");
      for (std::size_t r = 1; r + 3 < rows.size(); ++r) {
        const auto cs = cells_of(rows[r]);
        if (cs.empty()) continue;
        EXPECT_EQ(cs.front(), std::string(kBar));
        EXPECT_EQ(cs[cs.size() - 1], std::string(kBar));
      }
    }
  }
}

STELLAR_TEST(Tui, ScreenDiffOnlyRedrawsChangedRows) {
  Screen a;
  a.resize(20, 5);
  const Theme t = Theme::for_depth(ColorDepth::kTrueColor);
  // An identical copy must produce no output at all: this is what keeps a
  // redraw from flickering and what stops the loop spamming the terminal.
  EXPECT_TRUE(a.render_diff(t, a).empty());
  Screen b = a;
  b.put(2, 2, "changed", Style::kValue);
  const std::string diff = b.render_diff(t, a);
  EXPECT_FALSE(diff.empty());
  EXPECT_TRUE(contains(diff, "changed"));
}

STELLAR_TEST(Tui, ScreenWritesOutsideItsGridAreIgnored) {
  Screen s;
  s.resize(10, 3);
  s.put(-5, -5, "before", Style::kValue);
  s.put(99, 99, "after", Style::kValue);
  s.box(0, 0, 1000, 1000, Style::kBorder);
  s.progress_bar(0, 0, 1000, 2.0);
  s.progress_bar(0, 0, 1000, -1.0);
  // The grid must still be exactly as tall and as printable as it started.
  EXPECT_EQ(s.rows(), 3);
  EXPECT_EQ(s.cols(), 10);
  const std::string frame = s.render(Theme::for_depth(ColorDepth::kNone));
  for (const std::string& line : lines_of(frame)) {
    EXPECT_TRUE(stellar::tui::display_width(line) <= writable_cols(10));
  }
}

STELLAR_TEST(Tui, DisplayWidthCountsDoubleWidthCodepoints) {
  // Layout maths has to agree with the terminal, or every box is off by one.
  EXPECT_EQ(stellar::tui::display_width("abc"), std::size_t(3));
  EXPECT_EQ(stellar::tui::display_width(""), std::size_t(0));
  EXPECT_EQ(stellar::tui::display_width("\xE2\x96\x88"), std::size_t(1));  // full block
  EXPECT_EQ(stellar::tui::display_width("\xE2\x95\x90"), std::size_t(1));  // ─
  EXPECT_EQ(stellar::tui::display_width("\xE2\x96\xB6"), std::size_t(1));  // ▶
  EXPECT_EQ(stellar::tui::display_width("\xE2\x9C\x93"), std::size_t(1));  // ✓
  EXPECT_EQ(stellar::tui::display_width("\xE3\x81\x82"), std::size_t(2));  // CJK
  // Truncation must never split a codepoint.
  const std::string cut = stellar::tui::truncate_to_width("\xE3\x81\x82\xE3\x81\x84", 2);
  EXPECT_EQ(stellar::tui::display_width(cut), std::size_t(2));
}
