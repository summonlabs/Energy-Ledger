// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Crash recovery: uncommitted bytes left at the tail of the active segment by
// an interrupted append are recognised as residue, never adopted, and the
// committed prefix replays exactly. The residue is reported by the store audit
// and discarded by the next writer session.

#include <cstdio>
#include <string>
#include <vector>

#include "example_support.hpp"
#include "energy_ledger/test_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

}  // namespace

int main() {
  const std::string path = "example-06-store";
  std::remove((path + "/LEDGER.STORE").c_str());

  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  if (!ledger) {
    return example::fail("create", ledger.error());
  }

  EntrySpec delivered;
  delivered.kind = EntryKind::Delivered;
  delivered.value = 100;
  auto committed = example::append(ledger.value(), delivered);
  if (!committed) {
    return example::fail("append", committed.error());
  }
  const SequenceNumber head_before = ledger.value().head_sequence();
  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }

  // Simulate an append that died after writing part of a record but before the
  // manifest commit point: the bytes are durable, the entry is not committed.
  auto segments = test_support::list_segment_files(path);
  if (!segments) {
    return example::fail("list segments", segments.error());
  }
  const std::string active = path + "/SEGMENTS/" + segments.value().back();
  std::vector<std::uint8_t> residue;
  residue.push_back('E');
  residue.push_back('L');
  residue.push_back('R');
  residue.push_back('1');
  for (int index = 0; index < 40; ++index) {
    residue.push_back(static_cast<std::uint8_t>(0x5A));
  }
  auto injected = test_support::append_file_bytes(active, residue);
  if (!injected) {
    return example::fail("inject residue", injected.error());
  }

  auto audit_before = audit_store(path);
  if (!audit_before) {
    return example::fail("audit", audit_before.error());
  }
  std::cout << "residue-bytes-before-reopen=" << audit_before.value().residue_bytes << "\n";
  std::cout << "head-before-reopen=" << audit_before.value().head_sequence.value() << "\n";

  OpenOptions reopen;
  reopen.mode = AccessMode::ReadWrite;
  reopen.discard_staging_residue = true;
  auto recovered = Ledger::open(path, reopen);
  if (!recovered) {
    return example::fail("reopen", recovered.error());
  }
  auto summary = recovered.value().replay();
  if (!summary) {
    return example::fail("replay", summary.error());
  }
  std::cout << "entries-replayed=" << summary.value().entries_replayed
            << " head=" << summary.value().head_sequence.value() << "\n";
  if (!(summary.value().head_sequence == head_before)) {
    std::cerr << "recovery adopted uncommitted bytes\n";
    return 3;
  }
  if (summary.value().entries_replayed != 1) {
    std::cerr << "recovery adopted a record that was never committed\n";
    return 3;
  }
  auto own_audit = recovered.value().audit();
  if (!own_audit) {
    return example::fail("audit", own_audit.error());
  }
  std::cout << "residue-bytes-discarded=" << own_audit.value().residue_bytes << "\n";

  // The ledger remains writable and the chain continues from the committed head.
  EntrySpec follow_up = delivered;
  follow_up.value = 250;
  auto next = example::append(recovered.value(), follow_up);
  if (!next) {
    return example::fail("append after recovery", next.error());
  }
  std::cout << "post-recovery sequence=" << next.value().sequence.value() << "\n";

  auto verified = recovered.value().verify();
  if (!verified) {
    return example::fail("verify", verified.error());
  }
  std::cout << "ok=" << (verified.value().ok ? "true" : "false")
            << " entries-checked=" << verified.value().entries_checked << "\n";

  closed = recovered.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }
  return 0;
}
