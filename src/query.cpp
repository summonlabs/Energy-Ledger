// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Query path: resolution, rollups, reconciliation, provenance, history and the
// standalone verification/audit views. Every value here is derived from
// immutable committed entries; nothing in this file mutates the store.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "byte_io.hpp"
#include "energy_ledger/canonical.hpp"
#include "ledger_internal.hpp"

namespace energy_ledger {
namespace {

using namespace detail;

struct Rank {
  std::uint8_t tier = 0;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;

  friend bool operator<(const Rank& a, const Rank& b) noexcept {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.epoch != b.epoch) return a.epoch < b.epoch;
    return a.revision < b.revision;
  }
  friend bool operator==(const Rank& a, const Rank& b) noexcept {
    return a.tier == b.tier && a.epoch == b.epoch && a.revision == b.revision;
  }
};

Rank rank_of(const IndexRecord& record) {
  return Rank{record.authority_tier, record.source_epoch, record.source_revision};
}

Expected<std::size_t> index_of_sequence(const Ledger::Impl& impl, std::uint64_t sequence) {
  const IndexRecord* record = impl.find_record(sequence);
  if (record == nullptr) {
    return make_error(StatusCode::EntryRetired, "entry is not retained");
  }
  return static_cast<std::size_t>(record - impl.records.data());
}

std::string describe_quantity(std::int64_t joules) { return std::to_string(joules) + " J"; }

}  // namespace

