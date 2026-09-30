// SPDX-License-Identifier: MIT
// Tiny test framework.
//
// Deliberately dependency-free: the project must build with nothing but a C++20
// compiler, and the test suite has to stay fast enough to run on every change
// (no 583 MB binary required for the normal test run).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace stellar::test {

struct Case {
  const char* suite;
  const char* name;
  void (*fn)();
};

/// Registry of all cases. Function-local static keeps this header-only.
inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failure_count() {
  static int n = 0;
  return n;
}

inline bool& current_failed() {
  static bool b = false;
  return b;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    registry().push_back({suite, name, fn});
  }
};

inline void report_failure(const char* file, int line, std::string_view expr,
                           std::string_view detail) {
  current_failed() = true;
  ++failure_count();
  std::fprintf(stderr, "  FAIL %s:%d\n    %.*s\n", file, line, static_cast<int>(expr.size()),
               expr.data());
  if (!detail.empty()) {
    std::fprintf(stderr, "    -> %.*s\n", static_cast<int>(detail.size()), detail.data());
  }
}

/// Runs every registered case whose "Suite.Name" contains `filter` and does not
/// contain `exclude`. Empty arguments mean "no restriction".
inline int run_all(std::string_view filter, std::string_view exclude = {}) {
  int executed = 0;
  for (const Case& c : registry()) {
    std::string full = std::string(c.suite) + "." + c.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    if (!exclude.empty() && full.find(exclude) != std::string::npos) continue;
    current_failed() = false;
    ++executed;
    std::fprintf(stderr, "[ RUN      ] %s\n", full.c_str());
    try {
      c.fn();
    } catch (const std::exception& e) {
      current_failed() = true;
      ++failure_count();
      std::fprintf(stderr, "  threw: %s\n", e.what());
    } catch (...) {
      current_failed() = true;
      ++failure_count();
      std::fprintf(stderr, "  threw: unknown exception\n");
    }
    if (current_failed()) {
      std::fprintf(stderr, "[   FAILED ] %s\n", full.c_str());
    } else {
      std::fprintf(stderr, "[       OK ] %s\n", full.c_str());
    }
  }
  std::fprintf(stderr, "\n%d test(s) run, %d failure(s)\n", executed, failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace stellar::test

#define STELLAR_TEST(suite_, name_)                                                  \
  static void suite_##_##name_##_body();                                         \
  static ::stellar::test::Registrar suite_##_##name_##_reg(#suite_, #name_,          \
                                                     &suite_##_##name_##_body);   \
  static void suite_##_##name_##_body()

#define EXPECT_TRUE(expr)                                                        \
  do {                                                                            \
    if (!(expr)) ::stellar::test::report_failure(__FILE__, __LINE__, #expr, "");     \
  } while (0)

#define EXPECT_FALSE(expr)                                                       \
  do {                                                                            \
    if (static_cast<bool>(expr))                                                  \
      ::stellar::test::report_failure(__FILE__, __LINE__, "!(" #expr ")", "");        \
  } while (0)

#define EXPECT_EQ(a, b)                                                          \
  do {                                                                            \
    const auto stellar_a_ = (a);                                                      \
    const auto stellar_b_ = (b);                                                      \
    if (!(stellar_a_ == stellar_b_)) {                                                    \
      ::stellar::test::report_failure(__FILE__, __LINE__, #a " == " #b,              \
                                 "values differ");                                \
    }                                                                             \
  } while (0)

#define EXPECT_NE(a, b)                                                          \
  do {                                                                            \
    const auto stellar_a_ = (a);                                                      \
    const auto stellar_b_ = (b);                                                      \
    if (stellar_a_ == stellar_b_) {                                                       \
      ::stellar::test::report_failure(__FILE__, __LINE__, #a " != " #b,              \
                                 "values equal");                                 \
    }                                                                             \
  } while (0)

#define EXPECT_STREQ(a, b)                                                        \
  do {                                                                            \
    const std::string stellar_a_ = (a);                                               \
    const std::string stellar_b_ = (b);                                               \
    if (stellar_a_ != stellar_b_) {                                                       \
      ::stellar::test::report_failure(__FILE__, __LINE__, #a " == " #b,              \
                                 "\"" + stellar_a_ + "\" != \"" + stellar_b_ + "\"");   \
    }                                                                             \
  } while (0)

/// Size equality that reports the actual sizes, which is what matters when a
/// walk unexpectedly produced the wrong number of DIEs.
#define ASSERT_EQ_SIZE(container, n_)                                             \
  do {                                                                            \
    const std::size_t stellar_n_ = (n_);                                              \
    if ((container).size() != stellar_n_) {                                           \
      ::stellar::test::report_failure(__FILE__, __LINE__,                              \
                                 #container ".size() == " #n_,                    \
                                 "actual size = " + std::to_string((container).size())); \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define ASSERT_TRUE(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                             \
      ::stellar::test::report_failure(__FILE__, __LINE__, #cond, "required");      \
      return;                                                                 \
    }                                                                         \
  } while (0)
