// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store format: descriptor, alternating manifest slots, staging
// manifest, append-only segments and the writer lease. All framing, version and
// integrity rules live here so the ledger runtime and the standalone
// verification path share exactly one interpretation of the bytes on disk.

#ifndef ENERGY_LEDGER_SRC_STORE_HPP
#define ENERGY_LEDGER_SRC_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "energy_ledger/entry.hpp"
#include "energy_ledger/expected.hpp"
#include "energy_ledger/ledger.hpp"
#include "energy_ledger/strong.hpp"
#include "os_file.hpp"

namespace energy_ledger {
namespace detail {

inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::uint32_t kEndianMarker = 0x01020304u;

inline constexpr std::uint64_t kDescriptorHeaderBytes = 64;
inline constexpr std::uint64_t kDescriptorBytes = 96;
inline constexpr std::uint64_t kManifestHeaderBytes = 224;
inline constexpr std::uint64_t kManifestSegmentBytes = 64;
inline constexpr std::uint64_t kManifestTrailerBytes = 48;
inline constexpr std::uint64_t kMaxSegmentsPerManifest = 4096;
inline constexpr std::uint64_t kSegmentHeaderBytes = 64;
inline constexpr std::uint64_t kSegmentTrailerBytes = 72;
inline constexpr std::uint64_t kLeaseHeaderBytes = 96;
inline constexpr std::uint64_t kLeaseBytes = 128;
inline constexpr std::uint64_t kRecordHeaderBytes = 12;
inline constexpr std::uint64_t kMaxRecordBytes = ModelLimits::kMaxRecordBytes;
inline constexpr std::uint64_t kMaxManifestBytes =
    kManifestHeaderBytes + (kMaxSegmentsPerManifest * kManifestSegmentBytes) + kManifestTrailerBytes;

extern const std::uint8_t kDescriptorMagic[8];
extern const std::uint8_t kManifestMagic[8];
extern const std::uint8_t kSegmentMagic[8];
extern const std::uint8_t kTrailerMagic[8];
extern const std::uint8_t kLeaseMagic[8];
extern const std::uint8_t kRecordMagic[4];

struct StoreLayout {
  std::string root;
  std::string descriptor;
  std::string manifest_a;
  std::string manifest_b;
  std::string manifest_staging;
  std::string lease;
  std::string segments_directory;
  std::string segment_file(std::uint64_t id) const;
};

Expected<StoreLayout> layout_for(const std::string& path, std::size_t max_path_bytes);

struct Descriptor {
  std::uint32_t version = kStoreFormatVersion;
  StoreUuid store;
  std::uint32_t canonical_unit = 1;
};

Expected<Descriptor> decode_descriptor(const std::uint8_t* data, std::size_t length,
                                       std::string* failure);
Expected<std::vector<std::uint8_t>> encode_descriptor(const Descriptor& descriptor);
Expected<Descriptor> read_descriptor(const StoreLayout& layout);

struct SegmentInfo {
  std::uint64_t id = 0;
  std::uint64_t first_sequence = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t committed_bytes = 0;
  Digest256 digest;

