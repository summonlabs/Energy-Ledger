// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Out-of-tree consumer of the installed Energy Ledger package. It exercises the
// public API only: create, append, roll up, reconcile, verify, replay, close and
// reopen. It exits non-zero if any checked expectation fails.

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <energy_ledger/energy_ledger.hpp>

namespace {

using namespace energy_ledger;

int failures = 0;

void expect(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "downstream check failed: " << what << "\n";
    ++failures;
  }
}

EntryContent make_entry(EntryKind kind, const std::string& interval, const std::string& instance,
                        std::int64_t kilowatt_hours) {
  EntryContent content;
  content.kind = kind;
  content.quality = ObservationQuality::Verified;
  content.authority_tier = AuthorityTier::create(3).value();
  content.object = FacilityObjectRef::parse("facility/line-9").value();
  content.generation = GenerationRef::parse("gen-1").value();
  content.interval = IntervalRef::parse(interval).value();
  content.clock_domain = ClockDomainRef::parse("utc").value();
  content.source_family = SourceFamilyRef::parse("revenue-meter").value();
  content.source_instance = SourceInstanceRef::parse(instance).value();
  content.authority = AuthorityRef::parse("metering-service").value();
  content.time_basis = TimeBasis::UtcUnixSeconds;
  content.interval_start = TimeTicks(1767225600);
  content.interval_end = TimeTicks(1767229200);
  content.observed_at = TimeTicks(1767229200);
  content.source_revision = SourceRevision(1);
  content.source_generation = SourceGeneration(1);
  content.source_epoch = SourceEpoch(1);
  content.declared_value = kilowatt_hours;
  content.declared_unit = EnergyUnit::KilowattHour;
  content.quantity = to_canonical_energy(kilowatt_hours, EnergyUnit::KilowattHour).value();
  return content;
}

}  // namespace

int main() {
  std::cout << "Energy Ledger " << kVersionString << " downstream consumer\n";
  const std::string path = "downstream-store";

  auto removed = remove(path.c_str());
  (void)removed;

  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  if (!ledger) {
    std::cerr << "create failed: " << ledger.error().describe() << "\n";
    return 2;
  }

  AppendRequest delivered;
  delivered.content = make_entry(EntryKind::Delivered, "interval-1", "meter-9", 1000);
  delivered.request_id = "downstream-1";
  auto first = ledger.value().append(delivered);
  expect(first.has_value(), "the first append must commit");

  AppendRequest consumed;
  consumed.content = make_entry(EntryKind::Consumed, "interval-1", "submeter-9", 900);
  expect(ledger.value().append(consumed).has_value(), "the second append must commit");

  // A retry of an accepted request returns the prior result.
  auto retry = ledger.value().append(delivered);
  expect(retry.has_value() && retry.value().replayed(), "a retry must replay");
  expect(retry.value().sequence.value() == first.value().sequence.value(),
         "a replay must return the original sequence");

  RollupQuery query;
  query.object = FacilityObjectRef::parse("facility/line-9").value();
  auto rollup = ledger.value().rollup(query);
  expect(rollup.has_value(), "rollup must succeed");
  auto reconciliation = ledger.value().reconcile(query);
  expect(reconciliation.has_value(), "reconciliation must succeed");
  if (reconciliation) {
    expect(reconciliation.value().delivered.joules() == std::int64_t{1000} * 3600000,
           "delivered energy must be 1000 kWh");
    expect(reconciliation.value().measured_residual.joules() == std::int64_t{100} * 3600000,
           "the residual must be preserved as 100 kWh");
  }

  auto verified = ledger.value().verify();
  expect(verified.has_value() && verified.value().ok, "verification must pass");
  auto closed = ledger.value().close();
  expect(closed.has_value(), "close must succeed");

  OpenOptions reopen;
  reopen.mode = AccessMode::ReadOnly;
  auto reopened = Ledger::open(path, reopen);
  expect(reopened.has_value(), "reopen must succeed");
  if (reopened) {
    auto summary = reopened.value().replay();
    expect(summary.has_value() && summary.value().entries_replayed == 2,
           "replay must recover two entries");
    auto audit = reopened.value().audit();
    expect(audit.has_value() && audit.value().entry_count == 2,
           "the store audit must report two entries");
    auto closed_again = reopened.value().close();
    expect(closed_again.has_value(), "close must succeed");
  }

  auto cleanup = remove(path.c_str());
  (void)cleanup;
  std::remove((path + "/LEDGER.STORE").c_str());

  if (failures != 0) {
    std::cerr << failures << " downstream checks failed\n";
    return 1;
  }
  std::cout << "downstream consumer ok: find_package, link, append, replay, rollup, "
               "reconcile, verify, reopen\n";
  return 0;
}
