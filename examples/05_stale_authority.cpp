// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Failure paths: an append planned against a moved head is refused, a second
// writer is refused while writer authority is held, and a writer whose lease
// was taken over refuses to publish.

#include <cstdio>
#include <string>

#include "example_support.hpp"
#include "energy_ledger/test_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

}  // namespace

int main() {
  const std::string path = "example-05-store";
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
  auto content = example::build(delivered);
  if (!content) {
    return example::fail("build", content.error());
  }

  AppendRequest stale;
  stale.content = content.value();
  stale.has_expected_head = true;
  stale.expected_head = SequenceNumber(41);
  auto refused = ledger.value().append(stale);
  if (refused) {
    std::cerr << "expected the stale append to be refused\n";
    return 3;
  }
  std::cout << "stale head refused with " << to_string(refused.error().code) << ": "
            << refused.error().detail << "\n";

  AppendRequest good;
  good.content = content.value();
  good.has_expected_head = true;
  good.expected_head = ledger.value().head_sequence();
  auto accepted = ledger.value().append(good);
  if (!accepted) {
    return example::fail("append", accepted.error());
  }
  std::cout << "append committed at sequence=" << accepted.value().sequence.value() << "\n";

  // Re-sending the accepted content with a now-stale head is a retry of an
  // accepted attempt: it returns the prior accepted result, never a duplicate
  // entry and never a stale-authority rejection.
  auto replay = ledger.value().append(good);
  if (!replay) {
    return example::fail("replay", replay.error());
  }
  std::cout << "stale head replay disposition=" << to_string(replay.value().disposition)
            << " sequence=" << replay.value().sequence.value() << "\n";

  // Different content planned against the stale head is refused.
  EntrySpec other = delivered;
  other.value = 250;
  auto other_content = example::build(other);
  if (!other_content) {
    return example::fail("build", other_content.error());
  }
  AppendRequest stale_other;
  stale_other.content = other_content.value();
  stale_other.has_expected_head = true;
  stale_other.expected_head = SequenceNumber(0);
  auto refused_again = ledger.value().append(stale_other);
  if (refused_again) {
    std::cerr << "expected the stale append to be refused\n";
    return 3;
  }
  std::cout << "stale append refused with " << to_string(refused_again.error().code) << "\n";

  // A second writer cannot acquire authority while this session holds it.
  OpenOptions writer;
  writer.mode = AccessMode::ReadWrite;
  auto second = Ledger::open(path, writer);
  if (second) {
    std::cerr << "expected the second writer to be refused\n";
    return 3;
  }
  std::cout << "second writer refused with " << to_string(second.error().code) << "\n";

  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }

  // Epoch handoff: a lease with a higher epoch is adopted, and its epoch is
  // strictly greater than the one it replaced.
  auto epoch_before = test_support::read_lease_epoch(path);
  if (!epoch_before) {
    return example::fail("read lease epoch", epoch_before.error());
  }
  auto forced = test_support::force_lease_epoch(path, WriterEpoch(epoch_before.value() + 7));
  if (!forced) {
    return example::fail("force lease epoch", forced.error());
  }
  OpenOptions reopen;
  reopen.mode = AccessMode::ReadWrite;
  auto adopted = Ledger::open(path, reopen);
  if (!adopted) {
    return example::fail("open after takeover", adopted.error());
  }
  std::cout << "adopted writer epoch=" << adopted.value().writer_epoch().value() << "\n";
  auto closed_adopted = adopted.value().close();
  if (!closed_adopted) {
    return example::fail("close", closed_adopted.error());
  }

  // A lease that is older than the generation already published by a higher
  // epoch is refused: the store cannot be opened from behind a newer writer.
  auto downgraded = test_support::force_lease_epoch(path, WriterEpoch(0));
  if (!downgraded) {
    return example::fail("force lease epoch", downgraded.error());
  }
  auto refused_open = Ledger::open(path, reopen);
  if (refused_open) {
    std::cerr << "expected the stale lease to be refused\n";
    return 3;
  }
  std::cout << "stale lease refused with " << to_string(refused_open.error().code) << "\n";
  return 0;
}