namespace detail {

AccountingKey key_of(const Ledger::Impl& impl, const IndexRecord& record) {
  AccountingKey key;
  key.object = FacilityObjectRef::parse(impl.text(record.object)).value_or(FacilityObjectRef());
  key.generation = GenerationRef::parse(impl.text(record.generation)).value_or(GenerationRef());
  key.interval = IntervalRef::parse(impl.text(record.interval)).value_or(IntervalRef());
  key.clock_domain =
      ClockDomainRef::parse(impl.text(record.clock_domain)).value_or(ClockDomainRef());
  key.kind = record.entry_kind();
  return key;
}

FreshnessState classify_freshness(const IndexRecord& record, const QueryOptions& options,
                                  std::string_view record_clock_domain) {
  if (!options.has_evaluation_time) {
    return FreshnessState::NotEvaluated;
  }
  if (record.time_basis == static_cast<std::uint8_t>(TimeBasis::Unspecified)) {
    return FreshnessState::UnknownTimeBasis;
  }
  if (!options.clock_domain.empty() && options.clock_domain.value() != record_clock_domain) {
    return FreshnessState::DomainMismatch;
  }
  if (options.time_basis != TimeBasis::Unspecified &&
      options.time_basis != static_cast<TimeBasis>(record.time_basis)) {
    return FreshnessState::UnknownTimeBasis;
  }
  if (record.observed_at == TimeTicks::kUnknownValue) {
    return FreshnessState::UnknownObservationTime;
  }
  if (!options.has_max_age) {
    return FreshnessState::NotEvaluated;
  }
  const std::int64_t evaluation = options.evaluation_time.value();
  std::int64_t age_ticks = 0;
  if (!checked::sub(evaluation, record.observed_at, age_ticks) || age_ticks < 0) {
    return FreshnessState::UnknownObservationTime;
  }
  // Milliseconds per tick expressed as an exact rational.
  std::int64_t numerator = 1;
  std::int64_t denominator = 1;
  switch (static_cast<TimeBasis>(record.time_basis)) {
    case TimeBasis::UtcUnixMilliseconds:
      break;
    case TimeBasis::UtcUnixSeconds:
      numerator = 1000;
      break;
    case TimeBasis::WallClockMinutes:
      numerator = 60000;
      break;
    case TimeBasis::MonotonicTicks:
    case TimeBasis::MeterRegisterTicks:
    case TimeBasis::IntervalIndex:
      if (options.ticks_per_second == 0) {
        return FreshnessState::UnknownTimeBasis;
      }
      numerator = 1000;
      denominator = static_cast<std::int64_t>(options.ticks_per_second);
      break;
    case TimeBasis::Unspecified:
      return FreshnessState::UnknownTimeBasis;
  }
  std::int64_t left = 0;
  std::int64_t right = 0;
  if (!checked::mul(age_ticks, numerator, left) ||
      !checked::mul(options.max_age.milliseconds(), denominator, right)) {
    return FreshnessState::UnknownTimeBasis;
  }
  return left > right ? FreshnessState::Stale : FreshnessState::Fresh;
}

Expected<EntryContent> load_content(const Ledger::Impl& impl, const IndexRecord& record) {
  auto file = open_file(impl.layout.segment_file(record.segment_id), OpenMode::ReadOnly);
  if (!file) {
    return file.error();
  }
  auto frame = read_record_at(file.value(), record.record_offset, kMaxRecordBytes);
  if (!frame) {
    return frame.error();
  }
  const FramedRecord& framed = frame.value();
  if (framed.sequence != record.sequence) {
    return make_error(StatusCode::SequenceReordered,
                      "stored record at the indexed offset has a different sequence");
  }
  const Digest256 recomputed = compute_entry_hash(framed.sequence, framed.content);
  if (recomputed != framed.entry_hash) {
    return make_error(StatusCode::DigestMismatch, "stored record digest does not match its hash");
  }
  if (!(recomputed == record.entry_hash)) {
    return make_error(StatusCode::DigestMismatch, "stored record digest does not match the index");
  }
  return decode_content(framed.content.data(), framed.content.size());
}

Expected<EntryView> make_entry_view(const Ledger::Impl& impl, const IndexRecord& record) {
  auto content = load_content(impl, record);
  if (!content) {
    return content.error();
  }
  EntryView view;
  view.sequence = SequenceNumber(record.sequence);
  view.incarnation = impl.manifest.incarnation;
  view.content = content.value();
  view.content_digest = record.content_digest;
  view.entry_hash = record.entry_hash;
  view.prev_chain_hash = record.prev_chain;
  view.chain_hash = record.chain_hash;
  view.event_id = record.event_id;
  view.segment_id = record.segment_id;
  view.record_offset = record.record_offset;
  view.record_length = record.record_length;
  view.committed_generation = ManifestGeneration(impl.manifest.generation);

  auto resolution = build_resolution(impl);
  if (!resolution) {
    return resolution.error();
  }
  const std::size_t position = static_cast<std::size_t>(&record - impl.records.data());
  view.state = resolution.value().states[position];
  return view;
}

Expected<ResolutionTable> build_resolution(const Ledger::Impl& impl) {
  const std::size_t count = impl.records.size();
  ResolutionTable table;
  table.states.assign(count, ResolutionState::Authoritative);
  table.effective_sequence.assign(count, 0);

  // 1. Effective target of every record: the entry that carries the value an
  //    adjustment or supersession applies to.
  for (std::size_t index = 0; index < count; ++index) {
    const IndexRecord& record = impl.records[index];
    if (record.target == 0) {
      table.effective_sequence[index] = record.sequence;
      continue;
    }
    auto target = index_of_sequence(impl, record.target);
    if (!target) {
      table.effective_sequence[index] = record.sequence;
      table.states[index] = ResolutionState::AdjustmentUnapplied;
      continue;
    }
    table.effective_sequence[index] = table.effective_sequence[target.value()];
  }

  // 2. Supersession. A supersession must be at least as authoritative as every
  //    entry it displaces; a weaker one has no effect and is reported as
  //    unauthorized instead of silently winning.
  std::vector<std::uint8_t> voided(count, 0);
  std::vector<std::uint8_t> applied(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    const IndexRecord& record = impl.records[index];
    if (record.entry_kind() != EntryKind::Supersession) {
      continue;
    }
    auto target = index_of_sequence(impl, record.target);
    if (!target) {
      table.states[index] = ResolutionState::AdjustmentUnapplied;
      continue;
    }
    std::size_t cursor = target.value();
    std::uint8_t required_tier = impl.records[cursor].authority_tier;
    std::vector<std::size_t> chain{cursor};
    bool broken = false;
    while (impl.records[cursor].entry_kind() == EntryKind::Supersession) {
      required_tier = std::max(required_tier, impl.records[cursor].authority_tier);
      auto next = index_of_sequence(impl, impl.records[cursor].target);
      if (!next) {
        broken = true;
        break;
      }
      cursor = next.value();
      required_tier = std::max(required_tier, impl.records[cursor].authority_tier);
      chain.push_back(cursor);
    }
    if (broken) {
      table.states[index] = ResolutionState::AdjustmentUnapplied;
      continue;
    }
    if (record.authority_tier < required_tier) {
      table.states[index] = ResolutionState::Unauthorized;
      voided[index] = 1;
      continue;
    }
    for (std::size_t displaced : chain) {
      table.states[displaced] = ResolutionState::Superseded;
      voided[displaced] = 1;
    }
    applied[index] = 1;
  }

  // 3. Competing observations of one accounting key.
  std::map<AccountingKey, std::vector<std::size_t>> members;
  for (std::size_t index = 0; index < count; ++index) {
    const IndexRecord& record = impl.records[index];
    if (is_adjustment(record.entry_kind())) {
      continue;
    }
    if (voided[index] != 0) {
      continue;
    }
    members[key_of(impl, record)].push_back(index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    const IndexRecord& record = impl.records[index];
    if (record.entry_kind() != EntryKind::Supersession || applied[index] == 0) {
      continue;
    }
    auto target = index_of_sequence(impl, record.target);
    if (!target) {
      continue;
    }
    std::size_t cursor = target.value();
    while (impl.records[cursor].entry_kind() == EntryKind::Supersession) {
      auto next = index_of_sequence(impl, impl.records[cursor].target);
      if (!next) {
        break;
      }
      cursor = next.value();
    }
    members[key_of(impl, impl.records[cursor])].push_back(index);
  }

  std::map<AccountingKey, std::vector<std::size_t>> conflicted_keys;
  for (auto& entry : members) {
    std::vector<std::size_t>& group = entry.second;
    Rank best = rank_of(impl.records[group.front()]);
    for (std::size_t member : group) {
      const Rank rank = rank_of(impl.records[member]);
      if (best < rank) {
        best = rank;
      }
    }
    std::vector<std::size_t> winners;
    for (std::size_t member : group) {
      if (rank_of(impl.records[member]) == best) {
        winners.push_back(member);
      }
    }
    bool agree = true;
    const IndexRecord& first = impl.records[winners.front()];
    for (std::size_t member : winners) {
      const IndexRecord& other = impl.records[member];
      if (other.quantity != first.quantity || other.quality != first.quality ||
          other.declared_unit != first.declared_unit) {
        agree = false;
        break;
      }
    }
    if (winners.size() == 1 || agree) {
      std::size_t authoritative = winners.front();
      for (std::size_t member : winners) {
        if (impl.records[member].sequence < impl.records[authoritative].sequence) {
          authoritative = member;
        }
      }
      table.states[authoritative] = ResolutionState::Authoritative;
      for (std::size_t member : winners) {
        if (member != authoritative) {
          table.states[member] = ResolutionState::DuplicateReplica;
        }
      }
    } else {
      for (std::size_t member : winners) {
        table.states[member] = ResolutionState::Conflicted;
        conflicted_keys[entry.first].push_back(member);
      }
    }
    for (std::size_t member : group) {
      if (rank_of(impl.records[member]) < best) {
        table.states[member] = ResolutionState::Shadowed;
      }
    }
  }

  // 4. Corrections and compensating entries are additive. They apply only when
  //    the key they modify has a single authoritative value.
  for (std::size_t index = 0; index < count; ++index) {
    const IndexRecord& record = impl.records[index];
    if (!is_adjustment(record.entry_kind()) || record.entry_kind() == EntryKind::Supersession) {
      continue;
    }
    if (table.states[index] == ResolutionState::Superseded) {
      continue;
    }
    const IndexRecord* root = impl.find_record(table.effective_sequence[index]);
    if (root == nullptr) {
      table.states[index] = ResolutionState::AdjustmentUnapplied;
      continue;
    }
    auto group = members.find(key_of(impl, *root));
    bool has_authoritative = false;
    if (group != members.end()) {
      for (std::size_t member : group->second) {
        if (table.states[member] == ResolutionState::Authoritative) {
          has_authoritative = true;
          break;
        }
      }
    }
    table.states[index] = has_authoritative ? ResolutionState::AdjustmentApplied
                                            : ResolutionState::AdjustmentUnapplied;
  }
  return table;
}

bool matches_selector(const Ledger::Impl& impl, const IndexRecord& record,
                      const RollupQuery& query) {
  if (impl.text(record.object) != query.object.value()) {
    return false;
  }
  if (!query.all_generations && impl.text(record.generation) != query.generation.value()) {
    return false;
  }
  if (!query.all_intervals && impl.text(record.interval) != query.interval.value()) {
    return false;
  }
  return true;
}

Expected<RollupResult> run_rollup(const Ledger::Impl& impl, const RollupQuery& query,
                                  const ResolutionTable* precomputed) {
  ResolutionTable owned;
  const ResolutionTable* table = precomputed;
  if (table == nullptr) {
    auto built = build_resolution(impl);
    if (!built) {
      return built.error();
    }
    owned = std::move(built).value();
    table = &owned;
  }

  RollupResult result;
  result.object = query.object;
  result.all_generations = query.all_generations;
  result.generation = query.generation;
  result.all_intervals = query.all_intervals;
  result.interval = query.interval;
  result.options = query.options;
  result.entries_considered = impl.records.size();

  std::map<AccountingKey, std::vector<std::size_t>> all_members;
  std::map<AccountingKey, std::vector<std::size_t>> adjustments;
  std::map<AccountingKey, std::vector<std::size_t>> conflicted;
  for (std::size_t index = 0; index < impl.records.size(); ++index) {
    const IndexRecord& record = impl.records[index];
    if (!matches_selector(impl, record, query)) {
      continue;
    }
    ++result.entries_matched;
    const ResolutionState state = table->states[index];
    const IndexRecord* root = impl.find_record(table->effective_sequence[index]);
    AccountingKey key = root != nullptr ? key_of(impl, *root) : key_of(impl, record);
    if (state == ResolutionState::Conflicted) {
      conflicted[key].push_back(index);
      continue;
    }
    if (is_adjustment(record.entry_kind()) && record.entry_kind() != EntryKind::Supersession) {
      adjustments[key].push_back(index);
      continue;
    }
    all_members[key].push_back(index);
  }

  std::map<AccountingKey, CategoryTotal> totals;
  for (const auto& entry : all_members) {
    const AccountingKey& key = entry.first;
    CategoryTotal total;
    total.kind = key.kind;
    total.domain = domain_of(key.kind);
    for (std::size_t index : entry.second) {
      const IndexRecord& record = impl.records[index];
      switch (table->states[index]) {
        case ResolutionState::Authoritative: {
          auto sum = Energy::add(total.authoritative, Energy::from_joules(record.quantity).value());
          if (!sum) {
            return sum.error();
          }
          total.authoritative = sum.value();
          total.authoritative_keys += 1;
          if (total.contributors.size() < query.options.max_contributors) {
            total.contributors.push_back(SequenceNumber(record.sequence));
          } else {
            total.contributors_truncated = true;
          }
          break;
        }
        case ResolutionState::DuplicateReplica:
          total.duplicate_replicas += 1;
          break;
        case ResolutionState::Shadowed: {
          auto sum = Energy::add(total.shadowed, Energy::from_joules(record.quantity).value());
          if (!sum) {
            return sum.error();
          }
          total.shadowed = sum.value();
          total.shadowed_entries += 1;
          break;
        }
        default:
          break;
      }
      if (classify_freshness(record, query.options, impl.text(record.clock_domain)) ==
          FreshnessState::Stale) {
        total.stale_entries += 1;
      }
      if (record.quality == static_cast<std::uint8_t>(ObservationQuality::Unknown)) {
        total.unknown_quality_entries += 1;
      }
      if (record.observed_at == TimeTicks::kUnknownValue) {
        total.unknown_time_entries += 1;
      }
    }
    totals[key] = total;
  }

  for (const auto& entry : adjustments) {
    auto found = totals.find(entry.first);
    if (found == totals.end()) {
      continue;
    }
    for (std::size_t index : entry.second) {
      if (table->states[index] != ResolutionState::AdjustmentApplied) {
        continue;
      }
      const IndexRecord& record = impl.records[index];
      auto sum = Energy::add(found->second.adjustments, Energy::from_joules(record.quantity).value());
      if (!sum) {
        return sum.error();
      }
      found->second.adjustments = sum.value();
      if (found->second.contributors.size() < query.options.max_contributors) {
        found->second.contributors.push_back(SequenceNumber(record.sequence));
      } else {
        found->second.contributors_truncated = true;
      }
    }
  }

  for (const auto& entry : conflicted) {
    auto found = totals.find(entry.first);
    if (found == totals.end()) {
      CategoryTotal fresh;
      fresh.kind = entry.first.kind;
      fresh.domain = domain_of(entry.first.kind);
      totals[entry.first] = fresh;
      found = totals.find(entry.first);
    }
    CategoryTotal& total = found->second;
    bool first = true;
    for (std::size_t index : entry.second) {
      const Energy candidate = Energy::from_joules(impl.records[index].quantity).value();
      if (first || candidate < total.conflicted_min) {
        total.conflicted_min = candidate;
      }
      if (first || candidate > total.conflicted_max) {
        total.conflicted_max = candidate;
      }
      first = false;
      total.conflicted_entries += 1;
    }
    total.conflicted_keys = 1;
  }

  for (auto& entry : totals) {
    auto sum = Energy::add(entry.second.authoritative, entry.second.adjustments);
    if (!sum) {
      return sum.error();
    }
    entry.second.total = sum.value();
  }

  // Every accounting key of a (generation, category) pair contributes to the
  // reported category total; the reported entry is the aggregate of all keys.
  std::map<std::string, std::map<EntryKind, CategoryTotal>> by_generation;
  for (const auto& entry : totals) {
    CategoryTotal& slot = by_generation[entry.first.generation.value()][entry.first.kind];
    const CategoryTotal& source = entry.second;
    if (slot.authoritative_keys == 0 && slot.conflicted_keys == 0 && slot.shadowed_entries == 0 &&
        slot.contributors.empty() && slot.kind != source.kind) {
      slot = source;
      continue;
    }
    if (slot.kind != source.kind) {
      slot.kind = source.kind;
      slot.domain = source.domain;
    }
    const auto add_energy = [](Energy& target, const Energy& value) -> Expected<void> {
      auto sum = Energy::add(target, value);
      if (!sum) {
        return sum.error();
      }
      target = sum.value();
      return Expected<void>();
    };
    auto merged = add_energy(slot.authoritative, source.authoritative);
    if (!merged) return merged.error();
    merged = add_energy(slot.adjustments, source.adjustments);
    if (!merged) return merged.error();
    merged = add_energy(slot.total, source.total);
    if (!merged) return merged.error();
    merged = add_energy(slot.conflicted_min, source.conflicted_min);
    if (!merged) return merged.error();
    merged = add_energy(slot.conflicted_max, source.conflicted_max);
    if (!merged) return merged.error();
    merged = add_energy(slot.shadowed, source.shadowed);
    if (!merged) return merged.error();
    slot.conflicted_keys += source.conflicted_keys;
    slot.conflicted_entries += source.conflicted_entries;
    slot.shadowed_entries += source.shadowed_entries;
    slot.duplicate_replicas += source.duplicate_replicas;
    slot.authoritative_keys += source.authoritative_keys;
    slot.stale_entries += source.stale_entries;
    slot.unknown_quality_entries += source.unknown_quality_entries;
    slot.unknown_time_entries += source.unknown_time_entries;
    for (SequenceNumber contributor : source.contributors) {
      if (slot.contributors.size() < query.options.max_contributors) {
        slot.contributors.push_back(contributor);
      } else {
        slot.contributors_truncated = true;
      }
    }
    slot.contributors_truncated = slot.contributors_truncated || source.contributors_truncated;
  }
  for (auto& entry : by_generation) {
    GenerationRollup rollup;
    auto generation = GenerationRef::parse(entry.first);
    if (!generation) {
      return generation.error();
    }
    rollup.generation = generation.value();
    for (auto& category : entry.second) {
      rollup.categories.push_back(category.second);
    }
    result.generations.push_back(std::move(rollup));
  }
  return result;
}

Expected<Reconciliation> run_reconcile(const Ledger::Impl& impl, const RollupQuery& query) {
  auto rollup = run_rollup(impl, query, nullptr);
  if (!rollup) {
    return rollup.error();
  }
  const RollupResult& rolled = rollup.value();

  Reconciliation report;
  report.object = query.object;
  report.all_generations = query.all_generations;
  report.generation = query.generation;
  report.interval = query.interval;

  const auto accumulate = [&](EntryKind kind) -> Expected<Energy> {
    Energy total = Energy::zero();
    for (const GenerationRollup& generation : rolled.generations) {
      for (const CategoryTotal& category : generation.categories) {
        if (category.kind != kind) {
          continue;
        }
        auto sum = Energy::add(total, category.total);
        if (!sum) {
          return sum.error();
        }
        total = sum.value();
      }
    }
    return total;
  };

  auto delivered = accumulate(EntryKind::Delivered);
  if (!delivered) return delivered.error();
  auto consumed = accumulate(EntryKind::Consumed);
  if (!consumed) return consumed.error();
  auto wasted = accumulate(EntryKind::WastedLost);
  if (!wasted) return wasted.error();
  auto unclassified = accumulate(EntryKind::Unclassified);
  if (!unclassified) return unclassified.error();
  auto committed = accumulate(EntryKind::ReservationCommit);
  if (!committed) return committed.error();
  auto curtailed = accumulate(EntryKind::Curtailed);
  if (!curtailed) return curtailed.error();

  report.delivered = delivered.value();
  report.consumed = consumed.value();
  report.wasted_lost = wasted.value();
  report.unclassified_recorded = unclassified.value();
  report.committed = committed.value();
  report.curtailed = curtailed.value();

  // Evidence surface merged across generations, with conflicts counted once.
  for (const GenerationRollup& generation : rolled.generations) {
    for (const CategoryTotal& category : generation.categories) {
      report.categories.push_back(category);
      report.conflicted_keys += category.conflicted_keys;
      report.shadowed_entries += category.shadowed_entries;
      report.stale_entries += category.stale_entries;
      report.unknown_quality_entries += category.unknown_quality_entries;
      report.unknown_time_entries += category.unknown_time_entries;
      auto low = Energy::add(report.conflicted_min, category.conflicted_min);
      if (!low) return low.error();
      report.conflicted_min = low.value();
      auto high = Energy::add(report.conflicted_max, category.conflicted_max);
      if (!high) return high.error();
      report.conflicted_max = high.value();
    }
  }
  std::sort(report.categories.begin(), report.categories.end(),
            [](const CategoryTotal& a, const CategoryTotal& b) {
              return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
            });

  auto classified = Energy::add(report.consumed, report.wasted_lost);
  if (!classified) return classified.error();
  auto classified_all = Energy::add(classified.value(), report.unclassified_recorded);
  if (!classified_all) return classified_all.error();
  report.measured_classified = classified_all.value();

  auto residual = Energy::subtract(report.delivered, report.measured_classified);
  if (!residual) return residual.error();
  report.measured_residual = residual.value();
  report.measured_residual_is_zero = residual.value().is_zero();

  auto slack = Energy::subtract(report.committed, report.delivered);
  if (!slack) return slack.error();
  report.commitment_slack = slack.value();

  auto commitment_residual = Energy::subtract(slack.value(), report.curtailed);
  if (!commitment_residual) return commitment_residual.error();
  report.commitment_residual = commitment_residual.value();
  report.commitment_residual_is_zero = commitment_residual.value().is_zero();

  ExplanationLine measured;
  measured.code = "measured-flow-identity";
  measured.detail =
      "delivered = consumed + wasted-lost + unclassified-recorded + residual; residual = " +
      describe_quantity(report.measured_residual.joules());
  report.explanation.push_back(measured);

  ExplanationLine commitment;
  commitment.code = "commitment-identity";
  commitment.detail = "commitment-residual = committed - delivered - curtailed = " +
                      describe_quantity(report.commitment_residual.joules()) +
                      "; commitment accounting is a separate domain and is never merged into "
                      "measured flow";
  report.explanation.push_back(commitment);

  if (!report.measured_residual_is_zero) {
    ExplanationLine line;
    line.code = "unexplained-residual";
    line.detail = "unexplained residual of " + describe_quantity(report.measured_residual.joules()) +
                  " is retained and reported; it is not assigned to any category";
    report.explanation.push_back(line);
  }
  if (report.conflicted_keys > 0) {
    ExplanationLine line;
    line.code = "conflict";
    line.detail =
        std::to_string(report.conflicted_keys) +
        " accounting keys have conflicting equal-authority observations; no winner was selected "
        "and their candidate quantities range from " +
        describe_quantity(report.conflicted_min.joules()) + " to " +
        describe_quantity(report.conflicted_max.joules());
    report.explanation.push_back(line);
  }
  if (report.shadowed_entries > 0) {
    ExplanationLine line;
    line.code = "shadowed";
    line.detail = std::to_string(report.shadowed_entries) +
                  " entries were outranked by a more authoritative observation of the same key";
    report.explanation.push_back(line);
  }
  if (report.unknown_quality_entries > 0) {
    ExplanationLine line;
    line.code = "unknown-quality";
    line.detail = std::to_string(report.unknown_quality_entries) +
                  " entries carry an unknown observation quality; they are never treated as "
                  "verified and never treated as zero";
    report.explanation.push_back(line);
  }
  if (report.stale_entries > 0) {
    ExplanationLine line;
    line.code = "stale";
    line.detail = std::to_string(report.stale_entries) +
                  " entries are stale for the requested evaluation time";
    report.explanation.push_back(line);
  }

  report.complete = report.conflicted_keys == 0;
  report.completeness_note =
      report.complete ? "no conflicting evidence for the selected scope"
                      : "incomplete: conflicting evidence prevents a definitive answer for at "
                        "least one accounting key";
  return report;
}

Expected<ProvenanceChain> run_provenance(const Ledger::Impl& impl, SequenceNumber root,
                                         std::size_t max_nodes) {
  auto resolution = build_resolution(impl);
  if (!resolution) {
    return resolution.error();
  }
  const IndexRecord* record = impl.find_record(root.value());
  if (record == nullptr) {
    if (root.value() != 0 && root.value() <= impl.manifest.retired_floor_sequence) {
      return make_error(StatusCode::EntryRetired, "the entry was retired by retention");
    }
    return make_error(StatusCode::EntryNotFound, "no committed entry with that sequence");
  }
  const std::size_t root_index = static_cast<std::size_t>(record - impl.records.data());

  ProvenanceChain chain;
  chain.root = root;
  std::vector<std::uint8_t> seen(impl.records.size(), 0);

  const auto push = [&](std::size_t index, std::uint32_t depth, const char* relation) {
    if (chain.nodes.size() >= max_nodes) {
      chain.truncated = true;
      return;
    }
    if (seen[index] != 0) {
      return;
    }
    seen[index] = 1;
    const IndexRecord& node = impl.records[index];
    ProvenanceNode out;
    out.sequence = SequenceNumber(node.sequence);
    out.kind = node.entry_kind();
    out.state = resolution.value().states[index];
    out.event_id = node.event_id;
    out.content_digest = node.content_digest;
    out.source_instance = impl.text(node.source_instance);
    out.source_revision = node.source_revision;
    out.source_epoch = node.source_epoch;
    out.authority_tier = node.authority_tier;
    out.quantity = Energy::from_joules(node.quantity).value();
    out.depth = depth;
    out.relation = relation;
    chain.nodes.push_back(std::move(out));
  };

  push(root_index, 0, "self");
  if (record->target != 0) {
    auto target = index_of_sequence(impl, record->target);
    if (target) {
      push(target.value(), 1, "corrects");
    }
  }
  const AccountingKey root_key = key_of(impl, *record);
  for (std::size_t index = 0; index < impl.records.size(); ++index) {
    if (index == root_index) {
      continue;
    }
    const IndexRecord& other = impl.records[index];
    if (other.target == root.value()) {
      const char* relation = "corrected-by";
      if (other.entry_kind() == EntryKind::Supersession) {
        relation = "superseded-by";
      } else if (other.entry_kind() == EntryKind::Compensating) {
        relation = "compensated-by";
      }
      push(index, 1, relation);
      continue;
    }
    if (is_adjustment(other.entry_kind()) && other.entry_kind() != EntryKind::Supersession) {
      continue;
    }
    const IndexRecord* peer_root = impl.find_record(resolution.value().effective_sequence[index]);
    if (peer_root == nullptr) {
      continue;
    }
    if (!(key_of(impl, *peer_root) == root_key)) {
      continue;
    }
    const ResolutionState state = resolution.value().states[index];
    const char* relation = "peer";
    if (state == ResolutionState::Conflicted) {
      relation = "conflicts-with";
    } else if (state == ResolutionState::Shadowed) {
      relation = "outranked-by";
    } else if (state == ResolutionState::DuplicateReplica) {
      relation = "agrees-with";
    }
    push(index, 2, relation);
  }
  return chain;
}

// ---------------------------------------------------------------------------
// Read-only verification and audit
// ---------------------------------------------------------------------------

namespace {

void add_finding(IntegrityReport& report, const char* area, StatusCode code,
                 const std::string& detail, std::size_t max_findings) {
  report.ok = false;
  if (report.findings.size() >= max_findings) {
    return;
  }
  IntegrityFinding finding;
  finding.area = area;
  finding.code = code;
  finding.detail = detail;
  report.findings.push_back(std::move(finding));
}

struct ChainCursor {
  Digest256 previous;
  std::uint64_t expected_sequence = 0;
  std::uint64_t entries = 0;
  std::uint64_t bytes = 0;
  bool broken = false;
};

/// Walks the records of one committed segment region, verifying framing,
/// sequence contiguity, entry hashes and the integrity chain.
void walk_records(FileHandle& file, std::uint64_t start, std::uint64_t end, std::uint64_t segment_id,
                  ChainCursor& cursor, IntegrityReport& report, const VerifyOptions& options) {
  std::uint64_t offset = start;
  while (offset < end && !cursor.broken) {
    auto frame = read_record_at(file, offset, kMaxRecordBytes);
    if (!frame) {
      add_finding(report, "segment", frame.error().code,
                  "segment " + std::to_string(segment_id) + " at offset " +
                      std::to_string(offset) + ": " + frame.error().detail,
                  options.max_findings);
      cursor.broken = true;
      return;
    }
    const FramedRecord& record = frame.value();
    if (record.sequence != cursor.expected_sequence) {
      add_finding(report, "sequence",
                  record.sequence < cursor.expected_sequence ? StatusCode::SequenceReordered
                                                             : StatusCode::SequenceGap,
                  "segment " + std::to_string(segment_id) + " holds sequence " +
                      std::to_string(record.sequence) + " where " +
                      std::to_string(cursor.expected_sequence) + " was expected",
                  options.max_findings);
      cursor.broken = true;
      return;
    }
    if (!(record.prev_chain == cursor.previous)) {
      add_finding(report, "chain", StatusCode::ChainBroken,
                  "entry " + std::to_string(record.sequence) +
                      " does not chain to its predecessor",
                  options.max_findings);
      cursor.broken = true;
      return;
    }
    const Digest256 recomputed = compute_entry_hash(record.sequence, record.content);
    if (recomputed != record.entry_hash) {
      add_finding(report, "digest", StatusCode::DigestMismatch,
                  "entry " + std::to_string(record.sequence) +
                      " content hash does not match its stored entry hash",
                  options.max_findings);
      cursor.broken = true;
      return;
    }
    auto decoded = decode_content(record.content.data(), record.content.size());
    if (!decoded) {
      add_finding(report, "content", decoded.error().code,
                  "entry " + std::to_string(record.sequence) + ": " + decoded.error().detail,
                  options.max_findings);
      cursor.broken = true;
      return;
    }
    cursor.previous = compute_chain_hash(cursor.previous, record.entry_hash);
    ++cursor.expected_sequence;
    ++cursor.entries;
    cursor.bytes += record.framed_length;
    offset += record.framed_length;
    if (offset > end) {
      add_finding(report, "segment", StatusCode::StoreTruncated,
                  "segment " + std::to_string(segment_id) +
                      " ends inside a record body",
                  options.max_findings);
      cursor.broken = true;
      return;
    }
  }
  if (!cursor.broken && offset != end) {
    add_finding(report, "segment", StatusCode::StoreTruncated,
                "segment " + std::to_string(segment_id) +
                    " committed length is not a whole number of records",
                options.max_findings);
    cursor.broken = true;
  }
}

}  // namespace

Expected<IntegrityReport> verify_store_at(const StoreLayout& layout, const Descriptor& descriptor,
                                          const VerifyOptions& options) {
  IntegrityReport report;
  report.path = layout.root;
  report.store = descriptor.store;
  report.ok = true;

  ManifestSlot slot_a;
  ManifestSlot slot_b;
  auto manifest = load_best_manifest(layout, descriptor.store, &slot_a, &slot_b);
  report.manifests_checked = (slot_a.present ? 1u : 0u) + (slot_b.present ? 1u : 0u);
  for (const ManifestSlot* slot : {&slot_a, &slot_b}) {
    if (slot->present && !slot->valid) {
      add_finding(report, "manifest", StatusCode::StoreCorrupt,
                  "manifest slot " + slot->name + " is unusable: " + slot->failure,
                  options.max_findings);
    }
  }
  if (!manifest) {
    add_finding(report, "manifest", manifest.error().code, manifest.error().detail,
                options.max_findings);
    return report;
  }
  const Manifest& committed = manifest.value();
  report.incarnation = committed.incarnation;
  report.generation = ManifestGeneration(committed.generation);
  report.chain_head = committed.chain_head;

  // Rollback fence. The lease is only readable when no writer session holds
  // it; when it is readable, a committed generation below the recorded floor
  // means the store was rolled back and must not be trusted.
  {
    LeaseRecord lease;
    std::string lease_failure;
    auto lease_ok = read_lease(layout, &lease, &lease_failure);
    if (lease_ok && lease_ok.value()) {
      if (committed.generation < lease.floor_generation) {
        add_finding(report, "lease", StatusCode::StoreRollbackDetected,
                    "committed generation " + std::to_string(committed.generation) +
                        " is older than the recorded floor " +
                        std::to_string(lease.floor_generation),
                    options.max_findings);
      }
    }
  }

  ChainCursor cursor;
  cursor.previous = committed.retired_floor_sequence == 0 ? Digest256() : committed.chain_at_floor;
  cursor.expected_sequence = committed.retired_floor_sequence + 1;

  std::uint64_t total_entries = 0;
  for (const SegmentInfo& segment : committed.segments) {
    const std::string path = layout.segment_file(segment.id);
    auto kind = inspect_path(path);
    if (!kind) {
      add_finding(report, "segment", kind.error().code, kind.error().detail, options.max_findings);
      cursor.broken = true;
      continue;
    }
    if (kind.value() == PathKind::Missing) {
      add_finding(report, "segment", StatusCode::SegmentMissing,
                  "segment " + std::to_string(segment.id) + " is missing", options.max_findings);
      cursor.broken = true;
      continue;
    }
    if (kind.value() != PathKind::RegularFile) {
      add_finding(report, "segment", StatusCode::StorePathUnsafe,
                  "segment " + std::to_string(segment.id) + " is not a regular file",
                  options.max_findings);
      cursor.broken = true;
      continue;
    }
    auto file = open_file(path, OpenMode::ReadOnly);
    if (!file) {
      add_finding(report, "segment", file.error().code, file.error().detail, options.max_findings);
      cursor.broken = true;
      continue;
    }
    ++report.segments_checked;
    auto size = file_size(file.value());
    if (!size) {
      add_finding(report, "segment", size.error().code, size.error().detail, options.max_findings);
      cursor.broken = true;
      continue;
    }
    if (size.value() < segment.committed_bytes) {
      add_finding(report, "segment", StatusCode::StoreTruncated,
                  "segment " + std::to_string(segment.id) + " holds " +
                      std::to_string(size.value()) + " bytes but the manifest commits " +
                      std::to_string(segment.committed_bytes),
                  options.max_findings);
      cursor.broken = true;
      continue;
    }
    std::uint64_t prefix_end = segment.committed_bytes;
    if (segment.committed_bytes >= kSegmentTrailerBytes) {
      auto trailer = read_exact(file.value(), segment.committed_bytes - kSegmentTrailerBytes,
                                kSegmentTrailerBytes, kSegmentTrailerBytes);
      if (!trailer) {
        add_finding(report, "segment", trailer.error().code, trailer.error().detail,
                    options.max_findings);
        cursor.broken = true;
        continue;
      }
      bool trailer_magic = true;
      for (std::size_t index = 0; index < 8; ++index) {
        if (trailer.value()[index] != kTrailerMagic[index]) {
          trailer_magic = false;
          break;
        }
      }
      if (!trailer_magic) {
        add_finding(report, "segment", StatusCode::StoreCorrupt,
                    "sealed segment " + std::to_string(segment.id) +
                        " does not end with a seal trailer",
                    options.max_findings);
        cursor.broken = true;
        continue;
      }
      std::string trailer_failure;
      auto decoded_trailer =
          decode_segment_trailer(trailer.value().data(), trailer.value().size(), &trailer_failure);
      if (!decoded_trailer) {
        add_finding(report, "segment", decoded_trailer.error().code,
                    "sealed segment " + std::to_string(segment.id) + ": " +
                        (trailer_failure.empty() ? decoded_trailer.error().detail : trailer_failure),
                    options.max_findings);
        cursor.broken = true;
        continue;
      }
      if (decoded_trailer.value().record_count != segment.entry_count ||
          decoded_trailer.value().last_sequence != segment.last_sequence() ||
          !(decoded_trailer.value().digest == segment.digest)) {
        add_finding(report, "segment", StatusCode::StoreCorrupt,
                    "sealed segment " + std::to_string(segment.id) +
                        " seal trailer disagrees with the manifest",
                    options.max_findings);
        cursor.broken = true;
        continue;
      }
      prefix_end = segment.committed_bytes - kSegmentTrailerBytes;
    }
    auto header = read_exact(file.value(), 0, kSegmentHeaderBytes, kSegmentHeaderBytes);
    if (!header) {
      add_finding(report, "segment", header.error().code, header.error().detail,
                  options.max_findings);
      cursor.broken = true;
      continue;
    }
    std::string failure;
    auto first_sequence = decode_segment_header(header.value().data(), header.value().size(),
                                                descriptor.store, segment.id, &failure);
    if (!first_sequence) {
      add_finding(report, "segment", first_sequence.error().code,
                  "segment " + std::to_string(segment.id) + ": " +
                      (failure.empty() ? first_sequence.error().detail : failure),
                  options.max_findings);
      cursor.broken = true;
      continue;
    }
    if (first_sequence.value() != segment.first_sequence) {
      add_finding(report, "segment", StatusCode::SequenceReordered,
                  "segment " + std::to_string(segment.id) + " declares first sequence " +
                      std::to_string(first_sequence.value()) + " but the manifest declares " +
                      std::to_string(segment.first_sequence),
                  options.max_findings);
      cursor.broken = true;
      continue;
    }
    if (options.deep) {
      Sha256 hasher = make_segment_hasher();
      constexpr std::uint64_t kChunk = 1u * 1024u * 1024u;
      std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunk));
      std::uint64_t consumed = 0;
      bool digest_ok = true;
      while (consumed < prefix_end) {
        const std::uint64_t request = std::min(kChunk, prefix_end - consumed);
        auto read = read_at(file.value(), consumed, buffer.data(), static_cast<std::size_t>(request));
        if (!read || read.value() == 0) {
          digest_ok = false;
          add_finding(report, "segment", StatusCode::StoreTruncated,
                      "segment " + std::to_string(segment.id) + " ended while hashing",
                      options.max_findings);
          break;
        }
        hasher.update(buffer.data(), read.value());
        consumed += read.value();
      }
      if (digest_ok && !(hasher.finish() == segment.digest)) {
        add_finding(report, "segment", StatusCode::DigestMismatch,
                    "segment " + std::to_string(segment.id) +
                        " digest does not match the manifest",
                    options.max_findings);
        cursor.broken = true;
        continue;
      }
    }
    const std::uint64_t before = cursor.entries;
    walk_records(file.value(), kSegmentHeaderBytes, prefix_end, segment.id, cursor, report, options);
    if (cursor.entries - before != segment.entry_count) {
      add_finding(report, "segment", StatusCode::StoreCorrupt,
                  "segment " + std::to_string(segment.id) + " holds " +
                      std::to_string(cursor.entries - before) +
                      " entries but the manifest declares " +
                      std::to_string(segment.entry_count),
                  options.max_findings);
      cursor.broken = true;
    }
  }

