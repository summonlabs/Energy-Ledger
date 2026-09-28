// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Normal energy flow accounting: delivered, consumed, wasted/lost, a
// reservation commitment and a curtailment, then a rollup, a reconciliation, a
// verified reopen and an integrity check.

#include <cstdio>
#include <string>

#include "example_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

std::string store_path() { return "example-01-store"; }

}  // namespace

int main() {
  const std::string path = store_path();
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
  consumed.value = 900;
  EntrySpec wasted;
  wasted.kind = EntryKind::WastedLost;
  wasted.value = 60;
  wasted.source_instance = "loss-model";
  wasted.source_family = "derived";
  wasted.quality = ObservationQuality::Estimated;
  EntrySpec committed;
  committed.kind = EntryKind::ReservationCommit;
  committed.value = 1200;
  committed.source_instance = "capacity-planner";
  committed.source_family = "planning";
  EntrySpec curtailed;
  curtailed.kind = EntryKind::Curtailed;
  curtailed.value = 200;
  curtailed.source_instance = "capacity-planner";
  curtailed.source_family = "planning";

  for (const EntrySpec* spec : {&delivered, &consumed, &wasted, &committed, &curtailed}) {
    auto result = example::append(ledger.value(), *spec);
    if (!result) {
      return example::fail("append", result.error());
    }
    std::cout << "appended " << to_string(spec->kind) << " sequence=" << result.value().sequence.value()
              << " generation=" << result.value().generation.value() << "\n";
  }

  RollupQuery query;
  auto object = FacilityObjectRef::parse("facility/line-1");
  if (!object) {
    return example::fail("object", object.error());
  }
  query.object = object.value();

  auto rollup = ledger.value().rollup(query);
  if (!rollup) {
    return example::fail("rollup", rollup.error());
  }
  for (const GenerationRollup& generation : rollup.value().generations) {
    std::cout << "rollup generation=" << generation.generation.value() << "\n";
    for (const CategoryTotal& category : generation.categories) {
      std::cout << "  " << to_string(category.kind) << " domain=" << to_string(category.domain)
                << " total-joules=" << category.total.joules()
                << " keys=" << category.authoritative_keys << "\n";
    }
  }

  auto report = ledger.value().reconcile(query);
  if (!report) {
    return example::fail("reconcile", report.error());
  }
  std::cout << "measured residual-joules=" << report.value().measured_residual.joules() << "\n";
  std::cout << "commitment residual-joules=" << report.value().commitment_residual.joules() << "\n";
  std::cout << "complete=" << (report.value().complete ? "true" : "false") << "\n";

  auto verify = ledger.value().verify();
  if (!verify) {
    return example::fail("verify", verify.error());
  }
  std::cout << "verified-entries=" << verify.value().entries_checked
            << " ok=" << (verify.value().ok ? "true" : "false") << "\n";

  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }

  OpenOptions reopen;
  reopen.mode = AccessMode::ReadWrite;
  auto reopened = Ledger::open(path, reopen);
  if (!reopened) {
    return example::fail("reopen", reopened.error());
  }
  auto summary = reopened.value().replay();
  if (!summary) {
    return example::fail("replay", summary.error());
  }
  std::cout << "reopened head=" << summary.value().head_sequence.value()
            << " entries=" << summary.value().entries_replayed
            << " chain-verified=" << (summary.value().chain_verified ? "true" : "false") << "\n";

  auto entry = reopened.value().inspect(SequenceNumber(1));
  if (!entry) {
    return example::fail("inspect", entry.error());
  }
  std::cout << "entry 1 event-id=" << entry.value().event_id.to_hex()
            << " content-digest=" << entry.value().content_digest.to_hex() << "\n";

  closed = reopened.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }

  OpenOptions read_only;
  read_only.mode = AccessMode::ReadOnly;
  auto audit = audit_store(path);
  if (!audit) {
    return example::fail("audit", audit.error());
  }
  std::cout << "audit generation=" << audit.value().generation.value()
            << " segments=" << audit.value().segments.size()
            << " residue-bytes=" << audit.value().residue_bytes << "\n";
  return 0;
}
