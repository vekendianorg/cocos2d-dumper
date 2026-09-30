// SPDX-License-Identifier: MIT
#include "c2d/diag/metrics.h"

#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "c2d/util/bytes.h"

namespace c2d::diag {
namespace {

/// Reads a "Key:  <number> kB" line from /proc/self/status.
///
/// Windows has no /proc; there the process API is used instead.
std::uint64_t read_status_kb(const char* key) {
#if defined(_WIN32)
  // Only the working-set figure is meaningful without /proc.
  if (std::strcmp(key, "VmHWM") != 0 && std::strcmp(key, "VmRSS") != 0) return 0;
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
#else
  std::ifstream f("/proc/self/status");
  if (!f) return 0;
  std::string line;
  const std::size_t klen = std::strlen(key);
  while (std::getline(f, line)) {
    if (line.compare(0, klen, key) == 0) {
      const char* p = line.c_str() + klen;
      while (*p == ':' || *p == ' ' || *p == '\t') ++p;
      return std::strtoull(p, nullptr, 10) * 1024ull;
    }
  }
  return 0;
#endif
}

}  // namespace

std::uint64_t peak_rss_bytes() { return read_status_kb("VmHWM"); }
std::uint64_t current_rss_bytes() { return read_status_kb("VmRSS"); }

void Metrics::add_phase(std::string name, double seconds, std::uint64_t count,
                        std::uint64_t bytes) {
  auto it = phases_.find(name);
  if (it == phases_.end()) {
    PhaseStat st;
    st.name = name;
    it = phases_.emplace(std::move(name), std::move(st)).first;
  }
  it->second.seconds += seconds;
  it->second.count += count;
  it->second.bytes += bytes;
}

void Metrics::add_peak_rss(std::uint64_t bytes) {
  if (bytes > peak_rss_) peak_rss_ = bytes;
}

std::string Metrics::report() const {
  std::ostringstream os;
  if (!label_.empty()) os << "== " << label_ << " ==\n";
  for (const auto& [name, st] : phases_) {
    os << "  phase " << name << ": " << (st.seconds * 1000.0) << " ms";
    if (st.count) os << "  count=" << st.count;
    if (st.bytes) os << "  bytes=" << util::human_size(st.bytes);
    os << "\n";
  }
  for (const auto& [key, value] : counters_) {
    os << "  counter " << key << " = " << value << "\n";
  }
  if (bytes_read_) os << "  bytes_read = " << util::human_size(bytes_read_) << "\n";
  const std::uint64_t rss = peak_rss();
  if (rss) os << "  peak_rss = " << util::human_size(rss) << "\n";
  return os.str();
}

ResourceReport resource_report(double wall_seconds) {
  ResourceReport r;
  r.wall_seconds = wall_seconds;
  r.peak_rss_bytes = peak_rss_bytes();
  r.current_rss_bytes = current_rss_bytes();
#if !defined(_WIN32)
  // rusage is POSIX; Windows has no equivalent, so user/system time stay 0 there.
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    r.user_seconds = static_cast<double>(ru.ru_utime.tv_sec) +
                     static_cast<double>(ru.ru_utime.tv_usec) * 1e-6;
    r.system_seconds = static_cast<double>(ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_stime.tv_usec) * 1e-6;
  }
#endif
  return r;
}

}  // namespace c2d::diag
