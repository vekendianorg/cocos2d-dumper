// SPDX-License-Identifier: MIT
// The Stellar TUI: screens, navigation and the frame loop.
//
// This file is the *frontend*. It owns no analysis knowledge: every fact it puts
// on screen comes out of an AnalysisSnapshot, which the core fills in. That is
// the rule the whole design rests on -- a widget that wanted a number the
// snapshot does not have has to render "--", because inventing a plausible one
// would be the single most damaging thing this UI could do.
//
// The loop is a plain poll-with-timeout, not a callback model, for one reason:
// a dump of a real binary runs for tens of seconds with no input at all, so the
// loop must keep redrawing on a timer whether or not anybody presses a key.
// The timeout is the user's redraw interval, which is why that setting is wired
// to something real.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <unordered_map>

#include "stellar/tui/analysis.h"
#include "stellar/tui/dwarfview.h"
#include "stellar/tui/screen.h"
#include "stellar/tui/terminal.h"
#include "stellar/tui/theme.h"

namespace stellar::tui {

/// Interactive front-end. Not copyable: it owns a Terminal and an Analysis.
class App {
 public:
  struct Options {
    bool no_color = false;          ///< --no-color / NO_COLOR
    std::string initial_path;       ///< `stellar <elf>`: the preselected input
  };

  /// Every screen the TUI has. The set is fixed on purpose: TODO.md asks for a
  /// stable navigation structure so later features can be added without
  /// redesigning how the user moves around.
  enum class ScreenId { kMain, kEmit, kAnalysis, kComplete, kSettings, kInfo, kUnits, kScan };

  /// The smallest terminal the screens are laid out for. Anything smaller gets
  /// the single "terminal too small" notice instead of a squashed, wrapped
  /// frame. The threshold lives here, in one place, so it cannot drift between
  /// the painter and the tests.
  static constexpr int kMinCols = 30;
  static constexpr int kMinRows = 10;

  App();
  /// Test seam: takes the theme from the caller instead of probing a terminal,
  /// so the whole render path can be exercised with no tty and no escape
  /// sequences. run() is unchanged by this -- it re-probes regardless.
  explicit App(Theme theme);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  /// Runs the session and returns the process exit code (0 on a clean quit).
  int run(const Options& options);

