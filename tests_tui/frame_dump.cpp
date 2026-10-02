// Dumps one rendered frame per (cols,rows,screen) to stdout as:  "<cols> <rows> <screen> <nbytes>\n<bytes>"
#include <cstdio>
#include <cstdlib>
#include <string>
#include "stellar/tui/app.h"
using namespace stellar::tui;
int main(int argc, char** argv) {
  int maxc = argc > 1 ? atoi(argv[1]) : 120, maxr = argc > 2 ? atoi(argv[2]) : 40;
  int color = argc > 3 ? atoi(argv[3]) : 1;
  Theme th = color ? Theme::detect(false) : Theme::detect(true);
  if (color) th.set_ansi_enabled(true);
  AnalysisSnapshot snap;
  snap.file.valid = true; snap.file.format = "ELF64 DYN aarch64"; snap.file.machine = "aarch64";
  snap.file.size_text = "142.3 MB"; snap.file.has_dwarf = true; snap.file.unit_total = 20454;
  for (int id = 0; id < 6; ++id)
    for (int r = 1; r <= maxr; ++r)
      for (int c = 1; c <= maxc; ++c) {
        App app(th);
        app.set_input_path_for_test("/storage/emulated/0/very/long/path/to/libcocos2dcpp_1.74.2.so");
        std::string f = app.render_frame_for_test(c, r, snap, static_cast<App::ScreenId>(id));
        std::printf("%d %d %d %zu\n", c, r, id, f.size());
        std::fwrite(f.data(), 1, f.size(), stdout);
      }
}