  {
    const std::uint64_t active_id = committed.active_segment_id;
    const std::string path = layout.segment_file(active_id);
    auto kind = inspect_path(path);
    if (!kind) {
      add_finding(report, "segment", kind.error().code, kind.error().detail, options.max_findings);
      cursor.broken = true;
    } else if (kind.value() == PathKind::Missing) {
      add_finding(report, "segment", StatusCode::SegmentMissing,
                  "active segment " + std::to_string(active_id) + " is missing",
                  options.max_findings);
      cursor.broken = true;
    } else if (kind.value() != PathKind::RegularFile) {
      add_finding(report, "segment", StatusCode::StorePathUnsafe,
                  "active segment is not a regular file", options.max_findings);
      cursor.broken = true;
    } else {
      auto file = open_file(path, OpenMode::ReadOnly);
      if (!file) {
        add_finding(report, "segment", file.error().code, file.error().detail,
                    options.max_findings);
        cursor.broken = true;
      } else {
        ++report.segments_checked;
        auto size = file_size(file.value());
        if (!size) {
          add_finding(report, "segment", size.error().code, size.error().detail,
                      options.max_findings);
          cursor.broken = true;
        } else {
          if (size.value() < committed.active_segment_bytes) {
            add_finding(report, "segment", StatusCode::StoreTruncated,
                        "active segment holds " + std::to_string(size.value()) +
                            " bytes but the manifest commits " +
                            std::to_string(committed.active_segment_bytes),
                        options.max_findings);
            cursor.broken = true;
          } else {
            report.residue_bytes = size.value() - committed.active_segment_bytes;
          }
          auto header = read_exact(file.value(), 0, kSegmentHeaderBytes, kSegmentHeaderBytes);
          if (!header) {
            add_finding(report, "segment", header.error().code, header.error().detail,
                        options.max_findings);
            cursor.broken = true;
          } else {
            std::string failure;
            auto first_sequence = decode_segment_header(header.value().data(),
                                                        header.value().size(), descriptor.store,
                                                        active_id, &failure);
            if (!first_sequence) {
              add_finding(report, "segment", first_sequence.error().code,
                          failure.empty() ? first_sequence.error().detail : failure,
                          options.max_findings);
              cursor.broken = true;
            } else if (committed.active_segment_entries > 0 &&
                       first_sequence.value() != committed.active_first_sequence) {
              add_finding(report, "segment", StatusCode::SequenceReordered,
                          "active segment declares first sequence " +
                              std::to_string(first_sequence.value()),
                          options.max_findings);
              cursor.broken = true;
            } else if (!cursor.broken) {
              const std::uint64_t before = cursor.entries;
              walk_records(file.value(), kSegmentHeaderBytes, committed.active_segment_bytes,
                           active_id, cursor, report, options);
              if (cursor.entries - before != committed.active_segment_entries) {
                add_finding(report, "segment", StatusCode::StoreCorrupt,
                            "active segment holds " +
                                std::to_string(cursor.entries - before) +
                                " entries but the manifest declares " +
                                std::to_string(committed.active_segment_entries),
                            options.max_findings);
                cursor.broken = true;
              }
            }
          }
        }
      }
    }
  }

