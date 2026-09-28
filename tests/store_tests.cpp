// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence proof obligations: framing, commit points, recovery, corruption,
// truncation, reordering, duplication, store swap, rollback and retention.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "energy_ledger/energy_ledger.hpp"
#include "energy_ledger/test_support.hpp"
#include "os_file.hpp"
#include "store.hpp"
#include "support/harness.hpp"
#include "support/util.hpp"

namespace {

using namespace energy_ledger;
using namespace energy_ledger::detail;

std::string layout_descriptor(const std::string& path) { return path + "/LEDGER.STORE"; }
std::string layout_manifest_a(const std::string& path) { return path + "/MANIFEST.A"; }
std::string layout_manifest_b(const std::string& path) { return path + "/MANIFEST.B"; }
std::string layout_staging(const std::string& path) { return path + "/MANIFEST.STG"; }
std::string layout_lease(const std::string& path) { return path + "/WRITER.LEASE"; }

std::vector<std::string> store_files(const std::string& path) {
  std::vector<std::string> files;
  files.push_back(layout_descriptor(path));
  files.push_back(layout_manifest_a(path));
  files.push_back(layout_manifest_b(path));
  files.push_back(layout_lease(path));
  auto segments = test_support::list_segment_files(path);
  if (segments) {
    for (const std::string& name : segments.value()) {
      files.push_back(path + "/SEGMENTS/" + name);
    }
  }
  return files;
}

/// Builds a store with the requested number of committed entries.
bool build_store(const std::string& path, std::size_t entries, StoreLimits limits,
                 std::uint64_t* committed_bytes = nullptr) {
  CreateOptions create;
  create.fail_if_exists = false;
  create.limits = limits;
  auto ledger = Ledger::create(path, create);
  if (!ledger) {
    return false;
  }
  for (std::size_t index = 0; index < entries; ++index) {
    AppendRequest request;
    request.content =
        eltest::sample_entry(EntryKind::Delivered, static_cast<std::int64_t>(100 + index),
                             "interval-" + std::to_string(index), "meter-1");
    auto result = ledger.value().append(request);
    if (!result) {
      return false;
    }
  }
  if (committed_bytes != nullptr) {
    auto audit = ledger.value().audit();
    if (!audit) {
      return false;
    }
    std::uint64_t total = 0;
    for (const SegmentAudit& segment : audit.value().segments) {
      total += segment.committed_bytes;
    }
    *committed_bytes = total;
  }
  auto closed = ledger.value().close();
  return closed.has_value();
}

/// Prints why a store is considered unhealthy. Used only when a check fails.
void diagnose(const std::string& path) {
  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  if (!report) {
    std::cout << "    diagnose: verify error " << report.error().describe() << "\n";
  } else {
    std::cout << "    diagnose: verify ok=" << (report.value().ok ? "true" : "false")
              << " entries=" << report.value().entries_checked
              << " generation=" << report.value().generation.value() << "\n";
    for (const IntegrityFinding& finding : report.value().findings) {
      std::cout << "      finding " << finding.area << " " << to_string(finding.code) << ": "
                << finding.detail << "\n";
    }
  }
  OpenOptions open;
  open.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(path, open);
  if (!ledger) {
    std::cout << "    diagnose: open error " << ledger.error().describe() << "\n";
  } else {
    std::cout << "    diagnose: open ok head=" << ledger.value().head_sequence().value() << "\n";
    auto closed = ledger.value().close();
    (void)closed;
  }
}

/// True when a mutation of the store is refused or reported.
bool mutation_detected(const std::string& path) {
  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  const bool verify_ok = report.has_value() && report.value().ok;

  OpenOptions open;
  open.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(path, open);
  const bool open_ok = ledger.has_value();
  if (ledger.has_value()) {
    auto closed = ledger.value().close();
    (void)closed;
  }
  if (verify_ok && open_ok) {
    return false;
  }
  return true;
}

}  // namespace

