// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Accounting proof obligations: resolution precedence, conflict reporting,
// residual preservation, freshness recomputation, and equality against an
// independently written reference model over seeded randomized event streams.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "energy_ledger/energy_ledger.hpp"
#include "support/harness.hpp"
#include "support/util.hpp"

namespace {

using namespace energy_ledger;

struct Store {
  std::string path;
  Ledger ledger;
};

std::vector<std::string> interval_names{"i-1", "i-2", "i-3"};

EntryContent entry(EntryKind kind, std::int64_t kwh, const std::string& interval,
                   const std::string& instance, std::uint32_t tier = 3,
                   std::uint64_t revision = 1, std::uint64_t epoch = 1) {
  EntryContent content = eltest::sample_entry(kind, kwh, interval, instance);
  content.authority_tier = AuthorityTier::create(tier).value();
  content.source_revision = SourceRevision(revision);
  content.source_epoch = SourceEpoch(epoch);
  return content;
}

RollupQuery object_query() {
  RollupQuery query;
  query.object = FacilityObjectRef::parse("facility/line-1").value();
  query.options.max_contributors = 512;
  return query;
}

std::int64_t total_of(const RollupResult& rolled, EntryKind kind) {
  std::int64_t total = 0;
  for (const GenerationRollup& generation : rolled.generations) {
    for (const CategoryTotal& category : generation.categories) {
      if (category.kind == kind) {
        total += category.total.joules();
      }
    }
  }
  return total;
}

const CategoryTotal* category_of(const RollupResult& rolled, EntryKind kind) {
  for (const GenerationRollup& generation : rolled.generations) {
    for (const CategoryTotal& category : generation.categories) {
      if (category.kind == kind) {
        return &category;
      }
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Independent reference model
//
// Written from the specification rather than from the library: string composed
// keys, linear scans, plain integer sums. It is deliberately simple and
// deliberately not sharing any code with the ledger.
// ---------------------------------------------------------------------------

struct RefEntry {
  std::string object;
  std::string generation;
  std::string interval;
  std::string kind;
  std::int64_t joules = 0;
  std::uint32_t tier = 0;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  std::uint64_t sequence = 0;
  std::uint64_t target = 0;
  std::string quality = "unknown";
  std::string unit = "J";
};

std::string ref_key(const RefEntry& e) {
  return e.object + "|" + e.generation + "|" + e.interval + "|" + e.kind;
}

bool ref_is_adjustment(const std::string& kind) {
  return kind == "correction" || kind == "supersession" || kind == "compensating";
}

struct RefModel {
  std::vector<RefEntry> entries;

  void add(const RefEntry& entry) { entries.push_back(entry); }

  /// Independent recomputation of the authoritative totals per category.
  std::map<std::string, std::int64_t> totals() const {
    std::map<std::string, std::int64_t> result;
    // Resolve the effective key of every entry.
    std::vector<std::string> effective(entries.size());
    std::vector<std::size_t> effective_index(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
      std::size_t cursor = index;
      while (entries[cursor].target != 0) {
        std::size_t found = entries.size();
        for (std::size_t scan = 0; scan < entries.size(); ++scan) {
          if (entries[scan].sequence == entries[cursor].target) {
            found = scan;
            break;
          }
        }
        if (found == entries.size()) {
          break;
        }
        cursor = found;
      }
      effective_index[index] = cursor;
      effective[index] = ref_key(entries[cursor]);
    }

    // Explicit supersession: walk in sequence order.
    std::vector<bool> voided(entries.size(), false);
    std::vector<std::size_t> applied;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (entries[index].kind != "supersession") {
        continue;
      }
      std::size_t cursor = effective_index[index];
      std::uint32_t required = entries[cursor].tier;
      std::vector<std::size_t> chain{cursor};
      bool broken = false;
      while (entries[cursor].kind == "supersession") {
        required = std::max(required, entries[cursor].tier);
        std::size_t found = entries.size();
        for (std::size_t scan = 0; scan < entries.size(); ++scan) {
          if (entries[scan].sequence == entries[cursor].target) {
            found = scan;
            break;
          }
        }
        if (found == entries.size()) {
          broken = true;
          break;
        }
        cursor = found;
        required = std::max(required, entries[cursor].tier);
        chain.push_back(cursor);
      }
      if (broken || entries[index].tier < required) {
        continue;
      }
      for (std::size_t displaced : chain) {
        voided[displaced] = true;
      }
      applied.push_back(index);
    }

    // Group the competing observations.
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (ref_is_adjustment(entries[index].kind) || voided[index]) {
        continue;
      }
      groups[effective[index]].push_back(index);
    }
    for (std::size_t index : applied) {
      groups[effective[index]].push_back(index);
    }

    std::map<std::string, bool> authoritative_present;
    for (const auto& group : groups) {
      const std::vector<std::size_t>& members = group.second;
      std::uint32_t best_tier = 0;
      std::uint64_t best_epoch = 0;
      std::uint64_t best_revision = 0;
      for (std::size_t member : members) {
        const RefEntry& e = entries[member];
        if (e.tier > best_tier || (e.tier == best_tier && e.epoch > best_epoch) ||
            (e.tier == best_tier && e.epoch == best_epoch && e.revision > best_revision)) {
          best_tier = e.tier;
          best_epoch = e.epoch;
          best_revision = e.revision;
        }
      }
      std::vector<std::size_t> winners;
      for (std::size_t member : members) {
        const RefEntry& e = entries[member];
        if (e.tier == best_tier && e.epoch == best_epoch && e.revision == best_revision) {
          winners.push_back(member);
        }
      }
      bool agree = true;
      for (std::size_t member : winners) {
        if (entries[member].joules != entries[winners.front()].joules ||
            entries[member].quality != entries[winners.front()].quality ||
            entries[member].unit != entries[winners.front()].unit) {
          agree = false;
        }
      }
      if (winners.size() == 1 || agree) {
        std::size_t chosen = winners.front();
        for (std::size_t member : winners) {
          if (entries[member].sequence < entries[chosen].sequence) {
            chosen = member;
          }
        }
        result[group.first] += entries[chosen].joules;
        authoritative_present[group.first] = true;
      }
    }

    // Additive adjustments apply only where an authoritative value exists.
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const std::string& kind = entries[index].kind;
      if (!ref_is_adjustment(kind) || kind == "supersession") {
        continue;
      }
      if (voided[index]) {
        continue;
      }
      if (authoritative_present[effective[index]]) {
        result[effective[index]] += entries[index].joules;
      }
    }
    return result;
  }

  std::int64_t total_for(const std::string& kind) const {
    std::int64_t total = 0;
    for (const auto& item : totals()) {
      const std::size_t split = item.first.rfind('|');
      if (split != std::string::npos && item.first.substr(split + 1) == kind) {
        total += item.second;
      }
    }
    return total;
  }
};

}  // namespace

