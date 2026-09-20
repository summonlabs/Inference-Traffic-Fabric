// Inference Traffic Fabric - minimal deterministic test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TEST_HPP
#define ITF_TEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "itf/error.hpp"

namespace itf::test {

struct TestCase {
  const char* name;
  void (*body)();
};

[[nodiscard]] std::vector<TestCase>& registry();
bool register_test(const char* name, void (*body)());

/// Records a failure and prints it. Returns the condition so that callers can
/// combine it with control flow.
bool check(bool condition, const char* expression, const char* file, int line);

template <class A, class B>
bool check_equal(const A& actual, const B& expected, const char* left, const char* right,
                 const char* file, int line) {
  if (actual == expected) return true;
  return check(false, (std::string(left) + " == " + right).c_str(), file, line);
}

bool check_status_ok(const itf::Status& status, const char* expression, const char* file, int line);
bool check_status_code(const itf::Status& status, itf::StatusCode expected, const char* expression,
                       const char* file, int line);

[[nodiscard]] int failure_count();
[[nodiscard]] std::uint64_t seed();
[[nodiscard]] bool verbose();

/// Parses --seed N and --verbose from the command line and runs every
/// registered test case. Returns the process exit code.
int run_all(const char* suite_name, int argc, char** argv);

}  // namespace itf::test

#define ITF_TEST(name)                                                            \
  static void name();                                                             \
  static const bool name##_registered = ::itf::test::register_test(#name, &name); \
  static void name()

#define ITF_CHECK(condition) \
  ::itf::test::check((condition), #condition, __FILE__, __LINE__)

#define ITF_CHECK_EQ(actual, expected) \
  ::itf::test::check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define ITF_CHECK_STATUS_OK(status) \
  ::itf::test::check_status_ok((status), #status, __FILE__, __LINE__)

#define ITF_CHECK_STATUS_CODE(status, expected) \
  ::itf::test::check_status_code((status), (expected), #status, __FILE__, __LINE__)

#define ITF_REQUIRE(condition)                                     \
  do {                                                             \
    if (!ITF_CHECK(condition)) return;                             \
  } while (false)

#define ITF_REQUIRE_STATUS_OK(status)                              \
  do {                                                             \
    if (!ITF_CHECK_STATUS_OK(status)) return;                      \
  } while (false)

#endif  // ITF_TEST_HPP
