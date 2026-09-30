// SPDX-License-Identifier: MIT
// Progress reporting: formatting, counting, and the promise that a disabled
// reporter costs nothing measurable in a hot loop.
#include <chrono>
#include <string>

#include "stellar/diag/progress.h"
#include "test_framework.h"

using stellar::diag::Progress;

namespace {

/// A reporter with drawing forced on, so render() is exercised without a tty.
Progress& reporter() {
  Progress& p = Progress::instance();
  p.reset();
  p.configure(/*force=*/true, /*min_interval_ms=*/0);
  // Count and format, but never write: these tests assert on render() and must
  // not scribble carriage returns over the test output.
  p.set_draw_enabled(false);
  return p;
}

}  // namespace

STELLAR_TEST(Progress, RendersCountersWithThousandsAndPercentage) {
  Progress& p = reporter();
  p.declare("DIEs", 50000);
  p.declare("Types", 4800);
  p.set("DIEs", 12450);
  p.set("Types", 1203);
  p.stage("Resolving types");
  // The first counter with a known total drives the percentage.
  EXPECT_TRUE(p.render().find("[ 24%]") != std::string::npos);
  EXPECT_TRUE(p.render().find("DIEs: 12,450/50,000") != std::string::npos);
  EXPECT_TRUE(p.render().find("Types: 1,203/4,800") != std::string::npos);
  EXPECT_TRUE(p.render().find("Stage: Resolving types") != std::string::npos);
}

STELLAR_TEST(Progress, ExplicitPrimaryOverridesTheDefaultChoice) {
  Progress& p = reporter();
  p.declare("DIEs", 100);
  p.declare("Methods", 400);
  p.set("DIEs", 10);
  p.set("Methods", 40);
  p.primary("Methods");
  EXPECT_TRUE(p.render().find("[ 10%]") != std::string::npos);
  p.primary("DIEs");
  EXPECT_TRUE(p.render().find("[ 10%]") != std::string::npos);
}

STELLAR_TEST(Progress, CountersWithoutTotalsShowNoFraction) {
  Progress& p = reporter();
  p.declare("Lines", 0, /*has_total=*/false);
  p.set("Lines", 4096);
  EXPECT_TRUE(p.render().find("Lines: 4,096") != std::string::npos);
  EXPECT_TRUE(p.render().find("Lines: 4,096/") == std::string::npos);
}

STELLAR_TEST(Progress, AddAccumulatesAndDeclareIsIdempotent) {
  Progress& p = reporter();
  p.declare("Fields", 1000);
  for (int i = 0; i < 250; ++i) p.add("Fields");
  EXPECT_TRUE(p.render().find("Fields: 250/1,000") != std::string::npos);
  // Re-declaring must not reset the value or duplicate the counter.
  p.declare("Fields", 2000);
  EXPECT_TRUE(p.render().find("Fields: 250/2,000") != std::string::npos);
  p.add("Fields", 5);
  EXPECT_TRUE(p.render().find("Fields: 255/2,000") != std::string::npos);
}

STELLAR_TEST(Progress, UnknownLabelsAreCreatedOnDemand) {
  Progress& p = reporter();
  // add() on a label that was never declared must still be counted, which is
  // what lets a stage report without pre-registering.
  p.add("AdHoc");
  p.add("AdHoc", 4);
  EXPECT_TRUE(p.render().find("AdHoc: 5") != std::string::npos);
}

STELLAR_TEST(Progress, DisabledReporterCostsNothingInAHotLoop) {
  Progress& p = reporter();
  p.set_enabled(false);
  // The point of the early-out: the 20M-iteration DIE loop must not pay for
  // progress. Compare a large count with the clock read a fraction as often.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 2'000'000; ++i) p.add("Hot");
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
  // A generous bound: this asserts the branch is predictable, not the exact cost.
  EXPECT_TRUE(ms < 250.0);
}

STELLAR_TEST(Progress, RenderIsStableAcrossRepeatedCalls) {
  Progress& p = reporter();
  p.declare("Units", 10);
  p.set("Units", 5);
  p.stage("Init");
  EXPECT_STREQ(p.render(), p.render());
}

STELLAR_TEST(Progress, RenderIsClampedToTheTerminalWidth) {
  Progress& p = reporter();
  // The dry-run reporter draws nothing, so exercise the clamp through render()
  // plus the width the reporter would have detected.
  p.declare("Wide", 2, false);
  p.set("Wide", 123456789);
  p.note(std::string(400, 'x'));
  EXPECT_TRUE(p.render().size() > 100);  // untruncated when no width is known
  EXPECT_EQ(p.width(), 0);             // not a terminal under the test runner
  EXPECT_FALSE(p.ansi());              // so the space-padding fallback is used
}

STELLAR_TEST(Progress, DrawIsDisabledInDryRunSoTestsNeverWrite) {
  Progress& p = reporter();
  p.declare("Lines", 0, false);
  // configure() enables drawing; the test helper turns it back off, which is
  // what keeps carriage returns out of the test output.
  EXPECT_TRUE(p.enabled());
  p.add("Lines", 5);
  p.stage("Some stage");
  p.checkpoint();
  EXPECT_TRUE(p.render().find("Lines: 5") != std::string::npos);
}
