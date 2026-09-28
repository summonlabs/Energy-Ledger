// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An unexplained residual is retained and reported, never assigned to a
// category and never silently zeroed.

#include <cstdio>
#include <string>

#include "example_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

}  // namespace

int main() {
  const std::string path = "example-02-store";
  std::remove((path + "/LEDGER.STORE").c_str());

  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  if (!ledger) {
    return example::fail("create", ledger.error());
  }

  EntrySpec delivered;
  delivered.kind = EntryKind::Delivered;
  delivered.value = 1000;
  EntrySpec consumed;
  consumed.kind = EntryKind::Consumed;
  consumed.value = 860;
  EntrySpec unclassified;
  unclassified.kind = EntryKind::Unclassified;
  unclassified.value = 40;
  unclassified.quality = ObservationQuality::Unknown;
  unclassified.source_instance = "classifier";
  unclassified.source_family = "classification";

  for (const EntrySpec* spec : {&delivered, &consumed, &unclassified}) {
    auto result = example::append(ledger.value(), *spec);
    if (!result) {
      return example::fail("append", result.error());
    }
  }

  RollupQuery query;
  auto object = FacilityObjectRef::parse("facility/line-1");
  if (!object) {
    return example::fail("object", object.error());
  }
  query.object = object.value();

  auto report = ledger.value().reconcile(query);
  if (!report) {
    return example::fail("reconcile", report.error());
  }
  const Reconciliation& value = report.value();
  std::cout << "delivered-joules=" << value.delivered.joules() << "\n";
  std::cout << "consumed-joules=" << value.consumed.joules() << "\n";
  std::cout << "unclassified-recorded-joules=" << value.unclassified_recorded.joules() << "\n";
  std::cout << "residual-joules=" << value.measured_residual.joules()
            << " residual-is-zero=" << (value.measured_residual_is_zero ? "true" : "false") << "\n";
  for (const ExplanationLine& line : value.explanation) {
    std::cout << "explain " << line.code << ": " << line.detail << "\n";
  }

  // A second interval where the source reports less than the classified sum:
  // the residual is negative and is preserved exactly as observed.
  EntrySpec short_delivered;
  short_delivered.kind = EntryKind::Delivered;
  short_delivered.value = 500;
  short_delivered.interval = "2026-01-01T01:00Z/PT1H";
  short_delivered.interval_start = 1767229200;
  short_delivered.interval_end = 1767232800;
  short_delivered.observed_at = 1767232800;
  EntrySpec over_consumed;
  over_consumed.kind = EntryKind::Consumed;
  over_consumed.value = 520;
  over_consumed.interval = short_delivered.interval;
  over_consumed.interval_start = short_delivered.interval_start;
  over_consumed.interval_end = short_delivered.interval_end;
  over_consumed.observed_at = short_delivered.observed_at;
  for (const EntrySpec* spec : {&short_delivered, &over_consumed}) {
    auto result = example::append(ledger.value(), *spec);
    if (!result) {
      return example::fail("append", result.error());
    }
  }

  RollupQuery second;
  second.object = object.value();
  auto interval = IntervalRef::parse(short_delivered.interval);
  if (!interval) {
    return example::fail("interval", interval.error());
  }
  second.all_intervals = false;
  second.interval = interval.value();
  auto second_report = ledger.value().reconcile(second);
  if (!second_report) {
    return example::fail("reconcile", second_report.error());
  }
  std::cout << "second-interval residual-joules=" << second_report.value().measured_residual.joules()
            << "\n";

  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }
  return 0;
}
