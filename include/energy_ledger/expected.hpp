// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Explicit outcome and error types. Every fallible library entry point returns
// an Expected<T>; no function reports failure through a sentinel value and no
// function silently converts a failure into a zero quantity.

#ifndef ENERGY_LEDGER_EXPECTED_HPP
#define ENERGY_LEDGER_EXPECTED_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace energy_ledger {

/// Stable, documented failure classification. Codes are part of the public
/// contract: the same invalid request always produces the same primary code
/// (validation precedence is documented in docs/FORMAT.md).
enum class StatusCode : std::uint16_t {
  Ok = 0,

  // ---- request / model validation -------------------------------------
  InvalidArgument = 1,
  InvalidRef = 2,
  InvalidQuantity = 3,
  InvalidUnit = 4,
  InexactUnitConversion = 5,
  QuantitySignNotAllowed = 6,
  InconsistentDeclaredQuantity = 7,
  InvalidInterval = 8,
  InvalidTimeBasis = 9,
  InvalidAnnotation = 10,
  InvalidTarget = 11,
  InvalidAuthorityTier = 12,
  LimitExceeded = 13,
  ArithmeticOverflow = 14,
  Unsupported = 15,
  MalformedInput = 16,
  IntegrationInexact = 17,
  TimeBasisMismatch = 18,
  MissingIntervalDuration = 19,

  // ---- store / persistence --------------------------------------------
  StoreNotFound = 32,
  StoreAlreadyExists = 33,
  StoreBusy = 34,
  StoreCorrupt = 35,
  StoreTruncated = 36,
  StoreIoError = 37,
  StoreFormatUnsupported = 38,
  StoreEndianMismatch = 39,
  StoreSwapped = 40,
  StoreRollbackDetected = 41,
  StorePathUnsafe = 42,
  StorePathTooLong = 43,
  StorePathNotDirectory = 44,
  StoreReparsePointRejected = 45,
  StoreStagingResidue = 46,
  StoreReadOnly = 47,
  StoreIncompleteInit = 48,

  // ---- ledger state and authority --------------------------------------
  LedgerClosed = 64,
  EntryNotFound = 65,
  EntryRetired = 66,
  StaleAuthority = 67,
  StaleWriter = 68,
  IncarnationMismatch = 69,
  GenerationMismatch = 70,
  RequestIdConflict = 71,
  UnauthorizedSupersession = 72,
  TargetNotInLedger = 73,
  TargetKeyMismatch = 74,
  ConflictUnresolved = 75,
  UnknownNotZero = 76,

  // ---- integrity --------------------------------------------------------
  ChainBroken = 96,
  DigestMismatch = 97,
  SequenceGap = 98,
  SequenceReordered = 99,
  DuplicateSequence = 100,
  CrcMismatch = 101,
  RecordFramingInvalid = 102,
  SegmentMissing = 103,
  ManifestMissing = 104,

  InternalError = 127,
};

/// Human-readable, stable name for a status code.
const char* to_string(StatusCode code) noexcept;

/// True when the code describes a durable-store level failure.
bool is_store_failure(StatusCode code) noexcept;

/// An error: a status code plus bounded, non-secret human context.
struct Error {
  StatusCode code = StatusCode::Ok;
  std::string detail;

  Error() = default;
  Error(StatusCode c, std::string d) : code(std::move(c)), detail(std::move(d)) {}

  bool ok() const noexcept { return code == StatusCode::Ok; }
  std::string describe() const;
};

inline Error make_error(StatusCode code, std::string detail) {
  return Error(code, std::move(detail));
}

/// Result of a fallible operation.
template <class T>
class Expected {
 public:
  Expected(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Expected(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

  const Error& error() const noexcept { return error_; }

  T* operator->() { return &*value_; }
  const T* operator->() const { return &*value_; }
  T& operator*() & { return *value_; }
  const T& operator*() const& { return *value_; }
  T&& operator*() && { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Error error_;
};

/// Result of a fallible operation that produces no value.
template <>
class Expected<void> {
 public:
  Expected() = default;
  Expected(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }
  const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_EXPECTED_HPP