  total_entries = cursor.entries;
  report.entries_checked = total_entries;
  report.bytes_checked = cursor.bytes;
  if (!cursor.broken && total_entries != committed.entry_count) {
    add_finding(report, "manifest", StatusCode::StoreCorrupt,
                "replay produced " + std::to_string(total_entries) +
                    " entries but the manifest commits " + std::to_string(committed.entry_count),
                options.max_findings);
  }
  if (!cursor.broken && committed.entry_count > 0 && !(cursor.previous == committed.chain_head)) {
    add_finding(report, "chain", StatusCode::ChainBroken,
                "replayed chain head does not match the committed manifest head",
                options.max_findings);
  }
  (void)total_entries;
  return report;
}

Expected<StoreAudit> audit_store_at(const StoreLayout& layout, const Descriptor& descriptor,
                                    std::size_t max_path_bytes) {
  (void)max_path_bytes;
  StoreAudit audit;
  audit.path = layout.root;
  audit.store = descriptor.store;
  audit.format_version = kStoreFormatVersion;

  ManifestSlot slot_a;
  ManifestSlot slot_b;
  auto manifest = load_best_manifest(layout, descriptor.store, &slot_a, &slot_b);
  for (const ManifestSlot* slot : {&slot_a, &slot_b}) {
    ManifestSlotAudit entry;
    entry.name = slot->name;
    entry.present = slot->present;
    entry.valid = slot->valid;
    entry.generation = slot->valid ? ManifestGeneration(slot->manifest.generation)
                                   : ManifestGeneration(0);
    entry.failure = slot->valid ? std::string() : slot->failure;
    audit.slots.push_back(std::move(entry));
  }

  if (manifest) {
    const Manifest& committed = manifest.value();
    audit.incarnation = committed.incarnation;
    audit.generation = ManifestGeneration(committed.generation);
    audit.head_sequence = SequenceNumber(committed.head_sequence);
    audit.entry_count = committed.entry_count;
    audit.retired_floor_sequence = committed.retired_floor_sequence;
    audit.chain_head = committed.chain_head;
    audit.chain_at_floor = committed.chain_at_floor;

    for (const SegmentInfo& segment : committed.segments) {
      SegmentAudit entry;
      entry.id = segment.id;
      entry.first_sequence = segment.first_sequence;
      entry.entry_count = segment.entry_count;
      entry.committed_bytes = segment.committed_bytes;
      entry.digest = segment.digest;
      entry.sealed = true;
      auto kind = inspect_path(layout.segment_file(segment.id));
      if (kind && kind.value() == PathKind::RegularFile) {
        auto file = open_file(layout.segment_file(segment.id), OpenMode::ReadOnly);
        if (file) {
          auto size = file_size(file.value());
          if (size) {
            entry.file_bytes = size.value();
          }
          if (entry.file_bytes >= segment.committed_bytes) {
            Sha256 hasher = make_segment_hasher();
            constexpr std::uint64_t kChunk = 1u * 1024u * 1024u;
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunk));
            std::uint64_t consumed = 0;
            std::uint64_t digest_end = segment.committed_bytes - kSegmentTrailerBytes;
            bool ok = true;
            while (consumed < digest_end) {
              const std::uint64_t request = std::min(kChunk, digest_end - consumed);
              auto read = read_at(file.value(), consumed, buffer.data(),
                                  static_cast<std::size_t>(request));
              if (!read || read.value() == 0) {
                ok = false;
                break;
              }
              hasher.update(buffer.data(), read.value());
              consumed += read.value();
            }
            entry.digest_verified = ok && (hasher.finish() == segment.digest);
          }
        }
      }
      audit.segments.push_back(std::move(entry));
    }

    SegmentAudit active;
    active.id = committed.active_segment_id;
    active.first_sequence = committed.active_first_sequence;
    active.entry_count = committed.active_segment_entries;
    active.committed_bytes = committed.active_segment_bytes;
    auto kind = inspect_path(layout.segment_file(committed.active_segment_id));
    if (kind && kind.value() == PathKind::RegularFile) {
      auto file = open_file(layout.segment_file(committed.active_segment_id), OpenMode::ReadOnly);
      if (file) {
        auto size = file_size(file.value());
        if (size) {
          active.file_bytes = size.value();
          if (size.value() > committed.active_segment_bytes) {
            audit.residue_bytes = size.value() - committed.active_segment_bytes;
          }
        }
      }
    }
    audit.segments.push_back(std::move(active));

    // Segment files that no committed generation references.
    auto names = list_directory_entries(layout.segments_directory);
    if (names) {
      for (const std::string& name : names.value()) {
        auto identifier = parse_segment_file_name(name);
        if (!identifier) {
          continue;
        }
        bool known = identifier.value() == committed.active_segment_id;
        for (const SegmentInfo& segment : committed.segments) {
          if (segment.id == identifier.value()) {
            known = true;
            break;
          }
        }
        if (!known) {
          ++audit.orphan_segments;
        }
      }
    }
  }

  // Presence is a path property. The record itself is only readable when no
  // writer session holds the lease lock, so a held lease is reported as
  // present with the epoch left at its default rather than as absent.
  auto lease_kind = inspect_path(layout.lease);
  audit.writer_lease_present =
      lease_kind && lease_kind.value() == PathKind::RegularFile;
  LeaseRecord lease;
  std::string failure;
  auto lease_ok = read_lease(layout, &lease, &failure);
  if (lease_ok && lease_ok.value()) {
    audit.writer_epoch = WriterEpoch(lease.epoch);
    audit.writer_process_id = lease.process_id;
    audit.floor_generation = ManifestGeneration(lease.floor_generation);
  }
  auto lease_file = open_file(layout.lease, OpenMode::ReadOnly);
  if (lease_file) {  // NOLINT(readability-implicit-bool-conversion)
    auto held = try_lock(lease_file.value(), LockMode::Exclusive);
    if (held) {
      audit.writer_lease_held = !held.value();
      if (held.value()) {
        auto released = release_lock(lease_file.value());
        (void)released;
      }
    }
  }
  return audit;
}

