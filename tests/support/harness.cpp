// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace eltest {
namespace {

std::uint64_t g_checks = 0;
std::uint64_t g_failures = 0;
std::uint64_t g_base_seed = 20260101ull;
bool g_list_only = false;
std::string g_filter;
std::string g_case;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, void (*function)()) {
  registry().push_back(TestCase{name, function});
}

void fail(const std::string& message) {
  ++g_failures;
  std::cerr << "    FAILED: " << message << "\n";
}

bool check(bool condition, const std::string& message) {
  ++g_checks;
  if (!condition) {
    fail(message);
    if (g_failures > 32) {
      throw CaseAborted{};
    }
  }
  return condition;
}

std::uint64_t checks_run() { return g_checks; }
std::uint64_t failures_in_case() { return g_failures; }
void reset_case_counters() { g_failures = 0; }

std::uint64_t base_seed() { return g_base_seed; }
void set_base_seed(std::uint64_t seed) { g_base_seed = seed; }

int run_all(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      g_list_only = true;
    } else if (argument == "--filter" && index + 1 < argc) {
      g_filter = argv[++index];
    } else if (argument == "--case" && index + 1 < argc) {
      g_case = argv[++index];
    } else if (argument == "--seed" && index + 1 < argc) {
      g_base_seed = std::strtoull(argv[++index], nullptr, 10);
    } else {
      std::cerr << "unknown argument '" << argument << "'\n";
      return 2;
    }
  }

  if (g_list_only) {
    for (const TestCase& item : registry()) {
      std::cout << item.name << "\n";
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failed_names;
  for (const TestCase& item : registry()) {
    if (!g_case.empty() && item.name != g_case) {
      continue;
    }
    if (!g_filter.empty() && item.name.find(g_filter) == std::string::npos) {
      continue;
    }
    ++executed;
    reset_case_counters();
    const std::uint64_t before = g_checks;
    try {
      item.function();
    } catch (const CaseAborted&) {
      std::cerr << "    case aborted after too many failures\n";
    }
    const std::uint64_t case_checks = g_checks - before;
    if (g_failures == 0) {
      std::cout << "[PASS] " << item.name << " (" << case_checks << " checks)\n";
    } else {
      std::cout << "[FAIL] " << item.name << " (" << g_failures << " failures, " << case_checks
                << " checks)\n";
      failed_names.push_back(item.name);
      ++failed;
    }
    std::cout.flush();
  }

  if (!g_case.empty() && executed == 0) {
    std::cerr << "no test case named '" << g_case << "'\n";
    return 2;
  }
  std::cout << "cases=" << executed << " failed=" << failed << " checks=" << g_checks
            << " seed=" << g_base_seed << "\n";
  for (const std::string& name : failed_names) {
    std::cout << "failed-case: " << name << "\n";
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace eltest

int main(int argc, char** argv) { return eltest::run_all(argc, argv); }
