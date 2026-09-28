// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef ENERGY_LEDGER_TESTS_UTIL_HPP
#define ENERGY_LEDGER_TESTS_UTIL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "energy_ledger/ledger.hpp"

namespace eltest {

/// Root directory for test state. Created on demand, removed by callers.
std::string scratch_root();

/// Returns a fresh, empty store path for the named scenario.
std::string fresh_store(const std::string& name);

/// Removes a store directory tree if it exists.
void remove_tree(const std::string& path);

/// Deterministic splitmix64 generator for seeded randomized cases.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed + 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next();
  std::int64_t range(std::int64_t low, std::int64_t high);
  std::size_t index(std::size_t count) { return static_cast<std::size_t>(next() % (count == 0 ? 1 : count)); }
  bool chance(std::uint32_t numerator, std::uint32_t denominator);

 private:
  std::uint64_t state_;
};

/// Absolute path of the energy-ledger CLI built by this configuration.
const std::string& cli_path();

/// Runs the CLI with the given argument list (already quoted per argument) and
/// returns its exit code. Output is discarded.
int run_cli(const std::vector<std::string>& arguments, std::string* output);

/// Convenience: builds an entry content with the shared defaults used across
/// the suites.
energy_ledger::EntryContent sample_entry(energy_ledger::EntryKind kind, std::int64_t value,
                                         const std::string& interval,
                                         const std::string& source_instance);

}  // namespace eltest

#endif  // ENERGY_LEDGER_TESTS_UTIL_HPP
