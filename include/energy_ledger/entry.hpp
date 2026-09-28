// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The accounting event model. An entry records what a source asserted about a
// facility object over an interval; it never records a physical actuation and
// it never records an unobserved measurement.

#ifndef ENERGY_LEDGER_ENTRY_HPP
#define ENERGY_LEDGER_ENTRY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "energy_ledger/digest.hpp"
#include "energy_ledger/expected.hpp"
#include "energy_ledger/strong.hpp"
#include "energy_ledger/units.hpp"

namespace energy_ledger {

/// Kind of an accepted accounting entry. Measured-flow kinds assert a physical
/// energy quantity, commitment kinds assert a reserved/curtailed quantity in a
/// separate accounting domain, adjustment kinds modify earlier entries without
/// rewriting them.
enum class EntryKind : std::uint8_t {
  Delivered = 1,
  Consumed = 2,
  WastedLost = 3,
  Unclassified = 4,
  ReservationCommit = 5,
  Curtailed = 6,
  Correction = 7,
  Supersession = 8,
  Compensating = 9,
};

/// Accounting domain. Domains are reconciled separately and are never mixed
/// into a single balance, because a commitment and a measured flow are not the
/// same physical identity.
enum class AccountingDomain : std::uint8_t {
  MeasuredFlow = 1,
  Commitment = 2,
  Adjustment = 3,
};

const char* to_string(EntryKind kind) noexcept;
Expected<EntryKind> parse_entry_kind(std::string_view text);
bool is_valid_entry_kind(std::uint8_t value) noexcept;
const char* to_string(AccountingDomain domain) noexcept;
AccountingDomain domain_of(EntryKind kind) noexcept;
/// True for kinds that assert a quantity in their own domain.
bool asserts_quantity(EntryKind kind) noexcept;
/// True for kinds that reference an earlier entry they modify.
bool is_adjustment(EntryKind kind) noexcept;

/// Quality of the observation behind an entry. Unknown is the default and is
/// never promoted: a missing quality is not evidence of a verified reading.
enum class ObservationQuality : std::uint8_t {
  Unknown = 0,
  Verified = 1,
  Estimated = 2,
  Synthetic = 3,
  Rejected = 4,
};

const char* to_string(ObservationQuality quality) noexcept;
Expected<ObservationQuality> parse_observation_quality(std::string_view text);
bool is_valid_quality(std::uint8_t value) noexcept;

/// Freshness of an observation, computed at query time from the observation
/// time, the evaluation time and a caller supplied maximum age. Freshness is
/// never stored: reopening a ledger cannot make recovered evidence fresh.
enum class FreshnessState : std::uint8_t {
  NotEvaluated = 0,
  Fresh = 1,
  Stale = 2,
  UnknownTimeBasis = 3,
  UnknownObservationTime = 4,
  DomainMismatch = 5,
};

const char* to_string(FreshnessState state) noexcept;

/// Resolution of an entry under the documented precedence rules. This is a
/// derived view, recomputed from immutable history, never stored state.
enum class ResolutionState : std::uint8_t {
  Authoritative = 1,
  DuplicateReplica = 2,
  Shadowed = 3,
  Conflicted = 4,
  Superseded = 5,
  Retired = 6,
  Unauthorized = 7,
  AdjustmentApplied = 8,
  AdjustmentUnapplied = 9,
};

const char* to_string(ResolutionState state) noexcept;

/// Bounded provenance annotation. Annotations are canonical state: they are
/// sorted by key, unique, and part of the content digest.
struct Annotation {
  std::string key;
  std::string value;

  friend bool operator==(const Annotation& a, const Annotation& b) noexcept {
    return a.key == b.key && a.value == b.value;
  }
  friend bool operator<(const Annotation& a, const Annotation& b) noexcept {
    return a.key < b.key || (a.key == b.key && a.value < b.value);
  }
};

/// Model limits. Every externally influenced size is bounded before allocation.
struct ModelLimits {
  static constexpr std::size_t kMaxAnnotations = 16;
  static constexpr std::size_t kMaxAnnotationKeyBytes = 64;
  static constexpr std::size_t kMaxAnnotationValueBytes = 128;
  static constexpr std::size_t kMaxContentBytes = 4096;
  static constexpr std::size_t kMaxRecordBytes = 65536;
  static constexpr std::size_t kMaxRequestIdBytes = 64;
};

/// The immutable content of an accepted entry. Everything that determines the
/// accounting meaning of the entry is here; nothing volatile is.
class EntryContent {
 public:
  EntryKind kind = EntryKind::Delivered;
  ObservationQuality quality = ObservationQuality::Unknown;
  AuthorityTier authority_tier;
  AuthorityRef authority;

  TimeBasis time_basis = TimeBasis::Unspecified;

  SourceRevision source_revision;
  SourceGeneration source_generation;
  SourceEpoch source_epoch;

  /// Target of an adjustment entry; SequenceNumber() (none) for non-adjustments.
  SequenceNumber target;

  /// Authoritative quantity in canonical joules.
  Energy quantity = Energy::zero();
  /// Value exactly as declared by the source, kept for provenance.
  std::int64_t declared_value = 0;
  /// Unit the source declared; re-checked against the canonical quantity.
  EnergyUnit declared_unit = EnergyUnit::Joule;

  TimeTicks interval_start;
  TimeTicks interval_end;
  TimeTicks observed_at;

  FacilityObjectRef object;
  GenerationRef generation;
  IntervalRef interval;
  ClockDomainRef clock_domain;
  SourceFamilyRef source_family;
  SourceInstanceRef source_instance;

  std::vector<Annotation> annotations;

  friend bool operator==(const EntryContent& a, const EntryContent& b);
  friend bool operator!=(const EntryContent& a, const EntryContent& b) { return !(a == b); }
};

/// Accounting key: the identity of the physical (or committed) quantity an
/// entry talks about. Observations from different sources compete inside one
/// key; they are never summed as if they were independent measurements.
struct AccountingKey {
  FacilityObjectRef object;
  GenerationRef generation;
  IntervalRef interval;
  ClockDomainRef clock_domain;
  EntryKind kind = EntryKind::Delivered;

  friend bool operator==(const AccountingKey& a, const AccountingKey& b) noexcept {
    return a.object == b.object && a.generation == b.generation && a.interval == b.interval &&
           a.clock_domain == b.clock_domain && a.kind == b.kind;
  }
  friend bool operator!=(const AccountingKey& a, const AccountingKey& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const AccountingKey& a, const AccountingKey& b) noexcept;
};

AccountingKey accounting_key_of(const EntryContent& content);

/// Deterministic validation. The ladder order is fixed and documented; the
/// same invalid request always yields the same primary error code.
Expected<void> validate_content(const EntryContent& content);

/// Canonical digest of the content (domain separated, version tagged).
Expected<Digest256> content_digest(const EntryContent& content);

/// Event identity: a pure function of the content, stable across ledger
/// incarnations, used for idempotent replay detection.
Expected<EventId> event_identity(const EntryContent& content);

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_ENTRY_HPP