  [[nodiscard]] ScreenId screen() const noexcept { return screen_; }
  [[nodiscard]] const AnalysisSnapshot& snapshot() const noexcept { return snap_; }
  [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

  // --- test seams ------------------------------------------------------------
  //
  // These exist so the layout can be proven at sizes no CI runner has a
  // terminal for. They drive exactly the same drawing code as run(); nothing is
  // duplicated for the test, which is the only reason they can be trusted.

  /// Renders one screen at a fixed size from a caller-supplied snapshot and
  /// returns the frame buffer. Touches no terminal, starts no threads.
  [[nodiscard]] std::string render_frame_for_test(int cols, int rows,
                                                  const AnalysisSnapshot& snap,
                                                  ScreenId id);

  /// Seeds the editable input path and the derived output path.
  void set_input_path_for_test(std::string path);
  /// Runs the startup autofill against `dir` instead of the real cwd.
  void autofill_input_for_test(const std::string& dir) { autofill_input(dir); }
  /// Sets the emit toggles (methods / padding / unit information).
  void set_emit_options_for_test(bool methods, bool pad_layout, bool build_units);
  /// Feeds one decoded key through the same handler the event loop calls, so
  /// navigation can be proven without a terminal.
  void handle_key_for_test(const Event& e) { handle_key(e); }
  /// Runs the file check on the current input at once (the loop debounces it).
  void probe_now_for_test() { run_probe(); }
  [[nodiscard]] const std::string& ghost_for_test() const noexcept { return ghost_; }
  void set_help_for_test(bool on) { help_ = on; }
  /// Seeds the unit browser / scan screens with data, so they can be laid out.
  void set_units_for_test(UnitList u) { units_ = std::move(u); units_sel_ = 0; }
  void set_scan_for_test(ScanSnapshot s) { scan_snap_ = std::move(s); }

 private:

  // --- editing model --------------------------------------------------------

  /// One line of editable text: the main input path, the output path, the
  /// output directory.
  ///
  /// The cursor is a byte offset rather than a cell index, because a path may
  /// hold multi-byte characters and splitting one would corrupt the value.
  /// `scroll` is the display column the view starts at, which is what keeps a
  /// long path inside its own box.
  struct Field {
    std::string text;
    std::size_t cursor = 0;
    int scroll = 0;
  };

  /// Focusable rows of the settings screen, in draw order.
  enum class Setting : std::uint8_t {
    kThreads,
    kRam,
    kAdaptive,
    kParallel,
    kProgressMode,
    kInterval,
    kDir,
    kCount,
  };

  /// Focusable rows of the emit screen, in draw order.
  enum class EmitItem : std::uint8_t {
    kOutPath,   // first: it is the first thing drawn, so focus order = draw order
    kMethods,
    kPadding,
    kUnits,
    kMaxLines,
    kStart,
    kCount,
  };

  // --- geometry -------------------------------------------------------------

  /// The interior of the frame: the rows and columns a screen may draw in.
  /// Four rows are chrome (top border, separator, footer, bottom border), and
  /// two columns go to the vertical borders.
  struct Region {
    int top = 0;     ///< first row inside the frame
    int bottom = 0;  ///< one past the last row inside the frame
    int left = 0;    ///< first column inside the frame
    int width = 0;   ///< usable columns
    bool headed = false;  ///< the logo header took the rows above `top`
  };
  [[nodiscard]] static Region content_region(int cols, int rows) noexcept;
  /// Rows the shared logo header takes on `id` at this size: 0 when the
  /// terminal is too small to show it *and* the screen's own content.
  [[nodiscard]] static int header_rows(int cols, int rows, ScreenId id) noexcept;
  /// The line under the logo: the tagline on the main screen, the screen's name
  /// everywhere else.
  [[nodiscard]] std::string screen_subtitle() const;
  void draw_header(Screen& s, const Region& base, int rows) const;

  // --- painting -------------------------------------------------------------
  //
  // One painter per screen. They are all const and all write into a Screen
  // handed to them, which is what lets the test seam drive the real layout
  // with no terminal attached.

  void paint(Screen& s) const;
  void paint_main(Screen& s, const Region& r) const;
  void paint_emit(Screen& s, const Region& r) const;
  void paint_analysis(Screen& s, const Region& r) const;
  void paint_complete(Screen& s, const Region& r) const;
  void paint_settings(Screen& s, const Region& r) const;
  void paint_info(Screen& s, const Region& r) const;
  void paint_units(Screen& s, const Region& r) const;
  void paint_scan(Screen& s, const Region& r) const;
  /// Fallback for terminals below kMinCols x kMinRows. Draws no frame, and
  /// degrades to shorter wording rather than ever overflowing the grid.
  void paint_too_small(Screen& s) const;

  /// The canonical banner, as two-tone cells.
  void draw_logo(Screen& s, int row, int col) const;
  /// An editable field: prompt, text and a block cursor.
  void draw_field(Screen& s, int row, int col, int width, const Field& f,
                  std::string_view prompt, bool caret, std::string_view ghost = {}) const;

  // --- input ----------------------------------------------------------------

  void handle_key(const Event& e);
  void handle_main_key(const Event& e);
  void handle_emit_key(const Event& e);
  void handle_settings_key(const Event& e);
  void handle_complete_key(const Event& e);
  void handle_units_key(const Event& e);
  void handle_scan_key(const Event& e);
  /// Lists the compilation units of the (valid) input and opens the browser.
  void open_units();
  void start_scan();
  [[nodiscard]] int list_rows() const noexcept { return list_rows_; }
  /// Text editing for whichever field has the caret. True when consumed.
  bool field_key(Field& f, const Event& e);
  void field_insert(Field& f, std::string_view utf8);
  void field_backspace(Field& f);
  void field_delete_forward(Field& f);
  void field_move(Field& f, int delta);
  void field_home_end(Field& f, bool home);
  /// Scrolls `f` just far enough to keep its cursor inside `view_cols`.
  void field_ensure_visible(Field& f, int view_cols) const;

  // --- actions --------------------------------------------------------------

  /// Starts the real Analysis and switches to the progress screen.
  void start_analysis();
  /// What Enter/R does for a main-screen menu row.
  void run_main_action(int item);
  void set_screen(ScreenId id);
  void set_status(std::string message, int kind = 0);
  void toggle_emit_item(int item);
  void begin_edit_max_lines();
  void commit_max_lines();
  void begin_edit_interval();
  void commit_interval();
  void cycle_progress_mode(int delta);
  void apply_settings();
  /// Re-derives the output path from the output directory setting.
  void sync_output_path();
  /// Everything that must follow a change to the input path: the derived output
  /// path, the inline completion and (debounced, or at once) the file check.
  void on_input_changed(bool immediate);
  /// Opens the typed path the way a run would and records what is wrong with it.
  void run_probe();
  void update_ghost();
  /// The text that would complete `text` to an existing path, or empty.
  [[nodiscard]] static std::string path_suggestion(const std::string& text);
  /// `~` and `~/x` expanded against $HOME; anything else unchanged.
  [[nodiscard]] static std::string expand_home(const std::string& text);
  /// True while a text field owns the keyboard (printable keys are text).
  [[nodiscard]] bool text_focus() const noexcept;
  void paint_help(Screen& s, const Region& r) const;
  void paint_status(Screen& s) const;
  /// Prefills the input from `dir`: the one ELF in it, else `dir/`.
  void autofill_input(const std::string& dir);
  /// Resizes both grids and forces a full repaint.
  void layout();
  /// Recomputes the field scroll offsets before a paint.
  void sync_view();
  /// Paints one frame to the terminal.
  void draw();
  /// The poll timeout, in milliseconds, and the bound the loop clamps to.
  [[nodiscard]] int redraw_interval() const noexcept;
  [[nodiscard]] std::string output_path() const;

  // --- state ----------------------------------------------------------------

  Theme theme_;
  Terminal term_;
  Screen cur_;   ///< the frame being built
  Screen prev_;  ///< the frame last written, for render_diff()
  Analysis analysis_;
  AnalysisSnapshot snap_{};
  ScreenId screen_ = ScreenId::kMain;

  // main
  Field input_;
  int main_panel_ = 0;  ///< 0 = input field, 1 = action menu, 2 = output path
  int main_item_ = 0;   ///< selected action, 0..4

  // emit
  bool opt_methods_ = true;
  bool opt_pad_ = true;
  bool opt_units_ = true;
  std::uint64_t max_lines_ = 0;  ///< 0 = unlimited
  Field out_path_;
  Field max_lines_edit_;         ///< scratch buffer while the limit is edited
  int emit_item_ = 0;
  bool editing_out_ = false;
  bool out_custom_ = false;  ///< the user typed the path; stop re-deriving it
  std::string out_saved_;    ///< restored when an output-path edit is cancelled
  bool editing_max_lines_ = false;

  // settings
  int redraw_ms_ = 80;
  int progress_mode_ = 0;  ///< 0 auto, 1 always, 2 never
  int threads_limit_ = 16;   ///< editable, but the core is single-threaded
  int ram_limit_mb_ = 8192;  ///< soft cap on a run's resident memory; 0 = off
  bool adaptive_ = true;     ///< displayed, never applied
  bool parallel_ = true;     ///< displayed, never applied
  Field out_dir_;
  std::string dir_saved_;  ///< value to restore when an edit is cancelled
  int setting_ = 0;
  bool editing_interval_ = false;
  bool editing_dir_ = false;
  bool editing_threads_ = false;
  bool editing_ram_ = false;
  Field interval_edit_;
  Field threads_edit_;
  Field ram_edit_;

  // live input check and inline completion
  FileFacts live_facts_;        ///< what the typed path opens as (valid=false if not)
  std::string path_note_;       ///< why it does not ("" = fine or nothing typed)
  bool probe_pending_ = false;  ///< an edit has not been checked yet
  std::chrono::steady_clock::time_point probe_due_{};
  std::string ghost_;           ///< suggested completion, accepted with Right

  // browse compilation units / scan DWARF
  UnitList units_;
  int units_sel_ = 0;
  std::unordered_map<std::uint64_t, std::uint64_t> unit_dies_;  ///< lazily counted
  ScanJob scan_;
  ScanSnapshot scan_snap_;
  bool watching_scan_ = false;
  int scan_scroll_ = 0;
  mutable int list_rows_ = 10;  ///< rows the last list paint had; sizes PgUp/PgDn

  // confirmations
  bool overwrite_armed_ = false;  ///< the next start request overwrites
  bool cancel_armed_ = false;     ///< the next cancel request really cancels
  bool help_ = false;

  // completion
  int complete_item_ = 0;

  // session
  bool quit_ = false;
  int exit_code_ = 0;
  bool watching_analysis_ = false;
  bool force_full_ = true;  ///< repaint every row instead of diffing
  bool need_clear_ = true;  ///< clear the terminal before the next full paint
  bool dirty_ = true;       ///< something changed that has not been repainted
  std::string status_;      ///< one line, shown on the separator row
  int status_kind_ = 0;     ///< 0 note, 1 success, 2 error (decides the symbol)
};

}  // namespace stellar::tui

