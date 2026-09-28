// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Administrative and test-support helpers that operate on store files directly
// instead of through the ledger. They exist so that store attacks (corruption,
// truncation, residue injection, lease forgery, rollback) can be exercised
// against a real on-disk store, and so that the store-audit tooling can read
// raw bytes. They perform no accounting and hold no ledger locks.

#ifndef ENERGY_LEDGER_TEST_SUPPORT_HPP
#define ENERGY_LEDGER_TEST_SUPPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "energy_ledger/expected.hpp"
#include "energy_ledger/strong.hpp"

namespace energy_ledger {
namespace test_support {

Expected<std::vector<std::uint8_t>> read_file_bytes(const std::string& path);
Expected<void> write_file_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes);
Expected<void> append_file_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes);
Expected<void> truncate_file_to(const std::string& path, std::uint64_t length);
/// Flips one bit in one byte of a file, in place.
Expected<void> flip_bit(const std::string& path, std::uint64_t byte_offset, std::uint8_t bit_index);
Expected<void> remove_file(const std::string& path);
/// Recursively removes a directory tree. Bounded and refuses reparse points.
Expected<void> remove_directory_tree(const std::string& path);
Expected<void> create_directory(const std::string& path);
Expected<void> copy_file(const std::string& from, const std::string& to);
Expected<bool> file_exists(const std::string& path);
Expected<std::uint64_t> file_size(const std::string& path);

/// Rewrites the writer lease epoch without holding writer authority, modelling
/// a second writer that ignored the operating-system lock.
Expected<void> force_lease_epoch(const std::string& store_path, WriterEpoch epoch);
Expected<std::uint64_t> read_lease_epoch(const std::string& store_path);
Expected<StoreUuid> read_store_uuid(const std::string& store_path);
Expected<LedgerIncarnation> read_ledger_incarnation(const std::string& store_path);

/// Lists segment file names (not full paths) in the store, sorted.
Expected<std::vector<std::string>> list_segment_files(const std::string& store_path);

/// Sleeps without busy-waiting; bounded by the platform.
void sleep_milliseconds(std::uint64_t milliseconds);

/// Exit code used by crash-injection children.
inline constexpr int kCrashExitCode = 0xC0DE;

}  // namespace test_support
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_TEST_SUPPORT_HPP
