// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A lost response is retried with the same request id. The retry returns the
// prior accepted result instead of appending a second entry, and it does so
// before any now-stale authority check can reject it.

#include <cstdio>
#include <string>

#include "example_support.hpp"

namespace {

using namespace energy_ledger;
using example::EntrySpec;

}  // namespace

int main() {
  const std::string path = "example-04-store";
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

  auto content = example::build(delivered);
  if (!content) {
    return example::fail("build", content.error());
  }
  AppendRequest first;
  first.content = content.value();
  first.request_id = "req-0001";
  first.has_expected_head = true;
  first.expected_head = ledger.value().head_sequence();
  auto accepted = ledger.value().append(first);
  if (!accepted) {
    return example::fail("append", accepted.error());
  }
  std::cout << "first disposition=" << to_string(accepted.value().disposition)
            << " sequence=" << accepted.value().sequence.value() << "\n";

  // The client never saw the response and retries with a now-stale head.
  AppendRequest retry = first;
  retry.has_expected_head = true;
  retry.expected_head = SequenceNumber(0);
  retry.attempt = AttemptNumber(2);
  auto replayed = ledger.value().append(retry);
  if (!replayed) {
    return example::fail("retry", replayed.error());
  }
  std::cout << "retry disposition=" << to_string(replayed.value().disposition)
            << " sequence=" << replayed.value().sequence.value()
            << " same-digest=" << (replayed.value().content_digest == accepted.value().content_digest
                                       ? "true"
                                       : "false")
            << "\n";
  if (!replayed.value().replayed()) {
    std::cerr << "the retry was not recognised as a replay\n";
    return 3;
  }

  // The same content under a different request id is still the same event.
  AppendRequest second_id = first;
  second_id.request_id = "req-0002";
  second_id.has_expected_head = false;
  auto by_identity = ledger.value().append(second_id);
  if (!by_identity) {
    return example::fail("identity replay", by_identity.error());
  }
  std::cout << "identity disposition=" << to_string(by_identity.value().disposition)
            << " sequence=" << by_identity.value().sequence.value() << "\n";

  // A request id reused for different content is refused.
  EntrySpec other = delivered;
  other.value = 2000;
  auto other_content = example::build(other);
  if (!other_content) {
    return example::fail("build", other_content.error());
  }
  AppendRequest conflict;
  conflict.content = other_content.value();
  conflict.request_id = "req-0001";
  auto conflicted = ledger.value().append(conflict);
  if (conflicted) {
    std::cerr << "expected a request id conflict\n";
    return 3;
  }
  std::cout << "conflicting reuse refused with " << to_string(conflicted.error().code) << "\n";

  std::cout << "head-sequence=" << ledger.value().head_sequence().value() << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return example::fail("close", closed.error());
  }
  return 0;
}
