// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Process authority and crash-semantics proof obligations. Every case in this
// suite starts real child processes; nothing is simulated in-process.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "energy_ledger/energy_ledger.hpp"
#include "energy_ledger/test_support.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"
#include "support/util.hpp"

namespace {

using namespace energy_ledger;

#ifdef _WIN32
constexpr int kExpectedCrashCode = 0xC0DE;
#else
constexpr int kExpectedCrashCode = 0xC0DE & 0xFF;
#endif

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

std::string record_json(const std::string& interval, std::int64_t kwh,
                        const std::string& instance = "meter-1") {
  return std::string("{\"kind\":\"delivered\",\"object\":\"facility/line-1\","
                     "\"generation\":\"gen-1\",\"interval\":\"") +
         interval +
         "\",\"clock-domain\":\"utc\",\"time-basis\":\"utc-unix-seconds\"," +
         "\"interval-start\":1767225600,\"interval-end\":1767229200," +
         "\"observed-at\":1767229200,\"quantity\":" + std::to_string(kwh) +
         ",\"unit\":\"kWh\",\"quality\":\"verified\",\"authority\":\"metering-service\"," +
         "\"authority-tier\":3,\"source-family\":\"revenue-meter\",\"source-instance\":\"" +
         instance +
         "\",\"source-revision\":1,\"source-generation\":1,\"source-epoch\":1}";
}

std::string correction_json(const std::string& interval, std::int64_t kwh,
                            std::uint64_t target) {
  return std::string("{\"kind\":\"correction\",\"object\":\"facility/line-1\","
                     "\"generation\":\"gen-1\",\"interval\":\"") +
         interval +
         "\",\"clock-domain\":\"utc\",\"time-basis\":\"utc-unix-seconds\"," +
         "\"interval-start\":1767225600,\"interval-end\":1767229200," +
         "\"observed-at\":1767229200,\"quantity\":" + std::to_string(kwh) +
         ",\"unit\":\"kWh\",\"quality\":\"verified\",\"authority\":\"metering-service\"," +
         "\"authority-tier\":3,\"source-family\":\"revenue-meter\",\"source-instance\":\"meter-1\"," +
         "\"source-revision\":2,\"source-generation\":1,\"source-epoch\":1,\"target\":" +
         std::to_string(target) + "}";
}

std::string write_file(const std::string& name, const std::string& text) {
  const std::string target = eltest::scratch_root() + "/" + name;
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  auto written = test_support::write_file_bytes(target, bytes);
  (void)written;
  return target;
}

std::string write_record(const std::string& name, const std::string& interval, std::int64_t kwh,
                         const std::string& instance = "meter-1") {
  const std::string path = eltest::scratch_root() + "/" + name + ".json";
  const std::string text = record_json(interval, kwh, instance);
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  auto written = test_support::write_file_bytes(path, bytes);
  (void)written;
  return path;
}

/// Creates a store and commits the requested number of entries through the
/// library, then closes it.
bool seed_store(const std::string& path, std::size_t entries) {
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  if (!ledger) {
    return false;
  }
  for (std::size_t index = 0; index < entries; ++index) {
    AppendRequest request;
    request.content = eltest::sample_entry(EntryKind::Delivered, static_cast<std::int64_t>(10 + index),
                                           "seed-" + std::to_string(index), "meter-1");
    if (!ledger.value().append(request).has_value()) {
      return false;
    }
  }
  return ledger.value().close().has_value();
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

EL_TEST(writer_authority_excludes_other_processes) {
  const std::string path = eltest::fresh_store("mp-exclusion");
  EL_REQUIRE(seed_store(path, 1));

  eltest::ChildProcess holder;
  EL_REQUIRE(holder.spawn(eltest::cli_path(), "__hold-writer " + quoted(path)));
  EL_CHECK(starts_with(holder.read_line(), "ACQUIRED"));

  eltest::ChildProcess probe;
  EL_REQUIRE(probe.spawn(eltest::cli_path(), "__try-open " + quoted(path)));
  EL_CHECK_EQ(probe.read_line(), std::string("BUSY"));
  EL_CHECK_EQ(probe.wait(), 3);

  eltest::ChildProcess second_probe;
  EL_REQUIRE(second_probe.spawn(eltest::cli_path(), "__try-open " + quoted(path)));
  EL_CHECK_EQ(second_probe.read_line(), std::string("BUSY"));
  EL_CHECK_EQ(second_probe.wait(), 3);

  holder.write_line("release");
  EL_CHECK_EQ(holder.wait(), 0);

  eltest::ChildProcess after;
  EL_REQUIRE(after.spawn(eltest::cli_path(), "__try-open " + quoted(path)));
  EL_CHECK_EQ(after.read_line(), std::string("ACQUIRED"));
  EL_CHECK_EQ(after.wait(), 0);
}

EL_TEST(writer_authority_is_released_by_process_death) {
  const std::string path = eltest::fresh_store("mp-death");
  EL_REQUIRE(seed_store(path, 1));

  eltest::ChildProcess victim;
  EL_REQUIRE(victim.spawn(eltest::cli_path(), "__hold-and-die " + quoted(path)));
  EL_CHECK(starts_with(victim.read_line(), "ACQUIRED"));
  const std::uint64_t victim_id = victim.process_id();
  EL_CHECK(victim_id != 0);

  // Force the process to end without any cleanup path running.
  victim.terminate();
  const int status = victim.wait();
  EL_CHECK(status != 0);

  eltest::ChildProcess after;
  EL_REQUIRE(after.spawn(eltest::cli_path(), "__try-open " + quoted(path)));
  EL_CHECK_EQ(after.read_line(), std::string("ACQUIRED"));
  EL_CHECK_EQ(after.wait(), 0);

  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(report.value().ok);
}

EL_TEST(lease_epoch_handoff_and_stale_lease_refusal) {
  const std::string path = eltest::fresh_store("mp-fenced");
  EL_REQUIRE(seed_store(path, 1));
  const std::string record = write_record("mp-fenced-record", "fenced-interval", 42);

  // A live writer session publishes normally.
  eltest::ChildProcess holder;
  EL_REQUIRE(holder.spawn(eltest::cli_path(),
                          "__hold-writer " + quoted(path) + " --record " + quoted(record)));
  EL_CHECK(starts_with(holder.read_line(), "ACQUIRED"));
  holder.write_line("append");
  const std::string appended = holder.read_line();
  EL_CHECK_MSG(starts_with(appended, "APPEND-OK sequence=2"),
               "expected the held writer to append, saw: '" + appended + "'");
  EL_CHECK_EQ(holder.wait(), 0);
  auto after_append = audit_store(path);
  EL_REQUIRE(after_append.has_value());
  EL_CHECK_EQ(after_append.value().head_sequence.value(), std::uint64_t{2});

  // Epoch handoff: a lease with a higher epoch is adopted, never reused.
  auto epoch = test_support::read_lease_epoch(path);
  EL_REQUIRE(epoch.has_value());
  auto forced = test_support::force_lease_epoch(path, WriterEpoch(epoch.value() + 11));
  EL_REQUIRE(forced.has_value());
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto adopted = Ledger::open(path, options);
  EL_REQUIRE(adopted.has_value());
  EL_CHECK_EQ(adopted.value().writer_epoch().value(), epoch.value() + 12);
  auto closed = adopted.value().close();
  EL_CHECK(closed.has_value());

  // The same store is now used to prove the live fence. A session holds the
  // lease; an external writer that ignores the lock cannot modify the locked
  // bytes, and if the attempt damages the lease anyway the session must refuse
  // to publish rather than publish under dubious authority.
  const std::string second_record = write_record("mp-fenced-record-2", "fenced-interval-2", 43);
  eltest::ChildProcess fenced;
  EL_REQUIRE(fenced.spawn(eltest::cli_path(),
                          "__hold-writer " + quoted(path) + " --record " + quoted(second_record)));
  EL_CHECK(starts_with(fenced.read_line(), "ACQUIRED"));
  const std::uint64_t head_before_fence = 2;
  // A session holds the lease, so an outside writer cannot write the locked
  // range through the operating-system lock.
  auto rogue = test_support::force_lease_epoch(path, WriterEpoch(4242));
  EL_CHECK_MSG(!rogue.has_value(),
               "the operating-system lock must block writes to the locked lease range");
  fenced.write_line("append");
  const std::string fenced_result = fenced.read_line();
  const bool published = starts_with(fenced_result, "APPEND-OK");
  const bool refused_for_authority =
      starts_with(fenced_result, "APPEND-REFUSED code=stale-writer");
  EL_CHECK_MSG(published || refused_for_authority,
               "the fenced session must either publish or refuse on authority, saw: '" +
                   fenced_result + "'");
  EL_CHECK_EQ(fenced.wait(), published ? 0 : 5);

  // Nothing is published under dubious authority: the head never advances
  // beyond the entry the session legitimately appended.
  auto after_fence = audit_store(path);
  EL_REQUIRE(after_fence.has_value());
  const std::uint64_t head_after_fence = after_fence.value().head_sequence.value();
  EL_CHECK(head_after_fence == head_before_fence || head_after_fence == head_before_fence + 1);
  if (refused_for_authority) {
    EL_CHECK_EQ(head_after_fence, head_before_fence);
  }

  // A lease older than the generation the store already published is refused:
  // the store never reopens from behind a newer writer.
  auto downgraded = test_support::force_lease_epoch(path, WriterEpoch(0));
  EL_REQUIRE(downgraded.has_value());
  auto refused = Ledger::open(path, options);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::StaleWriter);

  auto before_damage = audit_store(path);
  EL_REQUIRE(before_damage.has_value());
  const std::uint64_t generation_before = before_damage.value().generation.value();

  // A destroyed lease is refused as well, and explicit administrative recovery
  // re-establishes authority without lowering the epoch and without rewinding
  // the committed generation.
  auto destroyed = test_support::truncate_file_to(path + "/WRITER.LEASE", 0);
  EL_REQUIRE(destroyed.has_value());
  auto refused_destroyed = Ledger::open(path, options);
  EL_REQUIRE(!refused_destroyed.has_value());
  EL_CHECK_EQ(refused_destroyed.error().code, StatusCode::StaleWriter);

  options.recover_lease = true;
  auto recovered = Ledger::open(path, options);
  EL_REQUIRE(recovered.has_value());
  EL_CHECK_EQ(recovered.value().head_sequence().value(), head_after_fence);
  EL_CHECK(recovered.value().writer_epoch().value() > before_damage.value().writer_epoch.value());
  auto recovered_audit = recovered.value().audit();
  EL_REQUIRE(recovered_audit.has_value());
  EL_CHECK(recovered_audit.value().lease_recovered);
  EL_CHECK_EQ(recovered_audit.value().generation.value(), generation_before);
  auto closed_recovered = recovered.value().close();
  EL_CHECK(closed_recovered.has_value());

  // Recovery is not needed again once the lease has been re-established.
  options.recover_lease = false;
  auto normal = Ledger::open(path, options);
  EL_REQUIRE(normal.has_value());
  auto normal_audit = normal.value().audit();
  EL_REQUIRE(normal_audit.has_value());
  EL_CHECK(!normal_audit.value().lease_recovered);
  auto closed_normal = normal.value().close();
  EL_CHECK(closed_normal.has_value());
}

EL_TEST(crash_stage_matrix_never_adopts_a_partial_generation) {
  for (int stage = 1; stage <= 9; ++stage) {
    // A fresh store for every stage, so each crash starts from exactly two
    // committed entries.
    const std::string path = eltest::fresh_store("mp-crash-stage-" + std::to_string(stage));
    EL_REQUIRE(seed_store(path, 2));
    const std::string record =
        write_record("crash-stage-" + std::to_string(stage), "crash-interval-" + std::to_string(stage),
                     25 + stage);
    eltest::ChildProcess child;
    EL_REQUIRE(child.spawn(eltest::cli_path(),
                           "__crash-append " + quoted(path) + " --record " + quoted(record) +
                               " --crash-stage " + std::to_string(stage)));
    const int status = child.wait();
    EL_CHECK_EQ(status, kExpectedCrashCode);

    // Whatever happened, the store must adopt exactly one whole generation and
    // must verify completely.
    VerifyOptions verify_options;
    verify_options.deep = true;
    auto report = verify_store(path, verify_options);
    EL_REQUIRE(report.has_value());
    EL_CHECK_MSG(report.value().ok, "stage " + std::to_string(stage) + " left an unverifiable store");

    OpenOptions options;
    options.mode = AccessMode::ReadWrite;
    auto ledger = Ledger::open(path, options);
    EL_REQUIRE(ledger.has_value());
    auto summary = ledger.value().replay();
    EL_REQUIRE(summary.has_value());
    const std::uint64_t head = ledger.value().head_sequence().value();

    // Stages 1..5 die before the manifest slot is flushed: the entry is not
    // committed. Stages 7..9 die at or after the commit point: it is. Stage 6
    // dies between the slot write and its flush, where the durable outcome is
    // genuinely undetermined; both outcomes are acceptable, anything else is
    // not.
    if (stage <= 5) {
      EL_CHECK_MSG(head == 2, "stage " + std::to_string(stage) + " committed an unpublished entry");
    } else if (stage >= 7) {
      EL_CHECK_MSG(head == 3, "stage " + std::to_string(stage) + " lost a committed entry");
    } else {
      EL_CHECK(head == 2 || head == 3);
    }
    if (head == 3) {
      auto entry = ledger.value().inspect(SequenceNumber(3));
      EL_CHECK(entry.has_value());
    }
    EL_CHECK_EQ(summary.value().entries_replayed, head);
    auto audit = ledger.value().audit();
    EL_REQUIRE(audit.has_value());
    EL_CHECK_EQ(audit.value().orphan_segments, std::uint64_t{0});
    auto closed = ledger.value().close();
    EL_CHECK(closed.has_value());

    // The store still accepts new work after an interrupted append.
    auto reopened = Ledger::open(path, options);
    EL_REQUIRE(reopened.has_value());
    AppendRequest request;
    request.content =
        eltest::sample_entry(EntryKind::Delivered, 77, "after-crash-" + std::to_string(stage),
                             "meter-1");
    auto appended = reopened.value().append(request);
    EL_CHECK(appended.has_value());
    auto closed_after = reopened.value().close();
    EL_CHECK(closed_after.has_value());
  }
}

EL_TEST(crash_during_rotation_leaves_no_orphan_or_partial_state) {
  for (int stage = 1; stage <= 9; ++stage) {
    const std::string path = eltest::fresh_store("mp-crash-rotate-" + std::to_string(stage));
    StoreLimits limits;
    limits.max_segment_bytes = 4096;
    CreateOptions create;
    create.fail_if_exists = false;
    create.limits = limits;
    auto ledger = Ledger::create(path, create);
    EL_REQUIRE(ledger.has_value());
    for (int index = 0; index < 13; ++index) {
      AppendRequest request;
      request.content = eltest::sample_entry(EntryKind::Delivered, 10,
                                             "rotate-" + std::to_string(index), "meter-1");
      EL_REQUIRE(ledger.value().append(request).has_value());
    }
    auto closed = ledger.value().close();
    EL_REQUIRE(closed.has_value());
    const std::string record =
        write_record("rotate-stage-" + std::to_string(stage), "rotate-extra-" + std::to_string(stage),
                     5 + stage);
    eltest::ChildProcess child;
    EL_REQUIRE(child.spawn(eltest::cli_path(),
                           "__crash-append " + quoted(path) + " --record " + quoted(record) +
                               " --crash-stage " + std::to_string(stage)));
    EL_CHECK_EQ(child.wait(), kExpectedCrashCode);

    VerifyOptions verify_options;
    verify_options.deep = true;
    auto report = verify_store(path, verify_options);
    EL_REQUIRE(report.has_value());
    EL_CHECK(report.value().ok);

    OpenOptions options;
    options.mode = AccessMode::ReadWrite;
    auto reopened = Ledger::open(path, options);
    EL_REQUIRE(reopened.has_value());
    auto audit = reopened.value().audit();
    EL_REQUIRE(audit.has_value());
    EL_CHECK_EQ(audit.value().orphan_segments, std::uint64_t{0});
    // Uncommitted bytes are reported and discarded; committed bytes are intact.
    auto verified = reopened.value().verify();
    EL_REQUIRE(verified.has_value());
    EL_CHECK(verified.value().ok);
    auto closed_again = reopened.value().close();
    EL_CHECK(closed_again.has_value());
  }
}

EL_TEST(read_only_verification_is_available_while_a_writer_is_active) {
  const std::string path = eltest::fresh_store("mp-verify-during-write");
  EL_REQUIRE(seed_store(path, 3));

  eltest::ChildProcess holder;
  EL_REQUIRE(holder.spawn(eltest::cli_path(), "__hold-writer " + quoted(path)));
  EL_CHECK(starts_with(holder.read_line(), "ACQUIRED"));

  VerifyOptions options;
  options.deep = true;
  auto report = verify_store(path, options);
  EL_REQUIRE(report.has_value());
  EL_CHECK(report.value().ok);
  EL_CHECK_EQ(report.value().entries_checked, std::uint64_t{3});

  auto audit = audit_store(path);
  EL_REQUIRE(audit.has_value());
  EL_CHECK(audit.value().writer_lease_held);
  EL_CHECK(audit.value().writer_lease_present);
  EL_CHECK_EQ(audit.value().head_sequence.value(), std::uint64_t{3});

  holder.write_line("release");
  EL_CHECK_EQ(holder.wait(), 0);

  auto after = audit_store(path);
  EL_REQUIRE(after.has_value());
  EL_CHECK(!after.value().writer_lease_held);
}

EL_TEST(cli_round_trip_against_real_files) {
  const std::string path = eltest::fresh_store("cli-roundtrip");
  eltest::remove_tree(path);

  std::string output;
  EL_CHECK_EQ(eltest::run_cli({"init", path}, &output), 0);
  EL_CHECK(output.find("initialized store=") != std::string::npos);

  const std::string record = write_record("cli-record", "cli-interval", 123);
  EL_CHECK_EQ(eltest::run_cli({"append", path, "--record", record}, &output), 0);
  EL_CHECK(output.find("disposition=committed") != std::string::npos);
  EL_CHECK(output.find("sequence=1") != std::string::npos);

  // Replaying the same record through the CLI is reported as a replay.
  EL_CHECK_EQ(eltest::run_cli({"append", path, "--record", record}, &output), 0);
  EL_CHECK(output.find("disposition=replayed-by-event-id") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"verify", path, "--json"}, &output), 0);
  EL_CHECK(output.find("\"ok\": true") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"rollup", path, "--object", "facility/line-1"}, &output), 0);
  EL_CHECK(output.find("delivered") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"reconcile", path, "--object", "facility/line-1"}, &output), 0);
  EL_CHECK(output.find("measured-flow") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"audit", path}, &output), 0);
  EL_CHECK(output.find("generation=") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"inspect", path, "--sequence", "1"}, &output), 0);
  EL_CHECK(output.find("content-digest") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"provenance", path, "--sequence", "1"}, &output), 0);
  EL_CHECK(output.find("relation=self") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"history", path, "--object", "facility/line-1"}, &output), 0);
  EL_CHECK(output.find("total-matching=1") != std::string::npos);

  // A correction preserves the original entry and is applied to the total.
  const std::string correction = write_file(
      "cli-correction.json", correction_json("cli-interval", -10, 1));
  EL_CHECK_EQ(eltest::run_cli({"correct", path, "--target", "1", "--record", correction}, &output),
              0);
  EL_CHECK(output.find("mode=correction target=1") != std::string::npos);
  // The original entry is unchanged at 123 kWh; the correction is applied to
  // the accounting key, giving 113 kWh.
  EL_CHECK_EQ(eltest::run_cli({"inspect", path, "--sequence", "1", "--json"}, &output), 0);
  EL_CHECK(output.find("\"quantity-joules\": 442800000") != std::string::npos);
  EL_CHECK_EQ(eltest::run_cli({"reconcile", path, "--object", "facility/line-1", "--json"},
                              &output),
              0);
  EL_CHECK(output.find("\"delivered-joules\": 406800000") != std::string::npos);

  // A supersession replaces the value and leaves the displaced entry readable.
  const std::string supersession = write_file(
      "cli-supersession.json", correction_json("cli-interval", 150, 1));
  EL_CHECK_EQ(eltest::run_cli({"correct", path, "--target", "1", "--mode", "supersession",
                               "--record", supersession},
                              &output),
              0);
  EL_CHECK_EQ(eltest::run_cli({"inspect", path, "--sequence", "1", "--json"}, &output), 0);
  EL_CHECK(output.find("\"resolution\": \"superseded\"") != std::string::npos);
  // The supersession replaces the displaced value (150 kWh) and the correction
  // that targets the same key still applies, giving 140 kWh.
  EL_CHECK_EQ(eltest::run_cli({"reconcile", path, "--object", "facility/line-1", "--json"},
                              &output),
              0);
  EL_CHECK(output.find("\"delivered-joules\": 504000000") != std::string::npos);

  // Bulk import of two more records.
  std::string lines;
  for (int index = 0; index < 2; ++index) {
    lines += record_json("cli-import-" + std::to_string(index), 5 + index, "meter-2");
    lines += "\n";
  }
  const std::string jsonl = write_file("cli-import.jsonl", lines);
  EL_CHECK_EQ(eltest::run_cli({"import", path, "--records", jsonl}, &output), 0);
  EL_CHECK(output.find("accepted=2 replayed=0") != std::string::npos);

  // Re-importing the same file replays both records.
  EL_CHECK_EQ(eltest::run_cli({"import", path, "--records", jsonl}, &output), 0);
  EL_CHECK(output.find("accepted=0 replayed=2") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"format", "--json"}, &output), 0);
  EL_CHECK(output.find("\"library-version\": \"1.0.0\"") != std::string::npos);

  EL_CHECK_EQ(eltest::run_cli({"open", path, "--read-only", "--json"}, &output), 0);
  EL_CHECK(output.find("\"mode\": \"read-only\"") != std::string::npos);

  // A verification failure must be reported with the documented exit code.
  auto segments = test_support::list_segment_files(path);
  EL_REQUIRE(segments.has_value());
  const std::string active = path + "/SEGMENTS/" + segments.value().back();
  auto flipped = test_support::flip_bit(active, 400, 0);
  EL_REQUIRE(flipped.has_value());
  EL_CHECK_EQ(eltest::run_cli({"verify", path}, &output), 4);
  EL_CHECK(output.find("ok=false") != std::string::npos);
  auto restored = test_support::flip_bit(active, 400, 0);
  EL_REQUIRE(restored.has_value());
  EL_CHECK_EQ(eltest::run_cli({"verify", path}, &output), 0);
}