EL_TEST(rollup_adds_one_authoritative_value_per_key) {
  const std::string path = eltest::fresh_store("rollup-basic");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  const EntryKind kinds[] = {EntryKind::Delivered, EntryKind::Consumed, EntryKind::WastedLost,
                             EntryKind::Unclassified, EntryKind::ReservationCommit,
                             EntryKind::Curtailed};
  std::int64_t expected_total = 0;
  for (EntryKind kind : kinds) {
    const std::int64_t kwh = 100;
    AppendRequest request;
    request.content = entry(kind, kwh, "i-1", "meter-1");
    auto result = ledger.value().append(request);
    EL_REQUIRE(result.has_value());
    expected_total += 100 * 3600000;
  }
  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  for (EntryKind kind : kinds) {
    EL_CHECK_EQ(total_of(rolled.value(), kind), std::int64_t{100} * 3600000);
  }
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->authoritative_keys, std::uint64_t{1});
  EL_CHECK_EQ(delivered->domain, AccountingDomain::MeasuredFlow);
  EL_CHECK_EQ(delivered->total.joules(), expected_total / 6);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(equal_authority_conflict_is_never_resolved_arbitrarily) {
  const std::string path = eltest::fresh_store("conflict");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  for (int index = 0; index < 8; ++index) {
    AppendRequest request;
    request.content =
        entry(EntryKind::Delivered, 100 + index, "i-1", "meter-" + std::to_string(index));
    auto result = ledger.value().append(request);
    EL_REQUIRE(result.has_value());
  }
  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  // Ten competing equal-authority observations of one key: no winner, no sum.
  EL_CHECK_EQ(delivered->total.joules(), std::int64_t{0});
  EL_CHECK_EQ(delivered->conflicted_keys, std::uint64_t{1});
  EL_CHECK_EQ(delivered->conflicted_entries, std::uint64_t{8});
  EL_CHECK_EQ(delivered->conflicted_min.joules(), std::int64_t{100} * 3600000);
  EL_CHECK_EQ(delivered->conflicted_max.joules(), std::int64_t{107} * 3600000);

  auto report = ledger.value().reconcile(object_query());
  EL_REQUIRE(report.has_value());
  EL_CHECK(!report.value().complete);
  EL_CHECK_EQ(report.value().conflicted_keys, std::uint64_t{1});
  bool explained = false;
  for (const ExplanationLine& line : report.value().explanation) {
    if (line.code == "conflict") {
      explained = true;
    }
  }
  EL_CHECK(explained);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(precedence_shadowing_and_supersession) {
  const std::string path = eltest::fresh_store("precedence");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  // Baseline observation from a low authority source.
  AppendRequest low;
  low.content = entry(EntryKind::Delivered, 100, "i-1", "field-estimator", 1, 1, 1);
  auto low_result = ledger.value().append(low);
  EL_REQUIRE(low_result.has_value());

  // A higher authority source outranks it without a supersession record.
  AppendRequest high;
  high.content = entry(EntryKind::Delivered, 111, "i-1", "revenue-meter", 5, 1, 1);
  auto high_result = ledger.value().append(high);
  EL_REQUIRE(high_result.has_value());

  // The same source instance with a higher revision also loses to authority.
  AppendRequest revision;
  revision.content = entry(EntryKind::Delivered, 112, "i-1", "field-estimator", 1, 9, 1);
  auto revision_result = ledger.value().append(revision);
  EL_REQUIRE(revision_result.has_value());

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->authoritative.joules(), std::int64_t{111} * 3600000);
  EL_CHECK_EQ(delivered->shadowed_entries, std::uint64_t{2});
  EL_CHECK_EQ(delivered->shadowed.joules(), std::int64_t{212} * 3600000);

  // An explicit supersession displaces the previously authoritative entry.
  AppendRequest supersede;
  supersede.content = entry(EntryKind::Supersession, 90, "i-1", "revenue-meter", 5, 2, 1);
  supersede.content.target = SequenceNumber(2);
  auto superseded = ledger.value().append(supersede);
  EL_REQUIRE(superseded.has_value());
  auto after = ledger.value().rollup(object_query());
  EL_REQUIRE(after.has_value());
  const CategoryTotal* after_delivered = category_of(after.value(), EntryKind::Delivered);
  EL_REQUIRE(after_delivered != nullptr);
  EL_CHECK_EQ(after_delivered->authoritative.joules(), std::int64_t{90} * 3600000);

  // A weaker supersession is refused outright at append time.
  AppendRequest weak;
  weak.content = entry(EntryKind::Supersession, 1, "i-1", "field-estimator", 1, 1, 1);
  weak.content.target = SequenceNumber(2);
  auto refused = ledger.value().append(weak);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::UnauthorizedSupersession);

  // The displaced entry is still readable: history is never rewritten.
  auto original = ledger.value().inspect(SequenceNumber(2));
  EL_REQUIRE(original.has_value());
  EL_CHECK_EQ(original.value().state, ResolutionState::Superseded);
  EL_CHECK_EQ(original.value().content.quantity.joules(), std::int64_t{111} * 3600000);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(source_epoch_retires_older_observations) {
  const std::string path = eltest::fresh_store("epoch");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  AppendRequest old_epoch;
  old_epoch.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1", 3, 1, 1);
  EL_REQUIRE(ledger.value().append(old_epoch).has_value());
  AppendRequest new_epoch;
  new_epoch.content = entry(EntryKind::Delivered, 140, "i-1", "meter-1", 3, 1, 2);
  EL_REQUIRE(ledger.value().append(new_epoch).has_value());

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->authoritative.joules(), std::int64_t{140} * 3600000);
  EL_CHECK_EQ(delivered->shadowed_entries, std::uint64_t{1});
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(equal_revision_conflict_is_deterministic) {
  const std::string path = eltest::fresh_store("equal-revision");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  AppendRequest first;
  first.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1", 3, 4, 1);
  EL_REQUIRE(ledger.value().append(first).has_value());
  AppendRequest second;
  second.content = entry(EntryKind::Delivered, 250, "i-1", "meter-1", 3, 4, 1);
  EL_REQUIRE(ledger.value().append(second).has_value());

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->conflicted_keys, std::uint64_t{1});
  EL_CHECK_EQ(delivered->total.joules(), std::int64_t{0});
  // Repeating the query returns exactly the same answer.
  auto again = ledger.value().rollup(object_query());
  EL_REQUIRE(again.has_value());
  const CategoryTotal* repeated = category_of(again.value(), EntryKind::Delivered);
  EL_REQUIRE(repeated != nullptr);
  EL_CHECK_EQ(repeated->conflicted_min.joules(), delivered->conflicted_min.joules());
  EL_CHECK_EQ(repeated->conflicted_max.joules(), delivered->conflicted_max.joules());
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(identical_values_from_two_sources_agree) {
  const std::string path = eltest::fresh_store("agreement");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  AppendRequest first;
  first.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1");
  EL_REQUIRE(ledger.value().append(first).has_value());
  AppendRequest second;
  second.content = entry(EntryKind::Delivered, 100, "i-1", "meter-2");
  EL_REQUIRE(ledger.value().append(second).has_value());
  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->total.joules(), std::int64_t{100} * 3600000);
  EL_CHECK_EQ(delivered->duplicate_replicas, std::uint64_t{1});
  EL_CHECK_EQ(delivered->conflicted_keys, std::uint64_t{0});
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(corrections_are_additive_and_preserve_history) {
  const std::string path = eltest::fresh_store("corrections");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  AppendRequest base;
  base.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1");
  auto base_result = ledger.value().append(base);
  EL_REQUIRE(base_result.has_value());

  AppendRequest correction;
  correction.content = entry(EntryKind::Correction, -10, "i-1", "meter-1", 3, 2, 1);
  correction.content.target = base_result.value().sequence;
  auto correction_result = ledger.value().append(correction);
  EL_REQUIRE(correction_result.has_value());

  AppendRequest compensating;
  compensating.content = entry(EntryKind::Compensating, -5, "i-1", "meter-1", 3, 3, 1);
  compensating.content.target = base_result.value().sequence;
  EL_REQUIRE(ledger.value().append(compensating).has_value());

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->authoritative.joules(), std::int64_t{100} * 3600000);
  EL_CHECK_EQ(delivered->adjustments.joules(), std::int64_t{-15} * 3600000);
  EL_CHECK_EQ(delivered->total.joules(), std::int64_t{85} * 3600000);

  // A correction must match the key of its target.
  AppendRequest mismatched;
  mismatched.content = entry(EntryKind::Correction, -1, "i-2", "meter-1", 3, 4, 1);
  mismatched.content.target = base_result.value().sequence;
  auto refused = ledger.value().append(mismatched);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::TargetKeyMismatch);

  // A missing target is refused.
  AppendRequest missing;
  missing.content = entry(EntryKind::Correction, -1, "i-1", "meter-1", 3, 5, 1);
  missing.content.target = SequenceNumber(999);
  auto missing_result = ledger.value().append(missing);
  EL_REQUIRE(!missing_result.has_value());
  EL_CHECK_EQ(missing_result.error().code, StatusCode::TargetNotInLedger);

  auto chain = ledger.value().provenance(base_result.value().sequence);
  EL_REQUIRE(chain.has_value());
  bool saw_correction = false;
  bool saw_compensating = false;
  for (const ProvenanceNode& node : chain.value().nodes) {
    if (node.relation == "corrected-by") {
      saw_correction = true;
    }
    if (node.relation == "compensated-by") {
      saw_compensating = true;
    }
  }
  EL_CHECK(saw_correction);
  EL_CHECK(saw_compensating);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(residual_and_commitment_domains_stay_separate) {
  const std::string path = eltest::fresh_store("residual");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  struct Item {
    EntryKind kind;
    std::int64_t kwh;
    const char* instance;
  };
  const Item items[] = {
      {EntryKind::Delivered, 1000, "meter-1"},
      {EntryKind::Consumed, 800, "submeter-1"},
      {EntryKind::WastedLost, 100, "loss-model"},
      {EntryKind::ReservationCommit, 1300, "planner"},
      {EntryKind::Curtailed, 100, "planner"},
  };
  for (const Item& item : items) {
    AppendRequest request;
    request.content = entry(item.kind, item.kwh, "i-1", item.instance);
    EL_REQUIRE(ledger.value().append(request).has_value());
  }
  auto report = ledger.value().reconcile(object_query());
  EL_REQUIRE(report.has_value());
  const Reconciliation& value = report.value();
  EL_CHECK_EQ(value.delivered.joules(), std::int64_t{1000} * 3600000);
  EL_CHECK_EQ(value.measured_classified.joules(), std::int64_t{900} * 3600000);
  EL_CHECK_EQ(value.measured_residual.joules(), std::int64_t{100} * 3600000);
  EL_CHECK(!value.measured_residual_is_zero);
  EL_CHECK_EQ(value.commitment_slack.joules(), std::int64_t{300} * 3600000);
  EL_CHECK_EQ(value.commitment_residual.joules(), std::int64_t{200} * 3600000);
  EL_CHECK(value.complete);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(unknown_is_never_converted_to_zero) {
  const std::string path = eltest::fresh_store("unknown");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  // No evidence at all for the consumed category.
  AppendRequest delivered;
  delivered.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1");
  EL_REQUIRE(ledger.value().append(delivered).has_value());

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  EL_CHECK(category_of(rolled.value(), EntryKind::Consumed) == nullptr);

  // An explicitly unknown quality with a zero quantity is still evidence with
  // one authoritative key, not an absent observation.
  EntryContent zero = entry(EntryKind::Consumed, 0, "i-1", "estimator");
  zero.quality = ObservationQuality::Unknown;
  AppendRequest zero_request;
  zero_request.content = zero;
  EL_REQUIRE(ledger.value().append(zero_request).has_value());

  auto after = ledger.value().rollup(object_query());
  EL_REQUIRE(after.has_value());
  const CategoryTotal* consumed = category_of(after.value(), EntryKind::Consumed);
  EL_REQUIRE(consumed != nullptr);
  EL_CHECK_EQ(consumed->total.joules(), std::int64_t{0});
  EL_CHECK_EQ(consumed->authoritative_keys, std::uint64_t{1});
  EL_CHECK_EQ(consumed->unknown_quality_entries, std::uint64_t{1});

  auto report = ledger.value().reconcile(object_query());
  EL_REQUIRE(report.has_value());
  EL_CHECK_EQ(report.value().measured_residual.joules(), std::int64_t{100} * 3600000);
  bool noted = false;
  for (const ExplanationLine& line : report.value().explanation) {
    if (line.code == "unknown-quality") {
      noted = true;
    }
  }
  EL_CHECK(noted);
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(freshness_is_recomputed_and_never_stored) {
  const std::string path = eltest::fresh_store("freshness");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  AppendRequest request;
  request.content = entry(EntryKind::Delivered, 100, "i-1", "meter-1");
  request.content.observed_at = TimeTicks(1767229200);
  EL_REQUIRE(ledger.value().append(request).has_value());

  RollupQuery query = object_query();
  query.options.has_evaluation_time = true;
  query.options.evaluation_time = TimeTicks(1767229210);
  query.options.has_max_age = true;
  query.options.max_age = Duration::from_seconds(60).value();
  query.options.clock_domain = ClockDomainRef::parse("utc").value();
  query.options.time_basis = TimeBasis::UtcUnixSeconds;
  query.options.ticks_per_second = 1;

  auto fresh = ledger.value().rollup(query);
  EL_REQUIRE(fresh.has_value());
  EL_CHECK_EQ(category_of(fresh.value(), EntryKind::Delivered)->stale_entries, std::uint64_t{0});

  query.options.evaluation_time = TimeTicks(1767229200 + 3600);
  auto stale = ledger.value().rollup(query);
  EL_REQUIRE(stale.has_value());
  EL_CHECK_EQ(category_of(stale.value(), EntryKind::Delivered)->stale_entries, std::uint64_t{1});

  // Reopening does not make recovered evidence fresh: freshness is derived from
  // the stored observation time and the query evaluation time only.
  auto closed = ledger.value().close();
  EL_REQUIRE(closed.has_value());
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto reopened = Ledger::open(path, options);
  EL_REQUIRE(reopened.has_value());
  auto after_reopen = reopened.value().rollup(query);
  EL_REQUIRE(after_reopen.has_value());
  EL_CHECK_EQ(category_of(after_reopen.value(), EntryKind::Delivered)->stale_entries,
              std::uint64_t{1});

  // A tick based domain without a declared rate cannot be classified.
  RollupQuery tick_query = query;
  tick_query.options.time_basis = TimeBasis::MonotonicTicks;
  tick_query.options.ticks_per_second = 0;
  auto unknown = reopened.value().rollup(tick_query);
  EL_REQUIRE(unknown.has_value());
  EL_CHECK_EQ(category_of(unknown.value(), EntryKind::Delivered)->stale_entries, std::uint64_t{0});
  auto closed_again = reopened.value().close();
  EL_CHECK(closed_again.has_value());
}

EL_TEST(selectors_and_bounds_are_applied) {
  const std::string path = eltest::fresh_store("selectors");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  for (const std::string& interval : interval_names) {
    for (int generation = 1; generation <= 2; ++generation) {
      EntryContent content = entry(EntryKind::Delivered, 10 * generation, interval, "meter-1");
      content.generation = GenerationRef::parse("gen-" + std::to_string(generation)).value();
      AppendRequest request;
      request.content = content;
      EL_REQUIRE(ledger.value().append(request).has_value());
    }
  }
  RollupQuery query = object_query();
  auto rolled = ledger.value().rollup(query);
  EL_REQUIRE(rolled.has_value());
  EL_CHECK_EQ(rolled.value().generations.size(), std::size_t{2});
  EL_CHECK_EQ(rolled.value().entries_matched, std::uint64_t{6});

  query.all_generations = false;
  query.generation = GenerationRef::parse("gen-2").value();
  auto scoped = ledger.value().rollup(query);
  EL_REQUIRE(scoped.has_value());
  EL_CHECK_EQ(scoped.value().generations.size(), std::size_t{1});
  EL_CHECK_EQ(total_of(scoped.value(), EntryKind::Delivered), std::int64_t{60} * 3600000);

  query.all_intervals = false;
  query.interval = IntervalRef::parse("i-2").value();
  auto narrowed = ledger.value().rollup(query);
  EL_REQUIRE(narrowed.has_value());
  EL_CHECK_EQ(total_of(narrowed.value(), EntryKind::Delivered), std::int64_t{20} * 3600000);

  HistoryQuery history;
  history.by_object = true;
  history.object = FacilityObjectRef::parse("facility/line-1").value();
  history.limit = 2;
  auto entries = ledger.value().history(history);
  EL_REQUIRE(entries.has_value());
  EL_CHECK_EQ(entries.value().entries.size(), std::size_t{2});
  EL_CHECK_EQ(entries.value().total_matching, std::uint64_t{6});
  EL_CHECK(entries.value().truncated);

  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(aggregate_overflow_is_refused) {
  const std::string path = eltest::fresh_store("overflow");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  // Two of these overflow, one does not.
  const std::int64_t huge = std::numeric_limits<std::int64_t>::max() / 2 + 1;
  // Three distinct accounting keys: each is representable, their sum is not.
  for (int index = 0; index < 3; ++index) {
    EntryContent content = entry(EntryKind::Delivered, 0, "i-" + std::to_string(index), "meter-1");
    content.quantity = Energy::from_joules(huge).value();
    content.declared_value = huge;
    content.declared_unit = EnergyUnit::Joule;
    AppendRequest request;
    request.content = content;
    EL_REQUIRE(ledger.value().append(request).has_value());
  }
  // Aggregating the three keys overflows int64 joules: the ledger refuses and
  // reports overflow instead of wrapping or clamping.
  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(!rolled.has_value());
  EL_CHECK_EQ(rolled.error().code, StatusCode::ArithmeticOverflow);
  auto report = ledger.value().reconcile(object_query());
  EL_REQUIRE(!report.has_value());
  EL_CHECK_EQ(report.error().code, StatusCode::ArithmeticOverflow);

  // The same value observed for one key is still representable.
  RollupQuery single_key = object_query();
  single_key.all_intervals = false;
  single_key.interval = IntervalRef::parse("i-0").value();
  auto single = ledger.value().rollup(single_key);
  EL_REQUIRE(single.has_value());
  EL_CHECK_EQ(total_of(single.value(), EntryKind::Delivered), huge);

  // A correction that would push one key past the representable range is also
  // refused rather than wrapped.
  AppendRequest correction;
  correction.content = entry(EntryKind::Correction, 0, "i-1", "meter-1", 3, 2, 1);
  correction.content.target = SequenceNumber(2);
  correction.content.quantity = Energy::from_joules(huge).value();
  correction.content.declared_value = huge;
  correction.content.declared_unit = EnergyUnit::Joule;
  EL_REQUIRE(ledger.value().append(correction).has_value());
  RollupQuery adjusted = object_query();
  adjusted.all_intervals = false;
  adjusted.interval = IntervalRef::parse("i-1").value();
  auto overflowed = ledger.value().rollup(adjusted);
  EL_REQUIRE(!overflowed.has_value());
  EL_CHECK_EQ(overflowed.error().code, StatusCode::ArithmeticOverflow);

  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(independent_reference_model_matches_seeded_random_streams) {
  for (std::uint64_t seed_offset = 0; seed_offset < 6; ++seed_offset) {
    const std::uint64_t seed = eltest::base_seed() + seed_offset * 7919;
    eltest::Rng rng(seed);
    const std::string path = eltest::fresh_store("reference-" + std::to_string(seed_offset));
    CreateOptions create;
    create.fail_if_exists = false;
    auto ledger = Ledger::create(path, create);
    EL_REQUIRE(ledger.has_value());

    RefModel model;
    const EntryKind physical[] = {EntryKind::Delivered, EntryKind::Consumed, EntryKind::WastedLost,
                                  EntryKind::Unclassified};
    std::uint64_t sequence = 0;
    const int steps = 60;
    for (int step = 0; step < steps; ++step) {
      EntryKind kind = physical[rng.index(4)];
      const std::string interval = interval_names[rng.index(interval_names.size())];
      const std::string instance = "source-" + std::to_string(rng.index(3));
      const std::uint32_t tier = static_cast<std::uint32_t>(rng.range(1, 4));
      const std::uint64_t revision = static_cast<std::uint64_t>(rng.range(1, 3));
      const std::uint64_t epoch = 1;
      const std::int64_t kwh = rng.range(1, 500);

      EntryContent content = entry(kind, kwh, interval, instance, tier, revision, epoch);
      // Occasionally collide on the exact same accounting key with equal rank
      // so conflicts and shadowing are exercised, not only unique keys.
      if (rng.chance(1, 4)) {
        content = entry(kind, kwh, interval, instance, 2, 1, 1);
      }
      AppendRequest request;
      request.content = content;

      // Occasionally reuse an exact duplicate, which must replay.
      const bool duplicate = rng.chance(1, 6) && !model.entries.empty();
      if (duplicate) {
        // Rebuild an entry identical to the previous one.
        auto previous = ledger.value().inspect(SequenceNumber(model.entries.back().sequence));
        EL_REQUIRE(previous.has_value());
        request.content = previous.value().content;
      }

      auto result = ledger.value().append(request);
      EL_REQUIRE(result.has_value());
      if (result.value().replayed()) {
        continue;
      }
      ++sequence;
      EL_CHECK_EQ(result.value().sequence.value(), sequence);

      if (!duplicate) {
        RefEntry reference;
        reference.object = content.object.value();
        reference.generation = content.generation.value();
        reference.interval = content.interval.value();
        reference.kind = to_string(content.kind);
        reference.joules = content.quantity.joules();
        reference.quality = to_string(content.quality);
        reference.unit = to_string(content.declared_unit);
        reference.tier = content.authority_tier.value();
        reference.epoch = content.source_epoch.value();
        reference.revision = content.source_revision.value();
        reference.sequence = sequence;
        reference.target = content.target.value();
        model.add(reference);
      }

      // After every accepted mutation the ledger must agree with the model.
      auto rolled = ledger.value().rollup(object_query());
      EL_REQUIRE(rolled.has_value());
      for (EntryKind compare : physical) {
        const std::int64_t expected = model.total_for(to_string(compare));
        const std::int64_t actual = total_of(rolled.value(), compare);
        if (expected != actual) {
          std::cout << "  mismatch seed=" << seed << " step=" << step << " kind="
                    << to_string(compare) << " expected=" << expected << " actual=" << actual
                    << "\n";
        }
        EL_CHECK_EQ(actual, expected);
      }
    }
    auto closed = ledger.value().close();
    EL_CHECK(closed.has_value());
  }
}

EL_TEST(reference_model_covers_corrections_and_supersessions) {
  const std::string path = eltest::fresh_store("reference-adjusted");
  CreateOptions create;
  create.fail_if_exists = false;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());
  RefModel model;

  const std::uint64_t seed = eltest::base_seed() + 424242;
  eltest::Rng rng(seed);
  std::uint64_t sequence = 0;
  std::vector<std::uint64_t> bases;
  for (int step = 0; step < 24; ++step) {
    EntryKind kind = (step % 3 == 0) ? EntryKind::Consumed : EntryKind::Delivered;
    const std::string interval = interval_names[rng.index(interval_names.size())];
    const std::string instance = "source-" + std::to_string(rng.index(2));
    const std::int64_t kwh = rng.range(1, 100);
    EntryContent content = entry(kind, kwh, interval, instance, 2, 1, 1);
    AppendRequest request;
    request.content = content;
    if (!bases.empty() && rng.chance(1, 3)) {
      const std::uint64_t target = bases[rng.index(bases.size())];
      auto target_view = ledger.value().inspect(SequenceNumber(target));
      EL_REQUIRE(target_view.has_value());
      request.content = target_view.value().content;
      request.content.kind = rng.chance(1, 2) ? EntryKind::Correction : EntryKind::Compensating;
      request.content.target = SequenceNumber(target);
      request.content.declared_value = rng.chance(1, 2) ? -7 : -3;
      request.content.declared_unit = EnergyUnit::KilowattHour;
      request.content.quantity =
          to_canonical_energy(request.content.declared_value, EnergyUnit::KilowattHour).value();
    }
    auto result = ledger.value().append(request);
    EL_REQUIRE(result.has_value());
    ++sequence;
    RefEntry reference;
    reference.object = request.content.object.value();
    reference.generation = request.content.generation.value();
    reference.interval = request.content.interval.value();
    reference.kind = to_string(request.content.kind);
    reference.joules = request.content.quantity.joules();
    reference.quality = to_string(request.content.quality);
    reference.unit = to_string(request.content.declared_unit);
    reference.tier = request.content.authority_tier.value();
    reference.epoch = request.content.source_epoch.value();
    reference.revision = request.content.source_revision.value();
    reference.sequence = sequence;
    reference.target = request.content.target.value();
    model.add(reference);
    if (request.content.target.is_zero()) {
      bases.push_back(sequence);
    }
  }

  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  for (EntryKind kind : {EntryKind::Delivered, EntryKind::Consumed}) {
    EL_CHECK_EQ(total_of(rolled.value(), kind), model.total_for(to_string(kind)));
  }
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}

EL_TEST(adjustments_against_retired_targets_are_unapplied) {
  const std::string path = eltest::fresh_store("retired-adjustment");
  StoreLimits limits;
  limits.max_segment_bytes = 4096;
  CreateOptions create;
  create.fail_if_exists = false;
  create.limits = limits;
  auto ledger = Ledger::create(path, create);
  EL_REQUIRE(ledger.has_value());

  std::uint64_t first_sequence = 0;
  for (int index = 0; index < 80; ++index) {
    AppendRequest request;
    request.content = entry(EntryKind::Delivered, 10, "i-" + std::to_string(index), "meter-1");
    auto result = ledger.value().append(request);
    EL_REQUIRE(result.has_value());
    if (index == 0) {
      first_sequence = result.value().sequence.value();
    }
  }
  // A correction that targets the very first entry.
  AppendRequest correction;
  correction.content = entry(EntryKind::Correction, -1, "i-0", "meter-1", 3, 2, 1);
  correction.content.target = SequenceNumber(first_sequence);
  EL_REQUIRE(ledger.value().append(correction).has_value());

  CompactionOptions compaction;
  compaction.retain_from = SequenceNumber(61);
  compaction.keep_segments = 1;
  auto compacted = ledger.value().compact(compaction);
  EL_REQUIRE(compacted.has_value());
  EL_CHECK(compacted.value().segments_retired >= 1);
  EL_CHECK(compacted.value().floor_sequence.value() >= first_sequence);

  // A new correction against a retired target is refused explicitly.
  AppendRequest late;
  late.content = entry(EntryKind::Correction, -1, "i-0", "meter-1", 3, 3, 1);
  late.content.target = SequenceNumber(first_sequence);
  auto refused = ledger.value().append(late);
  EL_REQUIRE(!refused.has_value());
  EL_CHECK_EQ(refused.error().code, StatusCode::EntryRetired);

  // The surviving correction no longer has an authoritative base, so it is
  // reported as unapplied rather than silently added.
  auto rolled = ledger.value().rollup(object_query());
  EL_REQUIRE(rolled.has_value());
  const CategoryTotal* delivered = category_of(rolled.value(), EntryKind::Delivered);
  EL_REQUIRE(delivered != nullptr);
  EL_CHECK_EQ(delivered->adjustments.joules(), std::int64_t{0});
  auto closed = ledger.value().close();
  EL_CHECK(closed.has_value());
}
