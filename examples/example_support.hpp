// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared helpers for the examples. Examples call the same public API that any
// other consumer uses; they contain no accounting logic of their own.

#ifndef ENERGY_LEDGER_EXAMPLE_SUPPORT_HPP
#define ENERGY_LEDGER_EXAMPLE_SUPPORT_HPP

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "energy_ledger/energy_ledger.hpp"

namespace example {

using namespace energy_ledger;

struct EntrySpec {
  EntryKind kind = EntryKind::Delivered;
  std::string object = "facility/line-1";
  std::string generation = "gen-1";
  std::string interval = "2026-01-01T00:00Z/PT1H";
  std::string clock_domain = "utc";
  std::int64_t interval_start = 1767225600;
  std::int64_t interval_end = 1767229200;
  std::int64_t observed_at = 1767229200;
  std::int64_t value = 0;
  EnergyUnit unit = EnergyUnit::KilowattHour;
  std::string source_family = "revenue-meter";
  std::string source_instance = "meter-1";
  std::uint64_t revision = 1;
  std::uint64_t source_generation = 1;
  std::uint64_t epoch = 1;
  std::uint32_t tier = 3;
  ObservationQuality quality = ObservationQuality::Verified;
  std::string authority = "metering-service";
  SequenceNumber target;
  std::vector<Annotation> annotations;
};

inline Expected<EntryContent> build(const EntrySpec& spec) {
  EntryContent content;
  content.kind = spec.kind;
  content.quality = spec.quality;

  auto tier = AuthorityTier::create(spec.tier);
  if (!tier) return tier.error();
  content.authority_tier = tier.value();

  auto object = FacilityObjectRef::parse(spec.object);
  if (!object) return object.error();
  content.object = object.value();
  auto generation = GenerationRef::parse(spec.generation);
  if (!generation) return generation.error();
  content.generation = generation.value();
  auto interval = IntervalRef::parse(spec.interval);
  if (!interval) return interval.error();
  content.interval = interval.value();
  auto clock = ClockDomainRef::parse(spec.clock_domain);
  if (!clock) return clock.error();
  content.clock_domain = clock.value();
  auto family = SourceFamilyRef::parse(spec.source_family);
  if (!family) return family.error();
  content.source_family = family.value();
  auto instance = SourceInstanceRef::parse(spec.source_instance);
  if (!instance) return instance.error();
  content.source_instance = instance.value();
  auto authority = AuthorityRef::parse(spec.authority);
  if (!authority) return authority.error();
  content.authority = authority.value();

  content.time_basis = TimeBasis::UtcUnixSeconds;
  content.interval_start = TimeTicks(spec.interval_start);
  content.interval_end = TimeTicks(spec.interval_end);
  content.observed_at = TimeTicks(spec.observed_at);

  content.source_revision = SourceRevision(spec.revision);
  content.source_generation = SourceGeneration(spec.source_generation);
  content.source_epoch = SourceEpoch(spec.epoch);
  content.target = spec.target;

  content.declared_value = spec.value;
  content.declared_unit = spec.unit;
  auto quantity = to_canonical_energy(spec.value, spec.unit);
  if (!quantity) return quantity.error();
  content.quantity = quantity.value();

  content.annotations = spec.annotations;
  auto valid = validate_content(content);
  if (!valid) return valid.error();
  return content;
}

inline Expected<AppendResult> append(Ledger& ledger, const EntrySpec& spec,
                                     const std::string& request_id = std::string()) {
  auto content = build(spec);
  if (!content) return content.error();
  AppendRequest request;
  request.content = content.value();
  request.request_id = request_id;
  return ledger.append(request);
}

inline int fail(const char* what, const Error& error) {
  std::cerr << "example failed at " << what << ": " << to_string(error.code) << ": "
            << error.detail << "\n";
  return 2;
}

inline std::string scratch_path(const std::string& name);

}  // namespace example

#endif  // ENERGY_LEDGER_EXAMPLE_SUPPORT_HPP
