// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Public ledger API. The ledger owns durable accounting state only: it accepts
// observations and commitments as evidence, commits them to an append-only
// integrity-chained sequence, and answers provenance-preserving queries.

#ifndef ENERGY_LEDGER_LEDGER_HPP
#define ENERGY_LEDGER_LEDGER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "energy_ledger/entry.hpp"
#include "energy_ledger/expected.hpp"
#include "energy_ledger/strong.hpp"

namespace energy_ledger {

/// Bounds applied to a store. Every value is validated before use and every
/// externally influenced allocation is derived from these limits.
struct StoreLimits {
  /// Hard ceiling on committed entries in one ledger incarnation.
  std::uint64_t max_entries = 262144;
  /// Segment rotation threshold in bytes.
  std::uint64_t max_segment_bytes = 16u * 1024u * 1024u;
  /// Hard ceiling on committed bytes in one ledger incarnation.
  std::uint64_t max_ledger_bytes = 2ull * 1024ull * 1024ull * 1024ull;
  /// Number of retained request-id idempotency records.
  std::size_t idempotency_window = 4096;
  /// Maximum accepted request id length.
  std::size_t max_request_id_bytes = ModelLimits::kMaxRequestIdBytes;
  /// Maximum accepted store path length in bytes.
  std::size_t max_path_bytes = 512;
};

struct CreateOptions {
  StoreLimits limits{};
  /// Refuse (StoreAlreadyExists) when a store descriptor is already present.
  bool fail_if_exists = true;
};

enum class AccessMode : std::uint8_t { ReadOnly = 1, ReadWrite = 2 };

struct OpenOptions {
  AccessMode mode = AccessMode::ReadWrite;
  /// Re-verify every committed record during open (deterministic replay).
  bool verify_all_records = true;
  /// Truncate uncommitted trailing residue in the active segment.
  bool discard_staging_residue = true;
  /// Administrative recovery for a missing or unreadable writer lease. The
  /// operator asserts that no other writer is live. The rollback floor and the
  /// writer epoch are preserved (never lowered) from the committed manifest, so
  /// recovery cannot be used to roll the store backwards.
  bool recover_lease = false;
  StoreLimits limits{};
};

/// Explicit durability fault injection. This exists so crash behaviour can be
/// tested with real process termination; it is inert unless a stage is set.
struct FaultInjection {
  enum class Stage : std::uint8_t {
    None = 0,
    BeforeRecordWrite = 1,
    AfterRecordWrite = 2,
    AfterRecordFlush = 3,
    AfterStagingManifestWrite = 4,
    AfterStagingManifestFlush = 5,
    AfterSlotWrite = 6,
    AfterSlotFlush = 7,
    AfterHeadCommit = 8,
    BeforeResultReturn = 9,
  };

  Stage terminate_at = Stage::None;
  int exit_code = 0xC0DE;
};

const char* to_string(FaultInjection::Stage stage) noexcept;
Expected<FaultInjection::Stage> parse_fault_stage(std::string_view text);

/// A request to append one accounting entry.
struct AppendRequest {
  EntryContent content;

  /// Optional caller supplied idempotency key (bounded, ASCII).
  std::string request_id;
  AttemptNumber attempt;

  /// The authority the caller planned against. When present, a moved head is
  /// refused with StaleAuthority -- unless this request is an idempotent
  /// replay, which returns the prior accepted result first.
  bool has_expected_head = false;
  SequenceNumber expected_head;
  bool has_expected_incarnation = false;
  LedgerIncarnation expected_incarnation;
  bool has_expected_generation = false;
  ManifestGeneration expected_generation;

