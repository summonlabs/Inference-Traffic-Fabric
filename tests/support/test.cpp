// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace itf::test {
namespace {

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};
std::uint64_t g_seed = 0x5EED1234ULL;
bool g_verbose = false;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

bool register_test(const char* name, void (*body)()) {
  registry().push_back(TestCase{name, body});
  return true;
}

bool check(bool condition, const char* expression, const char* file, int line) {
  g_checks.fetch_add(1);
  if (condition) {
    if (g_verbose) std::printf("  ok    %s (%s:%d)\n", expression, file, line);
    return true;
  }
  g_failures.fetch_add(1);
  std::printf("  FAIL  %s (%s:%d)\n", expression, file, line);
  (void)std::fflush(stdout);
  return false;
}

bool check_status_ok(const itf::Status& status, const char* expression, const char* file, int line) {
  if (status.ok()) return check(true, expression, file, line);
  std::printf("  FAIL  %s -> %s (%s:%d)\n", expression, status.to_string().c_str(), file, line);
  g_failures.fetch_add(1);
  g_checks.fetch_add(1);
  (void)std::fflush(stdout);
  return false;
}

bool check_status_code(const itf::Status& status, itf::StatusCode expected, const char* expression,
                       const char* file, int line) {
  if (status.code() == expected) return check(true, expression, file, line);
  std::printf("  FAIL  %s -> %s (expected %s) (%s:%d)\n", expression, status.to_string().c_str(),
              std::string(itf::to_string(expected)).c_str(), file, line);
  g_failures.fetch_add(1);
  g_checks.fetch_add(1);
  (void)std::fflush(stdout);
  return false;
}

int failure_count() { return g_failures.load(); }
std::uint64_t seed() { return g_seed; }
bool verbose() { return g_verbose; }

int run_all(const char* suite_name, int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--seed") == 0 && index + 1 < argc) {
      g_seed = std::strtoull(argv[++index], nullptr, 10);
    } else if (std::strcmp(argv[index], "--verbose") == 0) {
      g_verbose = true;
    }
  }
  std::printf("[%s] seed=%llu cases=%zu\n", suite_name,
              static_cast<unsigned long long>(g_seed), registry().size());
  const int failures_before = g_failures.load();
  const auto started = std::chrono::steady_clock::now();
  for (const TestCase& test_case : registry()) {
    const int before = g_failures.load();
    std::printf("- %s\n", test_case.name);
    (void)std::fflush(stdout);
    test_case.body();
    if (g_failures.load() != before) {
      std::printf("  (case %s recorded %d failure(s))\n", test_case.name,
                  g_failures.load() - before);
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
  const int failures = g_failures.load() - failures_before;
  std::printf("[%s] checks=%d failures=%d elapsed_ms=%lld seed=%llu\n", suite_name, g_checks.load(),
              failures, static_cast<long long>(elapsed),
              static_cast<unsigned long long>(g_seed));
  return failures == 0 ? 0 : 1;
}

}  // namespace itf::test