EL_TEST(create_close_reopen_preserves_committed_state) {
  const std::string path = eltest::fresh_store("reopen");
  std::uint64_t bytes = 0;
  EL_REQUIRE(build_store(path, 5, StoreLimits{}, &bytes));
  EL_CHECK(bytes > 0);

  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(path, options);
  EL_REQUIRE(ledger.has_value());
  EL_CHECK_EQ(ledger.value().head_sequence().value(), std::uint64_t{5});
  auto summary = ledger.value().replay();
  EL_REQUIRE(summary.has_value());
  EL_CHECK_EQ(summary.value().entries_replayed, std::uint64_t{5});
  for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
    auto view = ledger.value().inspect(SequenceNumber(sequence));
    EL_REQUIRE(view.has_value());
    EL_CHECK_EQ(view.value().sequence.value(), sequence);
    EL_CHECK(view.value().content.object.value() == "facility/line-1");
  }
  auto entry = ledger.value().inspect(SequenceNumber(3));
  EL_REQUIRE(entry.has_value());
  EL_CHECK(entry.value().prev_chain_hash ==
           ledger.value().inspect(SequenceNumber(2)).value().chain_hash);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
  EL_CHECK(!ledger.value().is_open());
}

EL_TEST(idempotent_replay_appends_nothing) {
  const std::string path = eltest::fresh_store("idempotent");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  AppendRequest request;
  request.content = eltest::sample_entry(EntryKind::Delivered, 10, "interval-a", "meter-1");
  request.request_id = "request-1";
  auto first = ledger.value().append(request);
  EL_REQUIRE(first.has_value());
  EL_CHECK_EQ(first.value().disposition, AppendDisposition::Committed);

  for (int repeat = 0; repeat < 5; ++repeat) {
    auto replay = ledger.value().append(request);
    EL_REQUIRE(replay.has_value());
    EL_CHECK_EQ(replay.value().disposition, AppendDisposition::ReplayedByRequestId);
    EL_CHECK_EQ(replay.value().sequence.value(), first.value().sequence.value());
    EL_CHECK(replay.value().content_digest == first.value().content_digest);
  }

  AppendRequest other_id = request;
  other_id.request_id = "request-2";
  auto by_identity = ledger.value().append(other_id);
  EL_REQUIRE(by_identity.has_value());
  EL_CHECK_EQ(by_identity.value().disposition, AppendDisposition::ReplayedByEventId);
  EL_CHECK_EQ(by_identity.value().sequence.value(), first.value().sequence.value());

  EL_CHECK_EQ(ledger.value().head_sequence().value(), std::uint64_t{1});
  auto audit = ledger.value().audit();
  EL_REQUIRE(audit.has_value());
  EL_CHECK_EQ(audit.value().entry_count, std::uint64_t{1});

  // A request id reused for different content is refused.
  AppendRequest conflicting = request;
  conflicting.content = eltest::sample_entry(EntryKind::Delivered, 11, "interval-a", "meter-1");
  auto refused = ledger.value().append(conflicting);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::RequestIdConflict);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(stale_authority_is_refused_but_replay_succeeds) {
  const std::string path = eltest::fresh_store("stale-authority");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  AppendRequest request;
  request.content = eltest::sample_entry(EntryKind::Delivered, 10, "interval-a", "meter-1");
  request.request_id = "request-1";
  request.has_expected_head = true;
  request.expected_head = SequenceNumber(0);
  auto accepted = ledger.value().append(request);
  EL_REQUIRE(accepted.has_value());

  // The same request is now stale but must still replay.
  auto replay = ledger.value().append(request);
  EL_REQUIRE(replay.has_value());
  EL_CHECK(replay.value().replayed());

  // A different request planned against the old head is refused.
  AppendRequest stale;
  stale.content = eltest::sample_entry(EntryKind::Delivered, 20, "interval-b", "meter-1");
  stale.has_expected_head = true;
  stale.expected_head = SequenceNumber(0);
  auto refused = ledger.value().append(stale);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::StaleAuthority);

  stale.has_expected_head = false;
  stale.has_expected_incarnation = true;
  stale.expected_incarnation = generate_ledger_incarnation();
  auto wrong_incarnation = ledger.value().append(stale);
  EL_REQUIRE(!wrong_incarnation.has_value());
  EL_CHECK_EQ(wrong_incarnation.error().code, StatusCode::IncarnationMismatch);

  stale.has_expected_incarnation = false;
  stale.has_expected_generation = true;
  stale.expected_generation = ManifestGeneration(999);
  auto wrong_generation = ledger.value().append(stale);
  EL_REQUIRE(!wrong_generation.has_value());
  EL_CHECK_EQ(wrong_generation.error().code, StatusCode::StaleAuthority);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(lease_fence_predicate_is_exact) {
  const StoreUuid store = generate_store_uuid();
  LeaseRecord lease;
  lease.store = store;
  lease.epoch = 4;
  lease.writer = derive_writer_id(store, WriterEpoch(4));
  EL_CHECK(lease_authorizes(lease, lease.writer, WriterEpoch(4)));
  EL_CHECK(!lease_authorizes(lease, lease.writer, WriterEpoch(5)));
  EL_CHECK(!lease_authorizes(lease, derive_writer_id(store, WriterEpoch(5)), WriterEpoch(4)));
  EL_CHECK(!lease_authorizes(lease, derive_writer_id(store, WriterEpoch(3)), WriterEpoch(4)));
  // Writer identity is a pure function of store identity and epoch, so a
  // replayed lease is detected rather than trusted.
  EL_CHECK(derive_writer_id(store, WriterEpoch(4)) == lease.writer);
  EL_CHECK(!(derive_writer_id(store, WriterEpoch(5)) == lease.writer));
  EL_CHECK(!(derive_writer_id(generate_store_uuid(), WriterEpoch(4)) == lease.writer));
}