  std::uint64_t last_sequence() const { return first_sequence + entry_count - 1; }
};

struct Manifest {
  std::uint64_t generation = 0;
  LedgerIncarnation incarnation;
  std::uint64_t head_sequence = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t retired_floor_sequence = 0;
  std::uint64_t seq_at_floor = 0;
  Digest256 chain_head;
  Digest256 chain_at_floor;
  std::uint64_t writer_epoch = 0;
  std::uint64_t active_segment_id = 0;
  std::uint64_t active_first_sequence = 0;
  std::uint64_t active_segment_bytes = 0;
  std::uint64_t active_segment_entries = 0;
  std::vector<SegmentInfo> segments;
};

Expected<std::vector<std::uint8_t>> encode_manifest(const StoreUuid& store, const Manifest& manifest);
Expected<Manifest> decode_manifest(const std::uint8_t* data, std::size_t length,
                                   const StoreUuid& store, std::string* failure);

struct LeaseRecord {
  StoreUuid store;
  WriterId writer;
  std::uint64_t epoch = 0;
  std::uint64_t process_id = 0;
  std::uint64_t floor_generation = 0;
  std::uint64_t written_unix_ms = 0;
};

Expected<std::vector<std::uint8_t>> encode_lease(const LeaseRecord& lease);
Expected<LeaseRecord> decode_lease(const std::uint8_t* data, std::size_t length,
                                   std::string* failure);
/// Reads and verifies the lease through an already-open handle. This is the
/// production path: an exclusive byte-range lock blocks reads from any other
/// handle, including a second handle in the same process, so the writer must
/// read the lease it holds authoritatively.
Expected<bool> read_lease_from(FileHandle& file, LeaseRecord* lease, std::string* failure);

/// Reads and verifies the lease by opening the file. Only usable when no writer
/// session holds the lease lock. A missing or malformed lease is reported as
/// ok=false rather than as a hard failure, because a store that was never
/// opened for writing has no lease yet.
Expected<bool> read_lease(const StoreLayout& layout, LeaseRecord* lease, std::string* failure);

/// True when the lease authorizes exactly this writer session. A lease that was
/// taken over by another writer (a higher epoch) never authorizes the older
/// session, which is how a writer that lost its lock is fenced out.
bool lease_authorizes(const LeaseRecord& lease, const WriterId& writer, WriterEpoch epoch) noexcept;

struct ManifestSlot {
  std::string name;
  bool present = false;
  bool valid = false;
  Manifest manifest;
  std::string failure;
};

/// Reads and verifies both manifest slots, returning the highest valid
/// generation. Refuses when neither slot is usable.
Expected<Manifest> load_best_manifest(const StoreLayout& layout, const StoreUuid& store,
                                      ManifestSlot* slot_a, ManifestSlot* slot_b);

std::vector<std::uint8_t> encode_segment_header(const StoreUuid& store, std::uint64_t segment_id,
                                                std::uint64_t first_sequence);
Expected<std::uint64_t> decode_segment_header(const std::uint8_t* data, std::size_t length,
                                              const StoreUuid& store, std::uint64_t expected_id,
                                              std::string* failure);

struct SegmentTrailer {
  std::uint64_t record_count = 0;
  std::uint64_t last_sequence = 0;
  Digest256 digest;
};

/// Segment seal trailer. It carries the count, the last sequence and the
/// SHA-256 over the whole committed segment prefix (header and records).
std::vector<std::uint8_t> encode_segment_trailer(std::uint64_t record_count,
                                                 std::uint64_t last_sequence,
                                                 const Digest256& digest);
Expected<SegmentTrailer> decode_segment_trailer(const std::uint8_t* data, std::size_t length,
                                                std::string* failure);

Digest256 compute_entry_hash(std::uint64_t sequence, const std::vector<std::uint8_t>& content);
Digest256 compute_chain_hash(const Digest256& previous_chain, const Digest256& entry_hash);

/// One framed record as read back from a segment.
struct FramedRecord {
  std::uint64_t offset = 0;
  std::uint32_t framed_length = 0;
  std::uint64_t sequence = 0;
  Digest256 prev_chain;
  Digest256 entry_hash;
  std::vector<std::uint8_t> content;
};

/// Reads and structurally verifies one record at the given offset. Verifies the
/// record magic, the bounded length, the CRC and the internal length fields.
Expected<FramedRecord> read_record_at(FileHandle& file, std::uint64_t offset,
                                      std::uint64_t max_record_bytes);

std::vector<std::uint8_t> encode_record(std::uint64_t sequence, const Digest256& prev_chain,
                                        const Digest256& entry_hash,
                                        const std::vector<std::uint8_t>& content);

/// Seeds the running segment digest: SHA-256(domain || 0x00 || segment bytes).
Sha256 make_segment_hasher();

std::string segment_file_name(std::uint64_t id);
/// Parses a segment file name back to its identifier. Foreign names are refused
/// so unrelated files in the directory are never interpreted as segments.
Expected<std::uint64_t> parse_segment_file_name(const std::string& name);

}  // namespace detail
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_SRC_STORE_HPP
