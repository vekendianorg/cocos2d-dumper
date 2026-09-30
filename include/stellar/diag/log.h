// SPDX-License-Identifier: MIT
// Minimal severity-filtered logging. Deliberately tiny: the dumper must never
// allocate on the DIE-walking path, so logging is only ever called at coarse
// granularity (CLI, phase boundaries, error reporting).
#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace stellar::diag {

enum class Level : int { kTrace = 0, kDebug = 1, kInfo = 2, kWarn = 3, kError = 4, kOff = 5 };

class Log {
 public:
  static void set_level(Level l) { storage() = l; }
  [[nodiscard]] static Level level() { return storage(); }
  /// Mutable reference to the process-wide level, so set_level can assign.
  static Level& storage() {
    static Level g_level = Level::kInfo;
    return g_level;
  }
  /// Parses "trace"/"debug"/"info"/"warn"/"error"/"off"; returns false if the
  /// name is not recognised.
  static bool set_level_from_string(std::string_view name);
  [[nodiscard]] static bool enabled(Level l) { return static_cast<int>(l) >= static_cast<int>(level()); }
  [[nodiscard]] static std::string_view level_name(Level l);

  static void emit(Level l, std::string_view msg);

  /// printf-style emit, used by the STELLAR_* macros below. Not on any hot path.
  ///
  /// The GCC/Clang format attribute is a compile-time check that the call sites
  /// match the format string; MSVC has no equivalent, so it is opted into
  /// only where it exists.
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 4, 5)))
#endif
  static void emitf(Level l, const char* file, int line, const char* fmt, ...);
};

#define STELLAR_LOG_AT(lvl_, ...)                                            \
  do {                                                                   \
    if (::stellar::diag::Log::enabled(lvl_)) {                              \
      ::stellar::diag::Log::emitf(lvl_, __FILE__, __LINE__, __VA_ARGS__);    \
    }                                                                    \
  } while (0)

#define STELLAR_TRACE(...) STELLAR_LOG_AT(::stellar::diag::Level::kTrace, __VA_ARGS__)
#define STELLAR_DEBUG(...) STELLAR_LOG_AT(::stellar::diag::Level::kDebug, __VA_ARGS__)
#define STELLAR_INFO(...) STELLAR_LOG_AT(::stellar::diag::Level::kInfo, __VA_ARGS__)
#define STELLAR_WARN(...) STELLAR_LOG_AT(::stellar::diag::Level::kWarn, __VA_ARGS__)
#define STELLAR_ERROR(...) STELLAR_LOG_AT(::stellar::diag::Level::kError, __VA_ARGS__)

}  // namespace stellar::diag
