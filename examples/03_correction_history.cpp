// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A correction preserves the original entry: nothing is rewritten, the
// correction is a new immutable entry, and provenance explains the change.

#include <cstdio>
#include <string>

#include "example_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

}  // namespace

int main() {
  const std::string path = "example-03-store";
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
  auto original = example::append(ledger.value(), delivered);
  if (!original) {
    return example::fail("append", original.error());
  }
  std::cout << "original sequence=" << original.value().sequence.value()
            << " digest=" << original.value().content_digest.to_hex() << "\n";

  // A compensating entry for a meter that over reported by 30 kWh.
  EntrySpec compensating;
  compensating.kind = EntryKind::Compensating;
  compensating.value = -30;
  compensating.target = original.value().sequence;
  compensating.source_instance = "meter-1";
  compensating.revision = 2;
  compensating.annotations.push_back(Annotation{"reason", "meter-over-report"});
  auto correction = example::append(ledger.value(), compensating);
  if (!correction) {
    return example::fail("compensating", correction.error());
  }
  std::cout << "compensating sequence=" << correction.value().sequence.value() << "\n";

  auto original_view = ledger.value().inspect(original.value().sequence);
  if (!original_view) {
    return example::fail("inspect", original_view.error());
  }
  std::cout << "original still present: digest=" << original_view.value().content_digest.to_hex()
            << " resolution=" << to_string(original_view.value().state) << "\n";

  auto chain = ledger.value().provenance(original.value().sequence);
  if (!chain) {
    return example::fail("provenance", chain.error());
  }
  for (const ProvenanceNode& node : chain.value().nodes) {
    std::cout << "provenance sequence=" << node.sequence.value() << " relation=" << node.relation
              << " state=" << to_string(node.state) << " quantity-joules=" << node.quantity.joules()
              << "\n";
  }

  RollupQuery query;
  auto object = FacilityObjectRef::parse(delivered.object);
  if (!object) {
    return example::fail("object", object.error());
  }
  query.object = object.value();
  auto report = ledger.value().reconcile(query);
  if (!report) {
    return example::fail("reconcile", report.error());
  }
  std::cout << "delivered after correction=" << report.value().delivered.joules() << " joules\n";

  // A supersession with lower authority than its target is refused outright.
  EntrySpec weak;
  weak.kind = EntryKind::Supersession;
  weak.value = 5;
  weak.target = original.value().sequence;
  weak.tier = 1;
  auto weak_result = example::append(ledger.value(), weak);
  if (weak_result) {
    std::cerr << "expected the weak supersession to be refused\n";
    return 3;
  }
  std::cout << "weak supersession refused with " << to_string(weak_result.error().code) << "\n";

  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }
  return 0;
}
