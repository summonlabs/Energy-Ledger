// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic canonical encoding of entry content. Encoding is explicit
// little-endian, length-prefixed and bounded; decoding rejects malformed,
// truncated, oversized and non-canonical input.

#ifndef ENERGY_LEDGER_CANONICAL_HPP
#define ENERGY_LEDGER_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "energy_ledger/entry.hpp"
#include "energy_ledger/expected.hpp"

namespace energy_ledger {

inline constexpr std::uint8_t kContentEncodingVersion = 1;
inline constexpr std::string_view kContentDomain = "EnergyLedger.EntryContent.v1";
inline constexpr std::string_view kEventIdDomain = "EnergyLedger.EventId.v1";
inline constexpr std::string_view kEntryHashDomain = "EnergyLedger.EntryHash.v1";
inline constexpr std::string_view kChainDomain = "EnergyLedger.Chain.v1";
inline constexpr std::string_view kSegmentDigestDomain = "EnergyLedger.SegmentDigest.v1";

/// Encodes content into its canonical byte form.
Expected<std::vector<std::uint8_t>> encode_content(const EntryContent& content);

/// Decodes canonical bytes. Rejects trailing bytes, unknown versions, and any
/// length that exceeds the model limits.
Expected<EntryContent> decode_content(const std::uint8_t* data, std::size_t length);

/// Round-trip helper used by tests and the CLI.
Expected<EntryContent> decode_content(const std::vector<std::uint8_t>& bytes);

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_CANONICAL_HPP
