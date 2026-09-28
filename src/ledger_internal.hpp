// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal ledger state shared by the mutation path (ledger.cpp) and the query
// path (query.cpp). Nothing in this header is part of the installed API.

#ifndef ENERGY_LEDGER_SRC_LEDGER_INTERNAL_HPP
#define ENERGY_LEDGER_SRC_LEDGER_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "energy_ledger/ledger.hpp"
#include "os_file.hpp"
#include "store.hpp"

namespace energy_ledger {

struct EventIdHash {
  std::size_t operator()(const EventId& id) const noexcept {
    std::size_t hash = 1469598103934665603ull;
    for (std::uint8_t byte : id.storage()) {
      hash ^= static_cast<std::size_t>(byte);
      hash *= 1099511628211ull;
    }
    return hash;
  }
};

/// One committed entry as held in memory. Strings are interned; the canonical
/// content itself is re-read and re-verified from the segment on demand.
struct IndexRecord {
  std::uint64_t sequence = 0;
  std::uint64_t segment_id = 0;
  std::uint64_t record_offset = 0;
  std::uint32_t record_length = 0;
  std::uint32_t content_length = 0;

  std::uint32_t object = 0;
  std::uint32_t generation = 0;
  std::uint32_t interval = 0;
  std::uint32_t clock_domain = 0;
  std::uint32_t source_family = 0;
  std::uint32_t source_instance = 0;
  std::uint32_t authority = 0;

  std::uint64_t source_revision = 0;
  std::uint64_t source_generation = 0;
  std::uint64_t source_epoch = 0;
  std::uint64_t target = 0;

  std::int64_t quantity = 0;
  std::int64_t declared_value = 0;
  std::int64_t interval_start = 0;
  std::int64_t interval_end = 0;
  std::int64_t observed_at = 0;

  std::uint8_t kind = 0;
  std::uint8_t quality = 0;
  std::uint8_t time_basis = 0;
  std::uint8_t authority_tier = 0;
  std::uint8_t declared_unit = 1;

  Digest256 content_digest;
  Digest256 entry_hash;
  Digest256 chain_hash;
  Digest256 prev_chain;
  EventId event_id;

  EntryKind entry_kind() const noexcept { return static_cast<EntryKind>(kind); }
  AccountingDomain domain() const noexcept { return domain_of(entry_kind()); }
  EntryKind accounting_kind() const noexcept { return entry_kind(); }
  TimeTicks observed() const noexcept { return TimeTicks(observed_at); }
};

struct IdempotencyRecord {
  std::string request_id;
  std::uint64_t attempt = 0;
  AppendResult result;
};

struct StagedMutation {
  detail::Manifest previous_manifest;
  Sha256 previous_hasher;
  std::uint64_t previous_active_bytes = 0;
  std::uint64_t previous_active_id = 0;
  std::uint64_t previous_active_first_sequence = 0;
  std::uint64_t previous_active_entries = 0;
  bool rotated = false;
  bool armed = false;
};

struct Ledger::Impl {
  std::string path;
  detail::StoreLayout layout;
  StoreLimits limits;
  AccessMode mode = AccessMode::ReadWrite;
  bool open = false;
  bool poisoned = false;

  detail::Descriptor descriptor;
  detail::Manifest manifest;

  detail::FileHandle lease_file;
  bool lease_held = false;
  WriterEpoch writer_epoch;
  WriterId writer_id;

  std::vector<std::string> interned;
  std::unordered_map<std::string, std::uint32_t> intern_map;
  std::vector<IndexRecord> records;
  std::unordered_map<EventId, std::uint64_t, EventIdHash> event_index;
  std::unordered_map<std::string, IdempotencyRecord> request_index;
  std::deque<std::string> request_order;

  detail::FileHandle active_file;
  Sha256 segment_hasher;
  bool hasher_ready = false;

  std::uint64_t residue_bytes = 0;
  std::uint64_t floor_generation = 0;
  std::uint64_t orphan_segments = 0;
  bool lease_recovered = false;

  std::uint32_t intern(const std::string& text) {
    auto found = intern_map.find(text);
    if (found != intern_map.end()) {
      return found->second;
    }
    const std::uint32_t identifier = static_cast<std::uint32_t>(interned.size());
    interned.push_back(text);
    intern_map.emplace(interned.back(), identifier);
    return identifier;
  }

  const std::string& text(std::uint32_t identifier) const {
    static const std::string kEmpty;
    if (identifier >= interned.size()) {
      return kEmpty;
    }
    return interned[identifier];
  }

  const IndexRecord* find_record(std::uint64_t sequence) const {
    if (sequence <= manifest.retired_floor_sequence) {
      return nullptr;
    }
    const std::uint64_t offset = sequence - first_sequence();
    if (offset >= records.size()) {
      return nullptr;
    }
    const IndexRecord& record = records[static_cast<std::size_t>(offset)];
    return record.sequence == sequence ? &record : nullptr;
  }

  std::uint64_t first_sequence() const { return manifest.retired_floor_sequence + 1; }
  std::uint64_t next_sequence() const { return manifest.head_sequence + 1; }
  std::uint64_t committed_bytes_total() const {
    std::uint64_t total = manifest.active_segment_bytes;
    for (const detail::SegmentInfo& segment : manifest.segments) {
      total += segment.committed_bytes;
    }
    return total;
  }
};

namespace detail {

/// Re-reads one committed record from its segment, verifies its digest and
/// decodes the canonical content.
Expected<EntryContent> load_content(const Ledger::Impl& impl, const IndexRecord& record);

/// Builds the entry view for one index record.
Expected<EntryView> make_entry_view(const Ledger::Impl& impl, const IndexRecord& record);

/// Resolution of every record under the documented precedence rules, computed
/// from immutable history.
struct ResolutionTable {
  std::vector<ResolutionState> states;
  std::vector<std::uint64_t> effective_sequence;  // target of an adjustment, else own sequence
};

Expected<ResolutionTable> build_resolution(const Ledger::Impl& impl);

Expected<RollupResult> run_rollup(const Ledger::Impl& impl, const RollupQuery& query,
                                  const ResolutionTable* precomputed);
Expected<Reconciliation> run_reconcile(const Ledger::Impl& impl, const RollupQuery& query);
Expected<ProvenanceChain> run_provenance(const Ledger::Impl& impl, SequenceNumber root,
                                         std::size_t max_nodes);

/// Classifies freshness from observation time, evaluation time and maximum age.
/// Freshness is derived at query time and never stored, so recovered evidence
/// cannot become fresh merely because the ledger was reopened.
FreshnessState classify_freshness(const IndexRecord& record, const QueryOptions& options,
                                  std::string_view record_clock_domain);

/// True when the accounting key of the record matches the query selector.
bool matches_selector(const Ledger::Impl& impl, const IndexRecord& record, const RollupQuery& query);

AccountingKey key_of(const Ledger::Impl& impl, const IndexRecord& record);

/// Independent, read-only verification and audit paths. Both operate on the
/// committed prefix of the store and never modify it, so they are safe to run
/// while another process holds writer authority.
Expected<IntegrityReport> verify_store_at(const detail::StoreLayout& layout,
                                          const detail::Descriptor& descriptor,
                                          const VerifyOptions& options);
Expected<StoreAudit> audit_store_at(const detail::StoreLayout& layout,
                                    const detail::Descriptor& descriptor,
                                    std::size_t max_path_bytes);
Expected<detail::Descriptor> load_descriptor_for_path(const std::string& path,
                                                      std::size_t max_path_bytes,
                                                      detail::StoreLayout* layout);

}  // namespace detail
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_SRC_LEDGER_INTERNAL_HPP
