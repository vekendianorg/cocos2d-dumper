// The RAM limit is a real soft cap: a run whose resident memory passes it must
// stop, fail with a reason, and leave no partial dump behind.
#include <chrono>
#include <cstdio>
#include <thread>
#include "stellar/tui/analysis.h"
using namespace stellar::tui;
static int run(const char* in, const char* out, std::uint64_t cap, AnalysisSnapshot::Phase want, bool expect_msg) {
  Analysis a; Analysis::StartOptions o; o.input_path = in; o.out_path = out; o.ram_limit_bytes = cap;
  if (!a.start(o)) { std::puts("FAIL start"); return 1; }
  for (int i = 0; i < 400 && a.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto s = a.snapshot();
  const bool ok = s.phase == want && (!expect_msg || s.error.find("memory passed") != std::string::npos);
  std::printf("%s cap=%llu phase=%s err=%s\n", ok ? "PASS" : "FAIL", (unsigned long long)cap,
              Analysis::phase_name(s.phase), s.error.c_str());
  return ok ? 0 : 1;
}
int main(int, char** argv) {
  int bad = 0;
  bad += run(argv[1], argv[2], 1, AnalysisSnapshot::Phase::kFailed, true);          // 1 byte: must trip
  std::remove(argv[2]);
  bad += run(argv[1], argv[2], 0, AnalysisSnapshot::Phase::kDone, false);           // no cap: completes
  std::remove(argv[2]);
  bad += run(argv[1], argv[2], 64ull << 30, AnalysisSnapshot::Phase::kDone, false); // generous cap: completes
  return bad;
}
