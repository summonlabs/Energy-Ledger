// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal test harness. Cases are registered at load time and run in
// registration order by a single-threaded runner. No case is bounded by a
// timeout: a hanging case is a defect that must be diagnosed.

#ifndef ENERGY_LEDGER_TESTS_HARNESS_HPP
#define ENERGY_LEDGER_TESTS_HARNESS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace eltest {

struct TestCase {
  std::string name;
  void (*function)();
};

/// Thrown internally when a single case produces too many failures, so the
/// remaining cases still run.
struct CaseAborted {};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*function)());
};

/// Records a failure against the running case.
void fail(const std::string& message);
/// Records a check outcome; returns the outcome so callers can branch.
bool check(bool condition, const std::string& message);
std::uint64_t checks_run();
std::uint64_t failures_in_case();
void reset_case_counters();

/// Seeds available to randomized cases (fixed default for reproducibility).
std::uint64_t base_seed();
void set_base_seed(std::uint64_t seed);

int run_all(int argc, char** argv);

}  // namespace eltest

#define EL_TEST(name)                                                            \
  static void el_test_case_##name();                                             \
  static const ::eltest::Registrar el_test_registrar_##name(#name,               \
                                                            &el_test_case_##name); \
  static void el_test_case_##name()

#define EL_CHECK(expression)   ::eltest::check((expression), std::string(#expression) + " @ " + __FILE__ + ":" + std::to_string(__LINE__))

#define EL_CHECK_MSG(expression, message)   ::eltest::check((expression), std::string(message) + " @ " + __FILE__ + ":" + std::to_string(__LINE__))

#define EL_REQUIRE(expression)                                                       \
  do {                                                                               \
    if (!::eltest::check((expression), std::string(#expression) + " (required) @ " +  \
                                             __FILE__ + ":" + std::to_string(__LINE__))) { \
      return;                                                                        \
    }                                                                                \
  } while (false)

#define EL_CHECK_EQ(actual, expected)                                             \
  do {                                                                            \
    const auto el_actual = (actual);                                              \
    const auto el_expected = (expected);                                          \
    ::eltest::check(el_actual == el_expected,                                     \
                    std::string(#actual " == " #expected) + " @ " + __FILE__ +    \
                        ":" + std::to_string(__LINE__));                          \
  } while (false)

#endif  // ENERGY_LEDGER_TESTS_HARNESS_HPP