EL_TEST(single_writer_authority_in_process) {
  const std::string path = eltest::fresh_store("busy");
  CreateOptions create;
  create.fail_if_exists = false;
  auto first = Ledger::create(path, create);
  EL_REQUIRE(first.has_value());
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto second = Ledger::open(path, options);
  EL_REQUIRE(!second.has_value());
  EL_CHECK_EQ(second.error().code, StatusCode::StoreBusy);
  options.mode = AccessMode::ReadOnly;
  auto reader = Ledger::open(path, options);
  EL_REQUIRE(!reader.has_value());
  EL_CHECK_EQ(reader.error().code, StatusCode::StoreBusy);
  auto closed = first.value().close();
  EL_CHECK(closed.has_value());
  auto after = Ledger::open(path, options);
  EL_CHECK(after.has_value());
  auto closed_after = after.value().close();
  EL_CHECK(closed_after.has_value());
}

EL_TEST(every_single_bit_mutation_is_detected) {
  const std::string path = eltest::fresh_store("bitflip");
  EL_REQUIRE(build_store(path, 2, StoreLimits{}));
  const std::vector<std::string> files = store_files(path);
  std::uint64_t mutations = 0;
  std::uint64_t undetected = 0;
  for (const std::string& file : files) {
    auto size = test_support::file_size(file);
    EL_REQUIRE(size.has_value());
    for (std::uint64_t offset = 0; offset < size.value(); ++offset) {
      for (std::uint8_t bit = 0; bit < 8; ++bit) {
        auto flipped = test_support::flip_bit(file, offset, bit);
        EL_REQUIRE(flipped.has_value());
        ++mutations;
        if (!mutation_detected(path)) {
          ++undetected;
          if (undetected < 5) {
            std::cout << "  undetected mutation in " << file << " at offset " << offset
                      << " bit " << static_cast<int>(bit) << "\n";
          }
        }
        auto restored = test_support::flip_bit(file, offset, bit);
        EL_REQUIRE(restored.has_value());
      }
    }
  }
  EL_CHECK(mutations > 2000);
  EL_CHECK_EQ(undetected, std::uint64_t{0});
  EL_CHECK(mutation_detected(path) == false);
}

EL_TEST(truncation_at_every_byte_is_detected) {
  const std::string path = eltest::fresh_store("truncate");
  EL_REQUIRE(build_store(path, 2, StoreLimits{}));

  const std::vector<std::string> targets = {layout_manifest_a(path),
                                            path + "/SEGMENTS/" +
                                                test_support::list_segment_files(path).value().back()};
  for (const std::string& target : targets) {
    auto original = test_support::read_file_bytes(target);
    EL_REQUIRE(original.has_value());
    for (std::uint64_t length = 0; length < original.value().size(); ++length) {
      auto truncated = test_support::truncate_file_to(target, length);
      EL_REQUIRE(truncated.has_value());
      EL_CHECK_MSG(mutation_detected(path),
                   "truncation of " + target + " to " + std::to_string(length) + " must be detected");
    }
    auto restored = test_support::write_file_bytes(target, original.value());
    EL_REQUIRE(restored.has_value());
    EL_CHECK(!mutation_detected(path));
  }
}