Expected<Descriptor> load_descriptor_for_path(const std::string& path, std::size_t max_path_bytes,
                                              StoreLayout* layout) {
  auto resolved = layout_for(path, max_path_bytes);
  if (!resolved) {
    return resolved.error();
  }
  if (layout != nullptr) {
    *layout = resolved.value();
  }
  auto kind = inspect_path(resolved.value().root);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == PathKind::Missing) {
    return make_error(StatusCode::StoreNotFound, "store directory is missing: " + resolved.value().root);
  }
  if (kind.value() != PathKind::Directory) {
    return make_error(StatusCode::StorePathNotDirectory, "store root is not a directory");
  }
  return read_descriptor(resolved.value());
}

}  // namespace detail

Expected<RollupResult> Ledger::rollup(const RollupQuery& query) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  if (query.object.empty()) {
    return make_error(StatusCode::InvalidArgument, "rollup requires a facility object reference");
  }
  if (query.options.max_contributors == 0) {
    return make_error(StatusCode::LimitExceeded, "max_contributors must be at least one");
  }
  return detail::run_rollup(impl, query, nullptr);
}

Expected<Reconciliation> Ledger::reconcile(const RollupQuery& query) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  if (query.object.empty()) {
    return make_error(StatusCode::InvalidArgument, "reconcile requires a facility object reference");
  }
  return detail::run_reconcile(impl, query);
}

