// SPDX-License-Identifier: MIT
#include "stellar/diag/log.h"

#include <cstdarg>
#include <cstring>

namespace stellar::diag {
namespace {
const char* short_file(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}
const char* level_tag(Level l) {
  switch (l) {
    case Level::kTrace: return "TRACE";
    case Level::kDebug: return "DEBUG";
    case Level::kInfo: return "INFO ";
    case Level::kWarn: return "WARN ";
    case Level::kError: return "ERROR";
    case Level::kOff: break;
  }
  return "?????";
}
}  // namespace

bool Log::set_level_from_string(std::string_view name) {
  if (name == "trace") { set_level(Level::kTrace); return true; }
  if (name == "debug") { set_level(Level::kDebug); return true; }
  if (name == "info")  { set_level(Level::kInfo);  return true; }
  if (name == "warn")  { set_level(Level::kWarn);  return true; }
  if (name == "error") { set_level(Level::kError); return true; }
  if (name == "off" || name == "none") { set_level(Level::kOff); return true; }
  return false;
}

std::string_view Log::level_name(Level l) {
  switch (l) {
    case Level::kTrace: return "trace";
    case Level::kDebug: return "debug";
    case Level::kInfo:  return "info";
    case Level::kWarn:  return "warn";
    case Level::kError: return "error";
    case Level::kOff:   return "off";
  }
  return "?";
}

void Log::emit(Level l, std::string_view msg) {
  std::fprintf(stderr, "[%s] %.*s\n", level_tag(l), static_cast<int>(msg.size()), msg.data());
}

void Log::emitf(Level l, const char* file, int line, const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  std::fprintf(stderr, "[%s] %s:%d: %s\n", level_tag(l), short_file(file), line, buf);
}

}  // namespace stellar::diag
