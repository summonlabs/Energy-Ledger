// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <array>

#include "energy_ledger/digest.hpp"

namespace energy_ledger {
namespace {

// CRC-32, reflected, polynomial 0xEDB88320, initial value 0, no final XOR.
// Used only as a fast framing check; SHA-256 provides the integrity claim.
const std::array<std::uint32_t, 256>& table() {
  static const std::array<std::uint32_t, 256> instance = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      values[index] = value;
    }
    return values;
  }();
  return instance;
}

}  // namespace

std::uint32_t crc32_update(std::uint32_t seed, const void* data, std::size_t length) noexcept {
  const auto& lookup = table();
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t value = seed;
  for (std::size_t index = 0; index < length; ++index) {
    value = lookup[(value ^ bytes[index]) & 0xFFu] ^ (value >> 8);
  }
  return value;
}

std::uint32_t crc32(const void* data, std::size_t length) noexcept {
  return crc32_update(0u, data, length);
}

}  // namespace energy_ledger
