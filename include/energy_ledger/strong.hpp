// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities. Stable identity is separated from mutable
// metadata, and semantically distinct counters (sequence, revision, source
// generation, source epoch, manifest generation, writer epoch, attempt) are
// distinct types that cannot be interchanged by accident.

#ifndef ENERGY_LEDGER_STRONG_HPP
#define ENERGY_LEDGER_STRONG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "energy_ledger/digest.hpp"
#include "energy_ledger/expected.hpp"

namespace energy_ledger {

// ---------------------------------------------------------------------------
// Numeric identity wrappers
// ---------------------------------------------------------------------------

template <class Tag>
class Counter {
 public:
  using value_type = std::uint64_t;

  constexpr Counter() noexcept = default;
  explicit constexpr Counter(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// Next value. Refuses (returns false) instead of wrapping.
  bool next(Counter& out) const noexcept {
    if (value_ == UINT64_MAX) {
      return false;
    }
    out = Counter(value_ + 1);
    return true;
  }

  friend constexpr bool operator==(Counter a, Counter b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Counter a, Counter b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Counter a, Counter b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Counter a, Counter b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator<=(Counter a, Counter b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(Counter a, Counter b) noexcept { return a.value_ >= b.value_; }

 private:
  std::uint64_t value_ = 0;
};

struct SequenceTag {};
struct RevisionTag {};
struct SourceGenerationTag {};
struct SourceEpochTag {};
struct ManifestGenerationTag {};
struct WriterEpochTag {};
struct AttemptTag {};
struct AuthorizedOperationsTag {};

/// Position of a committed entry in the ledger incarnation. 0 means "none".
using SequenceNumber = Counter<SequenceTag>;
/// Monotonic revision of an observation produced by one source instance.
using SourceRevision = Counter<RevisionTag>;
/// Generation of the physical/logical measuring device or source.
using SourceGeneration = Counter<SourceGenerationTag>;
/// Epoch of a source instance; a newer epoch retires all older observations.
using SourceEpoch = Counter<SourceEpochTag>;
/// Manifest generation of the durable store (the commit point ordinal).
using ManifestGeneration = Counter<ManifestGenerationTag>;
/// Writer lease epoch; strictly increases on every writer acquisition.
using WriterEpoch = Counter<WriterEpochTag>;
/// Attempt counter carried by externally retried requests.
using AttemptNumber = Counter<AttemptTag>;

// ---------------------------------------------------------------------------
// Authority tier
// ---------------------------------------------------------------------------

/// Rank of the authority that asserted an observation. Higher tiers outrank
/// lower tiers when two observations describe the same accounting key. The
/// meaning of each rank is an operator policy decision; the ledger only orders
/// them.
class AuthorityTier {
 public:
  static constexpr std::uint8_t kMaxValue = 15;

  AuthorityTier() noexcept = default;

  static Expected<AuthorityTier> create(std::uint32_t value);

  constexpr std::uint8_t value() const noexcept { return value_; }

  friend constexpr bool operator==(AuthorityTier a, AuthorityTier b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(AuthorityTier a, AuthorityTier b) noexcept { return !(a == b); }
  friend constexpr bool operator<(AuthorityTier a, AuthorityTier b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(AuthorityTier a, AuthorityTier b) noexcept { return b < a; }
  friend constexpr bool operator<=(AuthorityTier a, AuthorityTier b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(AuthorityTier a, AuthorityTier b) noexcept { return !(a < b); }

 private:
  std::uint8_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Raw tick count in a named clock domain. The sentinel kUnknown marks a time
/// that was never observed; it is deliberately not zero, because zero is a
/// legitimate instant.
class TimeTicks {
 public:
  static constexpr std::int64_t kUnknownValue = INT64_MIN;

  constexpr TimeTicks() noexcept = default;
  explicit constexpr TimeTicks(std::int64_t value) noexcept : value_(value) {}

  static constexpr TimeTicks unknown() noexcept { return TimeTicks(kUnknownValue); }

  constexpr std::int64_t value() const noexcept { return value_; }
  constexpr bool is_unknown() const noexcept { return value_ == kUnknownValue; }
  constexpr bool is_known() const noexcept { return value_ != kUnknownValue; }

  friend constexpr bool operator==(TimeTicks a, TimeTicks b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(TimeTicks a, TimeTicks b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(TimeTicks a, TimeTicks b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(TimeTicks a, TimeTicks b) noexcept { return b < a; }
  friend constexpr bool operator<=(TimeTicks a, TimeTicks b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(TimeTicks a, TimeTicks b) noexcept { return !(a < b); }

 private:
  std::int64_t value_ = kUnknownValue;
};

/// Clock domain / time basis of an interval and its observation times. The
/// ledger never mixes bases inside one accounting key, and it never treats an
/// unspecified basis as a known time.
enum class TimeBasis : std::uint8_t {
  Unspecified = 0,
  MonotonicTicks = 1,
  UtcUnixMilliseconds = 2,
  UtcUnixSeconds = 3,
  WallClockMinutes = 4,
  MeterRegisterTicks = 5,
  IntervalIndex = 6,
};

const char* to_string(TimeBasis basis) noexcept;
Expected<TimeBasis> parse_time_basis(std::string_view text);
bool is_valid_time_basis(std::uint8_t value) noexcept;

/// True when the byte sequence is well-formed UTF-8 (rejects overlong forms,
/// surrogate code points and values above U+10FFFF).
bool is_valid_utf8(std::string_view text) noexcept;

/// A duration with an explicit, canonical millisecond representation.
class Duration {
 public:
  constexpr Duration() noexcept = default;
  explicit constexpr Duration(std::int64_t milliseconds) noexcept : millis_(milliseconds) {}

  static Expected<Duration> from_milliseconds(std::int64_t milliseconds);
  static Expected<Duration> from_seconds(std::int64_t seconds);
  static Expected<Duration> from_ticks(std::int64_t ticks, std::int64_t ticks_per_second);

  constexpr std::int64_t milliseconds() const noexcept { return millis_; }
  constexpr bool is_negative() const noexcept { return millis_ < 0; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.millis_ == b.millis_; }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept { return a.millis_ != b.millis_; }
  friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.millis_ < b.millis_; }

 private:
  std::int64_t millis_ = 0;
};

// ---------------------------------------------------------------------------
// Byte identifiers
// ---------------------------------------------------------------------------

struct LedgerIncarnationTag {};
struct StoreUuidTag {};
struct WriterIdTag {};
struct EventIdTag {};

template <class Tag, std::size_t N>
class ByteId {
 public:
  using Storage = std::array<std::uint8_t, N>;

  ByteId() noexcept : bytes_{} {}
  explicit ByteId(const Storage& bytes) noexcept : bytes_(bytes) {}

  static Expected<ByteId> from_hex(std::string_view hex) {
    Storage storage{};
    if (!energy_ledger::from_hex(hex, storage.data(), N)) {
      return make_error(StatusCode::InvalidArgument, "identifier is not valid hex of the expected length");
    }
    return ByteId(storage);
  }

  const Storage& storage() const noexcept { return bytes_; }
  const std::uint8_t* data() const noexcept { return bytes_.data(); }
  static constexpr std::size_t size() noexcept { return N; }

  bool is_zero() const noexcept {
    for (std::uint8_t byte : bytes_) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }

  std::string to_hex() const { return energy_ledger::to_hex(bytes_.data(), N); }

  friend bool operator==(const ByteId& a, const ByteId& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const ByteId& a, const ByteId& b) noexcept { return !(a == b); }
  friend bool operator<(const ByteId& a, const ByteId& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  Storage bytes_;
};

using LedgerIncarnation = ByteId<LedgerIncarnationTag, 16>;
using StoreUuid = ByteId<StoreUuidTag, 16>;
using WriterId = ByteId<WriterIdTag, 16>;
using EventId = ByteId<EventIdTag, 16>;
using EntryDigest = ByteId<struct EntryDigestTag, 32>;

/// Ledger incarnation for a freshly initialized store. Generated once from the
/// operating-system random source and then immutable: it is intentional
/// identity, not volatility, and it never appears in entry content digests.
LedgerIncarnation generate_ledger_incarnation();

/// Store identity, generated once at initialization.
StoreUuid generate_store_uuid();

/// Writer identity is derived, not random: it is a pure function of the store
/// identity and the writer lease epoch, so replaying a lease is detectable.
WriterId derive_writer_id(const StoreUuid& store, WriterEpoch epoch) noexcept;

// ---------------------------------------------------------------------------
// Opaque external references
// ---------------------------------------------------------------------------

/// Validates and normalizes an opaque reference: 1..128 printable ASCII bytes,
/// no control characters, no leading or trailing whitespace.
Expected<std::string> validate_opaque_ref(std::string_view text);

struct FacilityObjectTag {};
struct GenerationTag {};
struct IntervalTag {};
struct ClockDomainTag {};
struct SourceFamilyTag {};
struct SourceInstanceTag {};
struct AuthorityTag {};

template <class Tag>
class OpaqueRef {
 public:
  static constexpr std::size_t kMaxLength = 128;

  OpaqueRef() = default;

  static Expected<OpaqueRef> parse(std::string_view text) {
    auto validated = validate_opaque_ref(text);
    if (!validated) {
      return validated.error();
    }
    return OpaqueRef(std::move(validated).value());
  }

  const std::string& value() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const OpaqueRef& a, const OpaqueRef& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const OpaqueRef& a, const OpaqueRef& b) noexcept { return !(a == b); }
  friend bool operator<(const OpaqueRef& a, const OpaqueRef& b) noexcept { return a.value_ < b.value_; }

 private:
  explicit OpaqueRef(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

/// Facility object this ledger accounts for. Opaque: the ledger never resolves
/// it to facility metadata, which is owned by other systems.
using FacilityObjectRef = OpaqueRef<FacilityObjectTag>;
/// Owning generation of the facility object (physical or logical generation).
using GenerationRef = OpaqueRef<GenerationTag>;
/// Accounting interval identity.
using IntervalRef = OpaqueRef<IntervalTag>;
/// Clock domain in which the interval and observation times are expressed.
using ClockDomainRef = OpaqueRef<ClockDomainTag>;
/// Source family (for example a meter class or an estimation pipeline).
using SourceFamilyRef = OpaqueRef<SourceFamilyTag>;
/// Source instance inside a family (for example one specific meter).
using SourceInstanceRef = OpaqueRef<SourceInstanceTag>;
/// Identity of the authority that asserted an observation.
using AuthorityRef = OpaqueRef<AuthorityTag>;

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_STRONG_HPP