  FaultInjection fault;
};

enum class AppendDisposition : std::uint8_t {
  Committed = 1,
  ReplayedByEventId = 2,
  ReplayedByRequestId = 3,
};

const char* to_string(AppendDisposition disposition) noexcept;

struct AppendResult {
  AppendDisposition disposition = AppendDisposition::Committed;
  SequenceNumber sequence;
  EventId event_id;
  Digest256 content_digest;
  Digest256 chain_hash;
  ManifestGeneration generation;
  SequenceNumber head;
  bool replayed() const noexcept { return disposition != AppendDisposition::Committed; }
};

/// A committed entry as stored, plus the resolution derived from current
/// history.
struct EntryView {
  SequenceNumber sequence;
  LedgerIncarnation incarnation;
  EntryContent content;
  Digest256 content_digest;
  Digest256 entry_hash;
  Digest256 prev_chain_hash;
  Digest256 chain_hash;
  EventId event_id;
  std::uint64_t segment_id = 0;
  std::uint64_t record_offset = 0;
  std::uint32_t record_length = 0;
  ManifestGeneration committed_generation;
  ResolutionState state = ResolutionState::Authoritative;
  FreshnessState freshness = FreshnessState::NotEvaluated;
};

/// Query bounds: contributors are always bounded and truncation is explicit.
struct QueryOptions {
  std::size_t max_contributors = 256;
  bool has_evaluation_time = false;
  TimeTicks evaluation_time;
  Duration max_age{};
  bool has_max_age = false;
  ClockDomainRef clock_domain;
  TimeBasis time_basis = TimeBasis::Unspecified;
  /// Tick rate of a tick based clock domain. Zero means "not declared", in
  /// which case age cannot be established and freshness stays unknown.
  std::uint64_t ticks_per_second = 0;
};

struct RollupQuery {
  FacilityObjectRef object;
  bool all_generations = true;
  GenerationRef generation;
  bool all_intervals = true;
  IntervalRef interval;
  QueryOptions options{};
};

struct CategoryTotal {
  EntryKind kind = EntryKind::Delivered;
  AccountingDomain domain = AccountingDomain::MeasuredFlow;

  /// Total of the authoritative observation of each accounting key.
  Energy authoritative = Energy::zero();
  /// Signed total of applied correction/compensating adjustments.
  Energy adjustments = Energy::zero();
  /// authoritative + adjustments.
  Energy total = Energy::zero();

  /// Conflicting keys have no winner: their candidate quantities are reported
  /// as a range and never summed into a total.
  Energy conflicted_min = Energy::zero();
  Energy conflicted_max = Energy::zero();
  std::uint64_t conflicted_keys = 0;
  std::uint64_t conflicted_entries = 0;

  /// Entries that lost the deterministic precedence contest.
  Energy shadowed = Energy::zero();
  std::uint64_t shadowed_entries = 0;
  std::uint64_t duplicate_replicas = 0;

  std::uint64_t authoritative_keys = 0;
  std::uint64_t stale_entries = 0;
  std::uint64_t unknown_quality_entries = 0;
  std::uint64_t unknown_time_entries = 0;

  std::vector<SequenceNumber> contributors;
  bool contributors_truncated = false;
};

struct GenerationRollup {
  GenerationRef generation;
  std::vector<CategoryTotal> categories;
};

struct RollupResult {
  FacilityObjectRef object;
  bool all_generations = true;
  GenerationRef generation;
  bool all_intervals = true;
  IntervalRef interval;
  std::vector<GenerationRollup> generations;
  std::uint64_t entries_considered = 0;
  std::uint64_t entries_matched = 0;
  QueryOptions options{};
};

/// One explanation line: a bounded, human-readable statement about why a
/// residual exists or why a total is incomplete.
struct ExplanationLine {
  std::string code;
  std::string detail;
};

struct Reconciliation {
  FacilityObjectRef object;
  GenerationRef generation;
  IntervalRef interval;
  bool all_generations = false;

  // Measured-flow domain: delivered = consumed + wasted_lost + unclassified + residual
  Energy delivered = Energy::zero();
  Energy consumed = Energy::zero();
  Energy wasted_lost = Energy::zero();
  Energy unclassified_recorded = Energy::zero();
  Energy measured_classified = Energy::zero();
  Energy measured_residual = Energy::zero();
  bool measured_residual_is_zero = true;

  // Commitment domain (kept separate on purpose):
  // commitment_residual = committed - delivered - curtailed
  Energy committed = Energy::zero();
  Energy curtailed = Energy::zero();
  Energy commitment_slack = Energy::zero();
  Energy commitment_residual = Energy::zero();
  bool commitment_residual_is_zero = true;

  // Evidence that is not authoritative and explains why a residual may exist.
  std::uint64_t conflicted_keys = 0;
  Energy conflicted_min = Energy::zero();
  Energy conflicted_max = Energy::zero();
  std::uint64_t shadowed_entries = 0;
  std::uint64_t stale_entries = 0;
  std::uint64_t unknown_quality_entries = 0;
  std::uint64_t unknown_time_entries = 0;
  std::uint64_t retired_entries_excluded = 0;

