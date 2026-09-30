// SPDX-License-Identifier: MIT
#include "stellar/util/bytes.h"

#include <cstdio>

namespace stellar::util {

std::string human_size(std::uint64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int unit = 0;
  while (v >= 1024.0 && unit < 4) {
    v /= 1024.0;
    ++unit;
  }
  char buf[64];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, kUnits[unit]);
  }
  return std::string(buf);
}

}  // namespace stellar::util
