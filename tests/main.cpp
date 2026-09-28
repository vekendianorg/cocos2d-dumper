// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>

#include "c2d/diag/log.h"
#include "test_framework.h"

int main(int argc, char** argv) {
  std::string filter;
  std::string exclude;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quiet") == 0) {
      quiet = true;
    } else if (std::strncmp(argv[i], "--filter=", 9) == 0) {
      filter = argv[i] + 9;
    } else if (std::strncmp(argv[i], "--exclude=", 10) == 0) {
      exclude = argv[i] + 10;
    } else {
      filter = argv[i];
    }
  }
  // By default the real-binary tests are excluded: they open the 583 MB input,
  // which must not happen on every edit. Opt in with --include-real or by
  // passing an explicit filter that names them.
  if (exclude.empty() && filter.find("RealBinary") == std::string::npos) {
    exclude = "RealBinary";
  }
  // Tests exercise the "not found" paths on purpose; keep the noise down.
  c2d::diag::Log::set_level(c2d::diag::Level::kError);
  if (quiet) {
    std::freopen(nullptr, "w", stdout);
  }
  return c2d::test::run_all(filter, exclude);
}
