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
  for (int id = 0; id < 8; ++id)
    for (int r = 1; r <= maxr; ++r)
      for (int c = 1; c <= maxc; ++c) {
        App app(th);
        UnitList ul; ul.ok = true; ul.total = 20454;
        for (int i = 0; i < 300; ++i) { UnitRow u; u.index = i; u.offset = 0x1000u * i; u.version = 5; u.address_size = 8; ul.rows.push_back(u); }
        app.set_units_for_test(ul);
        ScanSnapshot ss; ss.phase = ScanSnapshot::Phase::kDone; ss.units_total = 20454; ss.units = 20454;
        ss.dies = 12345678; ss.max_depth = 31; ss.bytes = 500u << 20; ss.elapsed_seconds = 31.4;
        for (int i = 0; i < 40; ++i) ss.tags.emplace_back("DW_TAG_member_" + std::to_string(i), 900000 - i * 1000);
        app.set_scan_for_test(ss);
        app.set_input_path_for_test("/storage/emulated/0/very/long/path/to/libcocos2dcpp_1.74.2.so");
        std::string f = app.render_frame_for_test(c, r, snap, static_cast<App::ScreenId>(id));
        std::printf("%d %d %d %zu\n", c, r, id, f.size());
        std::fwrite(f.data(), 1, f.size(), stdout);
      }
}