EL_TEST(rollback_to_an_older_generation_is_refused) {
  const std::string path = eltest::fresh_store("rollback");
  EL_REQUIRE(build_store(path, 4, StoreLimits{}));

  auto slot_a = test_support::read_file_bytes(layout_manifest_a(path));
  auto slot_b = test_support::read_file_bytes(layout_manifest_b(path));
  EL_REQUIRE(slot_a.has_value());
  EL_REQUIRE(slot_b.has_value());

  // Copy the older generation over the newer one: the store now looks like it
  // never committed the last entry, but the recorded floor is already higher.
  auto overwritten = test_support::write_file_bytes(layout_manifest_a(path), slot_b.value());
  EL_REQUIRE(overwritten.has_value());

  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(path, options);
  EL_REQUIRE(!ledger.has_value());
  EL_CHECK_EQ(ledger.error().code, StatusCode::StoreRollbackDetected);

  VerifyOptions verify_options;
  verify_options.deep = true;
  auto report = verify_store(path, verify_options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(!report.value().ok);

  auto restored = test_support::write_file_bytes(layout_manifest_a(path), slot_a.value());
  EL_REQUIRE(restored.has_value());
  auto reopened = Ledger::open(path, options);
  EL_CHECK(reopened.has_value());
  if (reopened.has_value()) {
    auto closed = reopened.value().close();
    EL_CHECK(closed.has_value());
  }
}

EL_TEST(missing_duplicated_and_swapped_segments_are_detected) {
  const std::string path = eltest::fresh_store("segments");
  const std::string other = eltest::fresh_store("segments-other");
  StoreLimits limits;
  limits.max_segment_bytes = 4096;
  EL_REQUIRE(build_store(path, 24, limits));
  EL_REQUIRE(build_store(other, 2, StoreLimits{}));

  auto segments = test_support::list_segment_files(path);
  EL_REQUIRE(segments.has_value());
  EL_CHECK(segments.value().size() >= 2);

  const std::string first_segment = path + "/SEGMENTS/" + segments.value().front();
  auto snapshot = test_support::read_file_bytes(first_segment);
  EL_REQUIRE(snapshot.has_value());

  // Missing segment.
  auto removed = test_support::remove_file(first_segment);
  EL_REQUIRE(removed.has_value());
  EL_CHECK(mutation_detected(path));
  auto restored = test_support::write_file_bytes(first_segment, snapshot.value());
  EL_REQUIRE(restored.has_value());
  if (mutation_detected(path)) {
    diagnose(path);
  }
  EL_CHECK(!mutation_detected(path));

  // Duplicated segment content in place of a later segment.
  const std::string second_segment = path + "/SEGMENTS/" + segments.value()[1];
  auto second_snapshot = test_support::read_file_bytes(second_segment);
  EL_REQUIRE(second_snapshot.has_value());
  auto duplicated = test_support::write_file_bytes(second_segment, snapshot.value());
  EL_REQUIRE(duplicated.has_value());
  EL_CHECK(mutation_detected(path));
  restored = test_support::write_file_bytes(second_segment, second_snapshot.value());
  EL_REQUIRE(restored.has_value());
  EL_CHECK(!mutation_detected(path));

  // A segment copied in from a different store.
  auto foreign = test_support::list_segment_files(other);
  EL_REQUIRE(foreign.has_value());
  auto foreign_bytes =
      test_support::read_file_bytes(other + "/SEGMENTS/" + foreign.value().front());
  EL_REQUIRE(foreign_bytes.has_value());
  duplicated = test_support::write_file_bytes(second_segment, foreign_bytes.value());
  EL_REQUIRE(duplicated.has_value());
  EL_CHECK(mutation_detected(path));
  restored = test_support::write_file_bytes(second_segment, second_snapshot.value());
  EL_REQUIRE(restored.has_value());
  EL_CHECK(!mutation_detected(path));
}

EL_TEST(uncommitted_residue_is_ignored_and_reported) {
  const std::string path = eltest::fresh_store("residue");
  EL_REQUIRE(build_store(path, 3, StoreLimits{}));

  auto segments = test_support::list_segment_files(path);
  EL_REQUIRE(segments.has_value());
  const std::string active = path + "/SEGMENTS/" + segments.value().back();
  std::vector<std::uint8_t> residue;
  for (int index = 0; index < 64; ++index) {
    residue.push_back(static_cast<std::uint8_t>(index));
  }
  auto injected = test_support::append_file_bytes(active, residue);
  EL_REQUIRE(injected.has_value());

  auto audit = audit_store(path);
  EL_REQUIRE(audit.has_value());
  EL_CHECK_EQ(audit.value().residue_bytes, std::uint64_t{64});
  EL_CHECK_EQ(audit.value().head_sequence.value(), std::uint64_t{3});
  // Verification reports the residue but the committed prefix is intact.
  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(report.value().ok);
  EL_CHECK_EQ(report.value().entries_checked, std::uint64_t{3});

  OpenOptions open;
  open.mode = AccessMode::ReadWrite;
  open.discard_staging_residue = true;
  auto ledger = Ledger::open(path, open);
  EL_REQUIRE(ledger.has_value());
  EL_CHECK_EQ(ledger.value().head_sequence().value(), std::uint64_t{3});
  auto own_audit = ledger.value().audit();
  EL_REQUIRE(own_audit.has_value());
  EL_CHECK_EQ(own_audit.value().residue_bytes, std::uint64_t{64});
  auto size = test_support::file_size(active);
  EL_REQUIRE(size.has_value());
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());

  // The residue is gone after the writer session cleaned it up.
  auto after = audit_store(path);
  EL_REQUIRE(after.has_value());
  EL_CHECK_EQ(after.value().residue_bytes, std::uint64_t{0});

  // A read-only open never truncates and still reports the residue.
  auto residue_again = test_support::append_file_bytes(active, residue);
  EL_REQUIRE(residue_again.has_value());
  OpenOptions read_only;
  read_only.mode = AccessMode::ReadOnly;
  auto reader = Ledger::open(path, read_only);
  EL_REQUIRE(reader.has_value());
  auto reader_audit = reader.value().audit();
  EL_REQUIRE(reader_audit.has_value());
  EL_CHECK_EQ(reader_audit.value().residue_bytes, std::uint64_t{64});
  auto closed_reader = reader.value().close();
  EL_CHECK(closed_reader.has_value());
  auto still_there = test_support::file_size(active);
  EL_REQUIRE(still_there.has_value());
  EL_CHECK_EQ(still_there.value(), size.value() + 64);
}

