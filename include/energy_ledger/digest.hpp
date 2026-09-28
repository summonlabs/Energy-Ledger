// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Dependency-free SHA-256 and CRC-32 used by the store integrity model.

#ifndef ENERGY_LEDGER_DIGEST_HPP
#define ENERGY_LEDGER_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "energy_ledger/expected.hpp"

namespace energy_ledger {

inline constexpr std::size_t kDigestBytes = 32;

/// Fixed-width digest value.
class Digest256 {
 public:
  using Storage = std::array<std::uint8_t, kDigestBytes>;

  Digest256() noexcept : bytes_{} {}
  explicit Digest256(const Storage& bytes) noexcept : bytes_(bytes) {}

  static Expected<Digest256> from_hex(std::string_view hex);

  const Storage& storage() const noexcept { return bytes_; }
  bool is_zero() const noexcept;
  std::string to_hex() const;

  friend bool operator==(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ == b.bytes_;
  }
  friend bool operator!=(const Digest256& a, const Digest256& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest256& a, const Digest256& b) noexcept {
    return a.bytes_ < b.bytes_;
  }

 private:
  Storage bytes_;
};

/// Streaming SHA-256.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  /// Finalizes and returns the digest. Calling finish() more than once returns
  /// the same digest; the object is not reset.
  Digest256 finish() noexcept;

  static Digest256 hash(const void* data, std::size_t length) noexcept;
  static Digest256 hash(std::string_view text) noexcept {
    return hash(text.data(), text.size());
  }

  /// Domain-separated hash: SHA-256(domain || 0x00 || payload).
  static Digest256 hash_domain(std::string_view domain, const void* data,
                               std::size_t length) noexcept;

 private:
  std::uint32_t state_[8];
  std::uint8_t buffer_[64];
  std::uint64_t total_bytes_;
  std::size_t buffer_used_;
  bool finalized_;
  Digest256 digest_;
};

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), seeded with 0.
std::uint32_t crc32(const void* data, std::size_t length) noexcept;
std::uint32_t crc32_update(std::uint32_t seed, const void* data, std::size_t length) noexcept;

/// Lowercase hex encoding helpers.
std::string to_hex(const std::uint8_t* data, std::size_t length);
bool from_hex(std::string_view hex, std::uint8_t* out, std::size_t out_length) noexcept;

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_DIGEST_HPP