Expected<ProvenanceChain> Ledger::provenance(SequenceNumber sequence) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  return detail::run_provenance(impl, sequence, 64);
}

Expected<HistoryResult> Ledger::history(const HistoryQuery& query) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  HistoryResult result;
  auto resolution = detail::build_resolution(impl);
  if (!resolution) {
    return resolution.error();
  }
  for (std::size_t index = 0; index < impl.records.size(); ++index) {
    const IndexRecord& record = impl.records[index];
    bool matches = false;
    if (query.by_object) {
      matches = impl.text(record.object) == query.object.value();
    } else if (query.by_event_id) {
      matches = record.event_id == query.event;
    } else if (query.by_sequence_range) {
      matches = record.sequence >= query.from.value() && record.sequence <= query.to.value();
    } else {
      matches = true;
    }
    if (!matches) {
      continue;
    }
    ++result.total_matching;
    if (result.entries.size() >= query.limit) {
      result.truncated = true;
      continue;
    }
    HistoryEntry entry;
    entry.sequence = SequenceNumber(record.sequence);
    entry.kind = record.entry_kind();
    entry.state = resolution.value().states[index];
    entry.quantity = Energy::from_joules(record.quantity).value();
    entry.observed_at = TimeTicks(record.observed_at);
    entry.generation = impl.text(record.generation);
    entry.interval = impl.text(record.interval);
    entry.source_instance = impl.text(record.source_instance);
    entry.content_digest = record.content_digest;
    entry.chain_hash = record.chain_hash;
    entry.segment_id = record.segment_id;
    entry.record_offset = record.record_offset;
    result.entries.push_back(std::move(entry));
  }
  return result;
}