EL_TEST(staging_residue_is_cleaned_by_a_writer_session) {
  const std::string path = eltest::fresh_store("staging-residue");
  EL_REQUIRE(build_store(path, 1, StoreLimits{}));
  std::vector<std::uint8_t> junk(200, 0x11);
  auto written = test_support::write_file_bytes(layout_staging(path), junk);
  EL_REQUIRE(written.has_value());
  EL_CHECK(test_support::file_exists(layout_staging(path)).value());

  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  options.discard_staging_residue = true;
  auto ledger = Ledger::open(path, options);
  EL_REQUIRE(ledger.has_value());
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
  EL_CHECK(!test_support::file_exists(layout_staging(path)).value());
}

EL_TEST(repeated_open_close_cycles_are_stable) {
  const std::string path = eltest::fresh_store("cycles");
  EL_REQUIRE(build_store(path, 3, StoreLimits{}));
  std::uint64_t previous_epoch = 0;
  for (int cycle = 0; cycle < 20; ++cycle) {
    OpenOptions options;
    options.mode = AccessMode::ReadWrite;
    auto ledger = Ledger::open(path, options);
    EL_REQUIRE(ledger.has_value());
    EL_CHECK_EQ(ledger.value().head_sequence().value(), std::uint64_t{3});
    EL_CHECK_EQ(ledger.value().generation().value(), std::uint64_t{4});
    const std::uint64_t session_epoch = ledger.value().writer_epoch().value();
    EL_CHECK(session_epoch > previous_epoch);
    previous_epoch = session_epoch;
    auto closed = ledger.value().close();
    EL_CHECK(closed.has_value());
    // The persisted lease records the session epoch that just ended.
    auto persisted = test_support::read_lease_epoch(path);
    EL_REQUIRE(persisted.has_value());
    EL_CHECK_EQ(persisted.value(), session_epoch);
  }
  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(report.value().ok);
}