  std::vector<CategoryTotal> categories;
  std::vector<ExplanationLine> explanation;

  /// False when conflicting or unknown evidence prevents a definitive answer.
  bool complete = true;
  std::string completeness_note;
};

struct ProvenanceNode {
  SequenceNumber sequence;
  EntryKind kind = EntryKind::Delivered;
  ResolutionState state = ResolutionState::Authoritative;
  EventId event_id;
  Digest256 content_digest;
  std::string source_instance;
  std::uint64_t source_revision = 0;
  std::uint64_t source_epoch = 0;
  std::uint8_t authority_tier = 0;
  Energy quantity = Energy::zero();
  std::uint32_t depth = 0;
  std::string relation;
};

struct ProvenanceChain {
  SequenceNumber root;
  std::vector<ProvenanceNode> nodes;
  bool truncated = false;
};

struct HistoryQuery {
  bool by_object = false;
  FacilityObjectRef object;
  bool by_event_id = false;
  EventId event;
  bool by_sequence_range = false;
  SequenceNumber from;
  SequenceNumber to;
  std::size_t limit = 256;
};

struct HistoryEntry {
  SequenceNumber sequence;
  EntryKind kind = EntryKind::Delivered;
  ResolutionState state = ResolutionState::Authoritative;
  Energy quantity = Energy::zero();
  TimeTicks observed_at;
  std::string generation;
  std::string interval;
  std::string source_instance;
  Digest256 content_digest;
  Digest256 chain_hash;
  std::uint64_t segment_id = 0;
  std::uint64_t record_offset = 0;
};

struct HistoryResult {
  std::vector<HistoryEntry> entries;
  bool truncated = false;
  std::uint64_t total_matching = 0;
};

struct VerifyOptions {
  bool deep = true;
  bool stop_on_first_error = false;
  std::size_t max_findings = 64;
};

struct IntegrityFinding {
  std::string area;
  StatusCode code = StatusCode::Ok;
  std::string detail;
};

struct IntegrityReport {
  bool ok = false;
  std::string path;
  StoreUuid store;
  LedgerIncarnation incarnation;
  ManifestGeneration generation;
  std::uint64_t manifests_checked = 0;
  std::uint64_t segments_checked = 0;
  std::uint64_t entries_checked = 0;
  std::uint64_t bytes_checked = 0;
  std::uint64_t residue_bytes = 0;
  Digest256 chain_head;
  std::vector<IntegrityFinding> findings;
};

struct SegmentAudit {
  std::uint64_t id = 0;
  std::uint64_t first_sequence = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t committed_bytes = 0;
  std::uint64_t file_bytes = 0;
  Digest256 digest;
  /// True for a sealed segment. A sealed segment carries a digest in the
  /// manifest and ends with a seal trailer; the active segment does not, and
  /// its integrity is established by the record hashes and the chain instead.
  bool sealed = false;
  bool digest_verified = false;
  bool retired = false;
};

struct ManifestSlotAudit {
  std::string name;
  bool present = false;
  bool valid = false;
  ManifestGeneration generation;
  std::string failure;
};

struct StoreAudit {
  std::string path;
  StoreUuid store;
  LedgerIncarnation incarnation;
  ManifestGeneration generation;
  ManifestGeneration floor_generation;
  SequenceNumber head_sequence;
  std::uint64_t entry_count = 0;
  std::uint64_t retired_floor_sequence = 0;
  Digest256 chain_head;
  Digest256 chain_at_floor;
  std::vector<ManifestSlotAudit> slots;
  std::vector<SegmentAudit> segments;
  std::uint64_t residue_bytes = 0;
  /// Segment files that no committed generation references; they are residue of
  /// an interrupted rotation and are never read.
  std::uint64_t orphan_segments = 0;
  bool writer_lease_present = false;
  /// True when this session rebuilt the writer lease from the committed
  /// manifest because the previous lease was missing or unreadable.
  bool lease_recovered = false;
  WriterEpoch writer_epoch;
  std::uint64_t writer_process_id = 0;
  bool writer_lease_held = false;
  std::size_t idempotency_records = 0;
  std::size_t retained_entries = 0;
  std::uint64_t format_version = 0;
};

struct ReplaySummary {
  std::uint64_t entries_replayed = 0;
  SequenceNumber head_sequence;
  ManifestGeneration generation;
  LedgerIncarnation incarnation;
  Digest256 chain_head;
  std::uint64_t conflicted_keys = 0;
  std::uint64_t retired_entries = 0;
  bool chain_verified = false;
};

struct CompactionOptions {
  /// Every whole segment whose entries are all strictly below this sequence is
  /// retired. The active segment is never retired.
  SequenceNumber retain_from;
  /// Number of segment files to keep regardless of the floor (>= 1).
  std::uint64_t keep_segments = 1;
};

struct CompactionResult {
  ManifestGeneration generation;
  std::uint64_t segments_retired = 0;
  std::uint64_t bytes_reclaimed = 0;
  SequenceNumber floor_sequence;
  Digest256 chain_at_floor;
};

/// Durable, provenance-preserving accounting ledger for one store path.
///
/// Concurrency model: at most one writer process and no readers while a writer
/// session is open. Authority is an operating-system file lock on the writer
/// lease file; the lease epoch fences a writer that lost its authority. No
/// internal mutex participates in cross-process safety, and no method holds an
/// internal lock across I/O.
class Ledger {
 public:
  /// Opaque internal state. Declared here so out-of-line internal translation
  /// units can share it; it is intentionally incomplete in the public header
  /// and is not part of the supported API.
  struct Impl;