Expected<ReplaySummary> Ledger::replay() const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  ReplaySummary summary;
  summary.entries_replayed = impl.records.size();
  summary.head_sequence = SequenceNumber(impl.manifest.head_sequence);
  summary.generation = ManifestGeneration(impl.manifest.generation);
  summary.incarnation = impl.manifest.incarnation;
  summary.chain_head = impl.manifest.chain_head;
  summary.retired_entries = impl.manifest.retired_floor_sequence;
  summary.chain_verified = true;
  auto resolution = detail::build_resolution(impl);
  if (!resolution) {
    return resolution.error();
  }
  for (ResolutionState state : resolution.value().states) {
    if (state == ResolutionState::Conflicted) {
      ++summary.conflicted_keys;
    }
  }
  return summary;
}

Expected<IntegrityReport> Ledger::verify(const VerifyOptions& options) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  return detail::verify_store_at(impl.layout, impl.descriptor, options);
}

Expected<StoreAudit> Ledger::audit() const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  auto report = detail::audit_store_at(impl.layout, impl.descriptor, impl.limits.max_path_bytes);
  if (!report) {
    return report.error();
  }
  StoreAudit audit = report.value();
  audit.idempotency_records = impl.request_index.size();
  audit.retained_entries = impl.records.size();
  audit.residue_bytes = impl.residue_bytes;
  audit.orphan_segments = impl.orphan_segments;
  audit.lease_recovered = impl.lease_recovered;
  return audit;
}

Expected<IntegrityReport> verify_store(const std::string& path, const VerifyOptions& options) {
  StoreLayout layout;
  auto descriptor = detail::load_descriptor_for_path(path, 512, &layout);
  if (!descriptor) {
    IntegrityReport report;
    report.path = path;
    report.ok = false;
    IntegrityFinding finding;
    finding.area = "descriptor";
    finding.code = descriptor.error().code;
    finding.detail = descriptor.error().detail;
    report.findings.push_back(std::move(finding));
    return report;
  }
  return detail::verify_store_at(layout, descriptor.value(), options);
}

Expected<StoreAudit> audit_store(const std::string& path) {
  StoreLayout layout;
  auto descriptor = detail::load_descriptor_for_path(path, 512, &layout);
  if (!descriptor) {
    return descriptor.error();
  }
  return detail::audit_store_at(layout, descriptor.value(), 512);
}

}  // namespace energy_ledger