EL_TEST(segment_rotation_and_retention_preserve_verifiable_semantics) {
  const std::string path = eltest::fresh_store("rotation");
  StoreLimits limits;
  limits.max_segment_bytes = 4096;
  EL_REQUIRE(build_store(path, 40, limits));

  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(path, options);
  if (!ledger) {
    diagnose(path);
  }
  EL_REQUIRE(ledger.has_value());
  auto before = ledger.value().audit();
  EL_REQUIRE(before.has_value());
  EL_CHECK(before.value().segments.size() >= 3);
  for (const SegmentAudit& segment : before.value().segments) {
    if (segment.sealed) {
      EL_CHECK_MSG(segment.digest_verified,
                   "sealed segment " + std::to_string(segment.id) + " digest did not verify");
    }
  }

  CompactionOptions compaction;
  compaction.retain_from = SequenceNumber(21);
  compaction.keep_segments = 1;
  auto result = ledger.value().compact(compaction);
  EL_REQUIRE(result.has_value());
  EL_CHECK(result.value().segments_retired >= 1);
  EL_CHECK(result.value().bytes_reclaimed > 0);
  EL_CHECK(result.value().floor_sequence.value() >= 1);

  auto retired_head = ledger.value().inspect(SequenceNumber(1));
  EL_REQUIRE(!retired_head.has_value());
  EL_CHECK_EQ(retired_head.error().code, StatusCode::EntryRetired);
  auto retained = ledger.value().inspect(ledger.value().head_sequence());
  EL_CHECK(retained.has_value());

  auto verified = ledger.value().verify();
  EL_REQUIRE(verified.has_value());
  if (!verified.value().ok) {
    for (const IntegrityFinding& finding : verified.value().findings) {
      std::cout << "    verify finding " << finding.area << " " << to_string(finding.code) << ": "
                << finding.detail << "\n";
    }
    diagnose(path);
  }
  EL_CHECK(verified.value().ok);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());

  auto reopened = Ledger::open(path, options);
  if (!reopened) {
    diagnose(path);
  }
  EL_REQUIRE(reopened.has_value());
  auto summary = reopened.value().replay();
  EL_REQUIRE(summary.has_value());
  EL_CHECK_EQ(summary.value().entries_replayed, std::uint64_t{40} - result.value().floor_sequence.value());
  EL_CHECK_EQ(summary.value().retired_entries, result.value().floor_sequence.value());
  auto recount = reopened.value().verify();
  EL_REQUIRE(recount.has_value());
  EL_CHECK(recount.value().ok);
  auto closed_again = reopened.value().close();
  EL_CHECK(closed_again.has_value());
}

EL_TEST(crafted_structures_are_rejected) {
  const std::string path = eltest::fresh_store("crafted");
  EL_REQUIRE(build_store(path, 1, StoreLimits{}));
  auto store_uuid = test_support::read_store_uuid(path);
  EL_REQUIRE(store_uuid.has_value());
  auto descriptor_bytes = test_support::read_file_bytes(layout_descriptor(path));
  EL_REQUIRE(descriptor_bytes.has_value());

  // Descriptor: version, endian marker and identity mutations.
  {
    std::vector<std::uint8_t> mutated = descriptor_bytes.value();
    mutated[8] = 9;
    std::string failure;
    auto decoded = decode_descriptor(mutated.data(), mutated.size(), &failure);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreFormatUnsupported);

    mutated = descriptor_bytes.value();
    mutated[16] = 0xAA;
    decoded = decode_descriptor(mutated.data(), mutated.size(), &failure);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreEndianMismatch);

    mutated = descriptor_bytes.value();
    mutated[24] ^= 0xFF;
    decoded = decode_descriptor(mutated.data(), mutated.size(), &failure);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::DigestMismatch);

    // A truncated descriptor is refused.
    decoded = decode_descriptor(descriptor_bytes.value().data(), 32, &failure);
    EL_REQUIRE(!decoded.has_value());
  }

  // Manifest: version, endian, oversized segment table, wrong store identity.
  Manifest manifest;
  manifest.generation = 7;
  manifest.incarnation = generate_ledger_incarnation();
  manifest.head_sequence = 0;
  manifest.entry_count = 0;
  manifest.writer_epoch = 1;
  manifest.active_segment_id = 1;
  manifest.active_first_sequence = 1;
  manifest.active_segment_bytes = 64;
  manifest.active_segment_entries = 0;
  auto encoded = encode_manifest(store_uuid.value(), manifest);
  EL_REQUIRE(encoded.has_value());
  EL_CHECK(decode_manifest(encoded.value().data(), encoded.value().size(), store_uuid.value(),
                           nullptr)
               .has_value());
  {
    std::vector<std::uint8_t> mutated = encoded.value();
    mutated[8] = 2;
    auto decoded = decode_manifest(mutated.data(), mutated.size(), store_uuid.value(), nullptr);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreFormatUnsupported);

    mutated = encoded.value();
    mutated[16] = 0x00;
    mutated[17] = 0x00;
    decoded = decode_manifest(mutated.data(), mutated.size(), store_uuid.value(), nullptr);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreEndianMismatch);

    mutated = encoded.value();
    // segment_count lives at offset 168 in the manifest header.
    for (int index = 0; index < 8; ++index) {
      mutated[168 + index] = 0xFF;
    }
    decoded = decode_manifest(mutated.data(), mutated.size(), store_uuid.value(), nullptr);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK(decoded.error().code == StatusCode::LimitExceeded ||
             decoded.error().code == StatusCode::StoreTruncated);

    auto other_store = generate_store_uuid();
    decoded = decode_manifest(encoded.value().data(), encoded.value().size(), other_store, nullptr);
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreSwapped);
  }

  // Records: magic, declared length and CRC.
  {
    auto content = eltest::sample_entry(EntryKind::Delivered, 10, "interval-a", "meter-1");
    auto bytes = encode_content(content);
    EL_REQUIRE(bytes.has_value());
    const Digest256 entry_hash = compute_entry_hash(1, bytes.value());
    const Digest256 chain = compute_chain_hash(Digest256(), entry_hash);
    std::vector<std::uint8_t> frame = encode_record(1, Digest256(), entry_hash, bytes.value());
    auto file_path = path + "/CRAFTED.BIN";
    auto written = test_support::write_file_bytes(file_path, frame);
    EL_REQUIRE(written.has_value());
    auto handle = open_file(file_path, OpenMode::ReadWriteExisting);
    EL_REQUIRE(handle.has_value());
    auto record = read_record_at(handle.value(), 0, kMaxRecordBytes);
    EL_REQUIRE(record.has_value());
    EL_CHECK_EQ(record.value().sequence, std::uint64_t{1});
    EL_CHECK(record.value().entry_hash == entry_hash);
    EL_CHECK_EQ(chain.storage().size(), std::size_t{32});

    std::vector<std::uint8_t> bad_magic = frame;
    bad_magic[0] = 'X';
    written = test_support::write_file_bytes(file_path, bad_magic);
    EL_REQUIRE(written.has_value());
    handle = open_file(file_path, OpenMode::ReadOnly);
    EL_REQUIRE(handle.has_value());
    auto rejected = read_record_at(handle.value(), 0, kMaxRecordBytes);
    EL_REQUIRE(!rejected.has_value());
    EL_CHECK_EQ(rejected.error().code, StatusCode::RecordFramingInvalid);

    std::vector<std::uint8_t> bad_length = frame;
    bad_length[4] = 4;
    bad_length[5] = 0;
    bad_length[6] = 0;
    bad_length[7] = 0;
    written = test_support::write_file_bytes(file_path, bad_length);
    EL_REQUIRE(written.has_value());
    handle = open_file(file_path, OpenMode::ReadOnly);
    EL_REQUIRE(handle.has_value());
    rejected = read_record_at(handle.value(), 0, kMaxRecordBytes);
    EL_REQUIRE(!rejected.has_value());
    EL_CHECK_EQ(rejected.error().code, StatusCode::RecordFramingInvalid);

    std::vector<std::uint8_t> bad_crc = frame;
    bad_crc[8] ^= 0xFF;
    written = test_support::write_file_bytes(file_path, bad_crc);
    EL_REQUIRE(written.has_value());
    handle = open_file(file_path, OpenMode::ReadOnly);
    EL_REQUIRE(handle.has_value());
    rejected = read_record_at(handle.value(), 0, kMaxRecordBytes);
    EL_REQUIRE(!rejected.has_value());
    EL_CHECK_EQ(rejected.error().code, StatusCode::CrcMismatch);

    handle.value().close();
    auto removed = test_support::remove_file(file_path);
    EL_CHECK(removed.has_value());
  }
}