  Ledger(Ledger&& other) noexcept;
  Ledger& operator=(Ledger&& other) noexcept;
  ~Ledger();

  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;

  /// Initializes a new store. Fails if a store already exists unless
  /// fail_if_exists is false, in which case the existing store is opened.
  static Expected<Ledger> create(const std::string& path, const CreateOptions& options = {});

  /// Opens an existing store. Recovery adopts exactly one whole verified
  /// manifest generation or refuses.
  static Expected<Ledger> open(const std::string& path, const OpenOptions& options = {});

  /// True when a store descriptor and at least one valid manifest are present.
  static Expected<bool> is_initialized(const std::string& path);

  /// Appends one entry. Returns the committed sequence and chain position. The
  /// call returns only after the entry is durable at the documented commit
  /// point; a replay of an accepted attempt returns the prior result.
  Expected<AppendResult> append(const AppendRequest& request);

  Expected<EntryView> inspect(SequenceNumber sequence) const;
  Expected<EntryView> find_by_event_id(const EventId& id) const;
  Expected<RollupResult> rollup(const RollupQuery& query) const;
  Expected<Reconciliation> reconcile(const RollupQuery& query) const;
  Expected<ProvenanceChain> provenance(SequenceNumber sequence) const;
  Expected<HistoryResult> history(const HistoryQuery& query) const;
  Expected<IntegrityReport> verify(const VerifyOptions& options = {}) const;
  Expected<StoreAudit> audit() const;
  Expected<ReplaySummary> replay() const;
  Expected<CompactionResult> compact(const CompactionOptions& options);

  SequenceNumber head_sequence() const;
  ManifestGeneration generation() const;
  LedgerIncarnation incarnation() const;
  StoreUuid store_uuid() const;
  AccessMode mode() const;
  /// Lease epoch of this writer session; zero when opened read-only.
  WriterEpoch writer_epoch() const;
  bool is_open() const;
  const StoreLimits& limits() const;
  const std::string& path() const;

  /// Flushes, refreshes the rollback floor and releases writer authority.
  /// Idempotent: closing a closed ledger succeeds.
  Expected<void> close();

 private:
  explicit Ledger(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

/// Durable-format description used by the audit tooling.
struct FormatInfo {
  std::uint32_t descriptor_version = 0;
  std::uint32_t manifest_version = 0;
  std::uint32_t segment_version = 0;
  std::uint32_t lease_version = 0;
  std::size_t descriptor_bytes = 0;
  std::size_t manifest_header_bytes = 0;
  std::size_t segment_header_bytes = 0;
  std::size_t lease_bytes = 0;
  bool little_endian_on_disk = true;
};

FormatInfo format_info() noexcept;

/// Read-only, lock-free verification of a store from its path. Reads committed
/// prefixes only, so it is safe to run while another process holds writer
/// authority. Reports every structural, framing, digest and chain failure it
/// finds instead of stopping at the first one.
Expected<IntegrityReport> verify_store(const std::string& path, const VerifyOptions& options = {});

/// Read-only store inventory: manifest slots, segments, retirement floor,
/// writer lease state and residue. Never modifies the store.
Expected<StoreAudit> audit_store(const std::string& path);

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_LEDGER_HPP
