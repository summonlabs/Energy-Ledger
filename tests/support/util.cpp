// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/util.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "energy_ledger/test_support.hpp"
#include "support/process.hpp"

namespace eltest {

using namespace energy_ledger;

namespace {

std::string environment_or(const char* name, const std::string& fallback) {
#ifdef _MSC_VER
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
    std::string value(buffer);
    std::free(buffer);
    return value;
  }
  return fallback;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
#endif
}

}  // namespace

std::string scratch_root() {
  std::string root = environment_or("ENERGY_LEDGER_TEST_ROOT", "_scratch/test-state");
  auto created = test_support::create_directory(root);
  (void)created;
  auto segments = test_support::create_directory(root + "/stores");
  (void)segments;
  return root;
}

std::string fresh_store(const std::string& name) {
  const std::string path = scratch_root() + "/stores/" + name;
  remove_tree(path);
  return path;
}

void remove_tree(const std::string& path) {
  auto removed = test_support::remove_directory_tree(path);
  (void)removed;
}

std::uint64_t Rng::next() {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::int64_t Rng::range(std::int64_t low, std::int64_t high) {
  if (high <= low) {
    return low;
  }
  const std::uint64_t span = static_cast<std::uint64_t>(high - low) + 1ull;
  return low + static_cast<std::int64_t>(next() % span);
}

bool Rng::chance(std::uint32_t numerator, std::uint32_t denominator) {
  if (denominator == 0) {
    return false;
  }
  return (next() % denominator) < numerator;
}

const std::string& cli_path() {
  static const std::string path = EL_TEST_CLI_PATH;
  return path;
}

int run_cli(const std::vector<std::string>& arguments, std::string* output) {
  // The CLI is started directly, without a command shell, so quoting and
  // redirection never depend on shell rules.
  std::string command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) {
      command_line += " ";
    }
    command_line += "\"" + argument + "\"";
  }
  ChildProcess child;
  if (!child.spawn(cli_path(), command_line)) {
    if (output != nullptr) {
      *output = "failed to start the CLI";
    }
    return -1;
  }
  std::string text;
  for (;;) {
    const std::string line = child.read_line();
    const bool ended = child.stdout_ended();
    if (!line.empty()) {
      text += line;
      text += "\n";
    }
    if (ended) {
      break;
    }
  }
  const int status = child.wait();
  if (output != nullptr) {
    *output = text;
  }
  return status;
}

EntryContent sample_entry(EntryKind kind, std::int64_t value, const std::string& interval,
                          const std::string& source_instance) {
  EntryContent content;
  content.kind = kind;
  content.quality = ObservationQuality::Verified;
  content.authority_tier = AuthorityTier::create(3).value();
  content.object = FacilityObjectRef::parse("facility/line-1").value();
  content.generation = GenerationRef::parse("gen-1").value();
  content.interval = IntervalRef::parse(interval).value();
  content.clock_domain = ClockDomainRef::parse("utc").value();
  content.source_family = SourceFamilyRef::parse("revenue-meter").value();
  content.source_instance = SourceInstanceRef::parse(source_instance).value();
  content.authority = AuthorityRef::parse("metering-service").value();
  content.time_basis = TimeBasis::UtcUnixSeconds;
  content.interval_start = TimeTicks(1767225600);
  content.interval_end = TimeTicks(1767229200);
  content.observed_at = TimeTicks(1767229200);
  content.source_revision = SourceRevision(1);
  content.source_generation = SourceGeneration(1);
  content.source_epoch = SourceEpoch(1);
  content.declared_value = value;
  content.declared_unit = EnergyUnit::KilowattHour;
  content.quantity = to_canonical_energy(value, EnergyUnit::KilowattHour).value();
  return content;
}

}  // namespace eltest