EL_TEST(path_and_layout_validation) {
  const std::string root = eltest::scratch_root();
  CreateOptions create;
  create.fail_if_exists = false;

  auto traversal = Ledger::create(root + "/../escape-attempt", create);
  EL_REQUIRE(!traversal.has_value());
  EL_CHECK_EQ(traversal.error().code, StatusCode::StorePathUnsafe);

  auto device = Ledger::create(root + "/NUL", create);
  EL_REQUIRE(!device.has_value());
  EL_CHECK_EQ(device.error().code, StatusCode::StorePathUnsafe);

  auto reserved = Ledger::create(root + "/bad<name>", create);
  EL_REQUIRE(!reserved.has_value());
  EL_CHECK_EQ(reserved.error().code, StatusCode::StorePathUnsafe);

  auto control = Ledger::create(root + "/bad\x01name", create);
  EL_REQUIRE(!control.has_value());
  EL_CHECK_EQ(control.error().code, StatusCode::StorePathUnsafe);

  auto overlong = Ledger::create(root + "/" + std::string(600, 'a'), create);
  EL_REQUIRE(!overlong.has_value());
  EL_CHECK_EQ(overlong.error().code, StatusCode::StorePathTooLong);

  auto empty = Ledger::create("", create);
  EL_REQUIRE(!empty.has_value());
  EL_CHECK_EQ(empty.error().code, StatusCode::StorePathUnsafe);

  // A file where a store directory is required.
  const std::string file_path = root + "/not-a-directory";
  std::vector<std::uint8_t> junk(8, 0);
  auto written = test_support::write_file_bytes(file_path, junk);
  EL_REQUIRE(written.has_value());
  auto not_a_directory = Ledger::create(file_path, create);
  EL_REQUIRE(!not_a_directory.has_value());
  EL_CHECK_EQ(not_a_directory.error().code, StatusCode::StorePathNotDirectory);
  auto removed = test_support::remove_file(file_path);
  EL_CHECK(removed.has_value());

  // A directory where a store file is required.
  const std::string awkward = eltest::fresh_store("awkward");
  EL_REQUIRE(build_store(awkward, 1, StoreLimits{}));
  // Replace the older generation slot with a directory: the newest generation
  // stays usable, so the store still opens but verification reports the slot.
  auto renamed = test_support::remove_file(layout_manifest_a(awkward));
  EL_CHECK(renamed.has_value());
  auto directory = test_support::create_directory(layout_manifest_a(awkward));
  EL_REQUIRE(directory.has_value());
  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(awkward, options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(!report.value().ok);
  // The current generation is still intact, so a read-only open succeeds and
  // reports the unusable slot through the audit.
  OpenOptions read_only;
  read_only.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(awkward, read_only);
  if (!ledger.has_value()) {
    diagnose(awkward);
  }
  EL_CHECK(ledger.has_value());
  if (ledger.has_value()) {
    auto audit = ledger.value().audit();
    EL_REQUIRE(audit.has_value());
    EL_CHECK_EQ(audit.value().slots.size(), std::size_t{2});
    auto closed = ledger.value().close();
    EL_CHECK(closed.has_value());
  }
}

EL_TEST(reparse_point_store_root_is_refused) {
  const std::string root = eltest::scratch_root();
  const std::string target = root + "/junction-target";
  const std::string link = root + "/junction-link";
  eltest::remove_tree(target);
  eltest::remove_tree(link);
  std::system(("cmd /c rmdir \"" + link + "\" > NUL 2>&1").c_str());
  auto created = test_support::create_directory(target);
  if (!created) {
    std::cout << "  note: could not create the junction target; reparse test not exercised\n";
    return;
  }
  const int status =
      std::system(("cmd /c mklink /J \"" + link + "\" \"" + target + "\" > NUL 2>&1").c_str());
  if (status != 0) {
    std::cout << "  note: this environment cannot create a junction; reparse test not exercised\n";
    return;
  }
  CreateOptions create;
  create.fail_if_exists = false;
  auto rejected = Ledger::create(link, create);
  EL_REQUIRE(!rejected.has_value());
  EL_CHECK_EQ(rejected.error().code, StatusCode::StoreReparsePointRejected);
  OpenOptions open;
  open.mode = AccessMode::ReadWrite;
  auto opened = Ledger::open(link, open);
  EL_REQUIRE(!opened.has_value());
  EL_CHECK_EQ(opened.error().code, StatusCode::StoreReparsePointRejected);
  std::system(("cmd /c rmdir \"" + link + "\"").c_str());
  eltest::remove_tree(target);
}

EL_TEST(limits_are_enforced_before_allocation) {
  const std::string path = eltest::fresh_store("limits");
  StoreLimits limits;
  limits.max_entries = 1;
  CreateOptions create;
  create.fail_if_exists = false;
  create.limits = limits;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  auto first = ledger.value().append(
      [] {
        AppendRequest request;
        request.content = eltest::sample_entry(EntryKind::Delivered, 1, "interval-a", "meter-1");
        return request;
      }());
  EL_CHECK(first.has_value());
  AppendRequest second;
  second.content = eltest::sample_entry(EntryKind::Delivered, 2, "interval-b", "meter-1");
  auto refused = ledger.value().append(second);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::LimitExceeded);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());

  // Invalid limit configurations are refused outright.
  StoreLimits bad;
  bad.max_entries = 0;
  auto invalid = Ledger::create(eltest::fresh_store("limits-bad"), CreateOptions{bad, false});
  EL_REQUIRE(!invalid.has_value());
  EL_CHECK_EQ(invalid.error().code, StatusCode::LimitExceeded);

  StoreLimits tiny_segment;
  tiny_segment.max_segment_bytes = 16;
  auto tiny = Ledger::create(eltest::fresh_store("limits-tiny"), CreateOptions{tiny_segment, false});
  EL_REQUIRE(!tiny.has_value());
  EL_CHECK_EQ(tiny.error().code, StatusCode::LimitExceeded);
}
