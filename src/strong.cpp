// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

#include "energy_ledger/checked.hpp"
#include "energy_ledger/strong.hpp"

namespace energy_ledger {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

void fill_random(std::uint8_t* out, std::size_t length) {
  std::random_device device;
  std::size_t index = 0;
  while (index < length) {
    const std::uint32_t word = device();
    for (int byte = 0; byte < 4 && index < length; ++byte) {
      out[index++] = static_cast<std::uint8_t>((word >> (8 * byte)) & 0xFFu);
    }
  }
}

}  // namespace

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::InvalidArgument: return "invalid-argument";
    case StatusCode::InvalidRef: return "invalid-ref";
    case StatusCode::InvalidQuantity: return "invalid-quantity";
    case StatusCode::InvalidUnit: return "invalid-unit";
    case StatusCode::InexactUnitConversion: return "inexact-unit-conversion";
    case StatusCode::QuantitySignNotAllowed: return "quantity-sign-not-allowed";
    case StatusCode::InconsistentDeclaredQuantity: return "inconsistent-declared-quantity";
    case StatusCode::InvalidInterval: return "invalid-interval";
    case StatusCode::InvalidTimeBasis: return "invalid-time-basis";
    case StatusCode::InvalidAnnotation: return "invalid-annotation";
    case StatusCode::InvalidTarget: return "invalid-target";
    case StatusCode::InvalidAuthorityTier: return "invalid-authority-tier";
    case StatusCode::LimitExceeded: return "limit-exceeded";
    case StatusCode::ArithmeticOverflow: return "arithmetic-overflow";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::MalformedInput: return "malformed-input";
    case StatusCode::IntegrationInexact: return "integration-inexact";
    case StatusCode::TimeBasisMismatch: return "time-basis-mismatch";
    case StatusCode::MissingIntervalDuration: return "missing-interval-duration";
    case StatusCode::StoreNotFound: return "store-not-found";
    case StatusCode::StoreAlreadyExists: return "store-already-exists";
    case StatusCode::StoreBusy: return "store-busy";
    case StatusCode::StoreCorrupt: return "store-corrupt";
    case StatusCode::StoreTruncated: return "store-truncated";
    case StatusCode::StoreIoError: return "store-io-error";
    case StatusCode::StoreFormatUnsupported: return "store-format-unsupported";
    case StatusCode::StoreEndianMismatch: return "store-endian-mismatch";
    case StatusCode::StoreSwapped: return "store-swapped";
    case StatusCode::StoreRollbackDetected: return "store-rollback-detected";
    case StatusCode::StorePathUnsafe: return "store-path-unsafe";
    case StatusCode::StorePathTooLong: return "store-path-too-long";
    case StatusCode::StorePathNotDirectory: return "store-path-not-directory";
    case StatusCode::StoreReparsePointRejected: return "store-reparse-point-rejected";
    case StatusCode::StoreStagingResidue: return "store-staging-residue";
    case StatusCode::StoreReadOnly: return "store-read-only";
    case StatusCode::StoreIncompleteInit: return "store-incomplete-init";
    case StatusCode::LedgerClosed: return "ledger-closed";
    case StatusCode::EntryNotFound: return "entry-not-found";
    case StatusCode::EntryRetired: return "entry-retired";
    case StatusCode::StaleAuthority: return "stale-authority";
    case StatusCode::StaleWriter: return "stale-writer";
    case StatusCode::IncarnationMismatch: return "incarnation-mismatch";
    case StatusCode::GenerationMismatch: return "generation-mismatch";
    case StatusCode::RequestIdConflict: return "request-id-conflict";
    case StatusCode::UnauthorizedSupersession: return "unauthorized-supersession";
    case StatusCode::TargetNotInLedger: return "target-not-in-ledger";
    case StatusCode::TargetKeyMismatch: return "target-key-mismatch";
    case StatusCode::ConflictUnresolved: return "conflict-unresolved";
    case StatusCode::UnknownNotZero: return "unknown-not-zero";
    case StatusCode::ChainBroken: return "chain-broken";
    case StatusCode::DigestMismatch: return "digest-mismatch";
    case StatusCode::SequenceGap: return "sequence-gap";
    case StatusCode::SequenceReordered: return "sequence-reordered";
    case StatusCode::DuplicateSequence: return "duplicate-sequence";
    case StatusCode::CrcMismatch: return "crc-mismatch";
    case StatusCode::RecordFramingInvalid: return "record-framing-invalid";
    case StatusCode::SegmentMissing: return "segment-missing";
    case StatusCode::ManifestMissing: return "manifest-missing";
    case StatusCode::InternalError: return "internal-error";
  }
  return "unknown-status";
}

bool is_store_failure(StatusCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  return value >= 32 && value < 64;
}

std::string Error::describe() const {
  std::string text = to_string(code);
  if (!detail.empty()) {
    text += ": ";
    text += detail;
  }
  return text;
}

const char* to_string(TimeBasis basis) noexcept {
  switch (basis) {
    case TimeBasis::Unspecified: return "unspecified";
    case TimeBasis::MonotonicTicks: return "monotonic-ticks";
    case TimeBasis::UtcUnixMilliseconds: return "utc-unix-milliseconds";
    case TimeBasis::UtcUnixSeconds: return "utc-unix-seconds";
    case TimeBasis::WallClockMinutes: return "wall-clock-minutes";
    case TimeBasis::MeterRegisterTicks: return "meter-register-ticks";
    case TimeBasis::IntervalIndex: return "interval-index";
  }
  return "unknown";
}

bool is_valid_time_basis(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(TimeBasis::Unspecified) &&
         value <= static_cast<std::uint8_t>(TimeBasis::IntervalIndex);
}

Expected<TimeBasis> parse_time_basis(std::string_view text) {
  if (text == "unspecified") return TimeBasis::Unspecified;
  if (text == "monotonic-ticks") return TimeBasis::MonotonicTicks;
  if (text == "utc-unix-milliseconds") return TimeBasis::UtcUnixMilliseconds;
  if (text == "utc-unix-seconds") return TimeBasis::UtcUnixSeconds;
  if (text == "wall-clock-minutes") return TimeBasis::WallClockMinutes;
  if (text == "meter-register-ticks") return TimeBasis::MeterRegisterTicks;
  if (text == "interval-index") return TimeBasis::IntervalIndex;
  return make_error(StatusCode::InvalidTimeBasis, "unknown time basis '" + std::string(text) + "'");
}

Expected<AuthorityTier> AuthorityTier::create(std::uint32_t value) {
  if (value > kMaxValue) {
    return make_error(StatusCode::InvalidAuthorityTier,
                      "authority tier " + std::to_string(value) + " exceeds maximum " +
                          std::to_string(kMaxValue));
  }
  AuthorityTier tier;
  tier.value_ = static_cast<std::uint8_t>(value);
  return tier;
}

Expected<Duration> Duration::from_milliseconds(std::int64_t milliseconds) {
  if (milliseconds < 0) {
    return make_error(StatusCode::InvalidArgument, "duration must not be negative");
  }
  return Duration(milliseconds);
}

Expected<Duration> Duration::from_seconds(std::int64_t seconds) {
  if (seconds < 0) {
    return make_error(StatusCode::InvalidArgument, "duration must not be negative");
  }
  std::int64_t millis = 0;
  if (!checked::mul(seconds, 1000, millis)) {
    return make_error(StatusCode::ArithmeticOverflow, "duration in milliseconds overflows");
  }
  return Duration(millis);
}

Expected<Duration> Duration::from_ticks(std::int64_t ticks, std::int64_t ticks_per_second) {
  if (ticks < 0 || ticks_per_second <= 0) {
    return make_error(StatusCode::InvalidArgument,
                      "ticks and ticks-per-second must describe a non-negative duration");
  }
  std::int64_t millis = 0;
  if (!checked::mul(ticks, 1000, millis)) {
    return make_error(StatusCode::ArithmeticOverflow, "tick duration overflows");
  }
  if (!checked::div_exact(millis, ticks_per_second, millis)) {
    return make_error(StatusCode::InexactUnitConversion,
                      "tick duration is not a whole number of milliseconds");
  }
  return Duration(millis);
}

LedgerIncarnation generate_ledger_incarnation() {
  LedgerIncarnation::Storage storage{};
  fill_random(storage.data(), storage.size());
  return LedgerIncarnation(storage);
}

StoreUuid generate_store_uuid() {
  StoreUuid::Storage storage{};
  fill_random(storage.data(), storage.size());
  return StoreUuid(storage);
}

WriterId derive_writer_id(const StoreUuid& store, WriterEpoch epoch) noexcept {
  std::uint8_t payload[24];
  for (std::size_t index = 0; index < 16; ++index) {
    payload[index] = store.storage()[index];
  }
  const std::uint64_t value = epoch.value();
  for (int index = 0; index < 8; ++index) {
    payload[16 + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
  const Digest256 digest =
      Sha256::hash_domain("EnergyLedger.WriterId.v1", payload, sizeof(payload));
  WriterId::Storage storage{};
  for (std::size_t index = 0; index < storage.size(); ++index) {
    storage[index] = digest.storage()[index];
  }
  return WriterId(storage);
}

Expected<std::string> validate_opaque_ref(std::string_view text) {
  if (text.empty()) {
    return make_error(StatusCode::InvalidRef, "reference must not be empty");
  }
  if (text.size() > OpaqueRef<FacilityObjectTag>::kMaxLength) {
    return make_error(StatusCode::InvalidRef,
                      "reference exceeds " +
                          std::to_string(OpaqueRef<FacilityObjectTag>::kMaxLength) + " bytes");
  }
  for (char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu) {
      return make_error(StatusCode::InvalidRef,
                        "reference contains a non printable ASCII byte");
    }
  }
  if (text.front() == ' ' || text.back() == ' ') {
    return make_error(StatusCode::InvalidRef, "reference must not start or end with a space");
  }
  return std::string(text);
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80u) {
      index += 1;
      continue;
    } else if ((lead & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = lead & 0x07u;
    } else {
      return false;
    }
    if (index + extra >= text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    if (extra == 1 && code_point < 0x80u) return false;
    if (extra == 2 && code_point < 0x800u) return false;
    if (extra == 3 && code_point < 0x10000u) return false;
    if (code_point > 0x10FFFFu) return false;
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) return false;
    index += extra + 1;
  }
  return true;
}

std::string to_hex(const std::uint8_t* data, std::size_t length) {
  std::string text;
  text.reserve(length * 2);
  for (std::size_t index = 0; index < length; ++index) {
    text.push_back(kHexDigits[(data[index] >> 4) & 0x0Fu]);
    text.push_back(kHexDigits[data[index] & 0x0Fu]);
  }
  return text;
}

bool from_hex(std::string_view hex, std::uint8_t* out, std::size_t out_length) noexcept {
  if (hex.size() != out_length * 2) {
    return false;
  }
  for (std::size_t index = 0; index < out_length; ++index) {
    const int high = hex_value(hex[index * 2]);
    const int low = hex_value(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool Digest256::is_zero() const noexcept {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest256::to_hex() const { return energy_ledger::to_hex(bytes_.data(), bytes_.size()); }

Expected<Digest256> Digest256::from_hex(std::string_view hex) {
  Storage storage{};
  if (!energy_ledger::from_hex(hex, storage.data(), storage.size())) {
    return make_error(StatusCode::InvalidArgument, "digest is not 64 hexadecimal characters");
  }
  return Digest256(storage);
}

}  // namespace energy_ledger
