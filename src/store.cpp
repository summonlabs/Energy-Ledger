// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "byte_io.hpp"
#include "energy_ledger/canonical.hpp"

namespace energy_ledger {
namespace detail {
namespace {

bool matches(const std::uint8_t* data, std::size_t length, const std::uint8_t* magic,
             std::size_t magic_length) {
  if (length < magic_length) {
    return false;
  }
  for (std::size_t index = 0; index < magic_length; ++index) {
    if (data[index] != magic[index]) {
      return false;
    }
  }
  return true;
}

Digest256 digest_of(const std::uint8_t* data, std::size_t length) {
  return Sha256::hash(data, length);
}

/// Reads a stored 32 byte digest (not a hash of it).
Digest256 stored_digest(const std::uint8_t* data) {
  Digest256::Storage storage{};
  for (std::size_t index = 0; index < 32; ++index) {
    storage[index] = data[index];
  }
  return Digest256(storage);
}

/// Common fixed header validation shared by every durable structure.
Expected<void> validate_common_header(const std::uint8_t* data, std::size_t length,
                                      const std::uint8_t* magic, std::uint32_t expected_version,
                                      std::uint32_t expected_header_bytes, std::string* failure) {
  if (length < expected_header_bytes) {
    if (failure != nullptr) {
      *failure = "structure is shorter than its fixed header";
    }
    return make_error(StatusCode::StoreTruncated, "structure is shorter than its fixed header");
  }
  if (!matches(data, length, magic, 8)) {
    if (failure != nullptr) {
      *failure = "magic does not match";
    }
    return make_error(StatusCode::StoreFormatUnsupported, "magic does not match");
  }
  ByteReader reader(data + 8, length - 8);
  std::uint32_t version = 0;
  std::uint32_t header_bytes = 0;
  std::uint32_t endian = 0;
  std::uint32_t flags = 0;
  if (!reader.u32(version) || !reader.u32(header_bytes) || !reader.u32(endian) ||
      !reader.u32(flags)) {
    if (failure != nullptr) {
      *failure = "header fields are truncated";
    }
    return make_error(StatusCode::StoreTruncated, "header fields are truncated");
  }
  if (flags != 0) {
    if (failure != nullptr) {
      *failure = "header flags must be zero for this format version";
    }
    return make_error(StatusCode::StoreFormatUnsupported,
                      "header flags must be zero for this format version");
  }
  if (version != expected_version) {
    if (failure != nullptr) {
      *failure = "unsupported format version " + std::to_string(version);
    }
    return make_error(StatusCode::StoreFormatUnsupported,
                      "unsupported format version " + std::to_string(version));
  }
  if (header_bytes != expected_header_bytes) {
    if (failure != nullptr) {
      *failure = "header byte count is inconsistent with the format version";
    }
    return make_error(StatusCode::StoreFormatUnsupported, "header byte count mismatch");
  }
  if (endian != kEndianMarker) {
    if (failure != nullptr) {
      *failure = "endian marker mismatch";
    }
    return make_error(StatusCode::StoreEndianMismatch, "endian marker mismatch");
  }
  return Expected<void>();
}

void write_common_header(ByteWriter& writer, const std::uint8_t* magic, std::uint32_t version,
                         std::uint32_t header_bytes) {
  writer.bytes(magic, 8);
  writer.u32(version);
  writer.u32(header_bytes);
  writer.u32(kEndianMarker);
  writer.u32(0);  // flags
}

}  // namespace

const std::uint8_t kDescriptorMagic[8] = {'E', 'L', 'L', 'E', 'D', 'G', 'R', '1'};
const std::uint8_t kManifestMagic[8] = {'E', 'L', 'M', 'A', 'N', 'I', 'F', '1'};
const std::uint8_t kSegmentMagic[8] = {'E', 'L', 'S', 'E', 'G', '0', '0', '1'};
const std::uint8_t kTrailerMagic[8] = {'E', 'L', 'T', 'R', 'L', '0', '0', '1'};
const std::uint8_t kLeaseMagic[8] = {'E', 'L', 'L', 'E', 'A', 'S', 'E', '1'};
const std::uint8_t kRecordMagic[4] = {'E', 'L', 'R', '1'};

std::string segment_file_name(std::uint64_t id) {
  std::string name = "SEG-";
  std::string digits = std::to_string(id);
  for (std::size_t index = digits.size(); index < 20; ++index) {
    name.push_back('0');
  }
  name += digits;
  name += ".ELS";
  return name;
}

Expected<std::uint64_t> parse_segment_file_name(const std::string& name) {
  if (name.size() != 4 + 20 + 4 || name.compare(0, 4, "SEG-") != 0 ||
      name.compare(name.size() - 4, 4, ".ELS") != 0) {
    return make_error(StatusCode::InvalidArgument, "not a segment file name");
  }
  std::uint64_t value = 0;
  for (std::size_t index = 4; index < 4 + 20; ++index) {
    const char character = name[index];
    if (character < '0' || character > '9') {
      return make_error(StatusCode::InvalidArgument, "not a segment file name");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return make_error(StatusCode::InvalidArgument, "segment identifier overflows");
    }
    value = value * 10 + digit;
  }
  return value;
}

std::string StoreLayout::segment_file(std::uint64_t id) const {
  return join_path(segments_directory, segment_file_name(id));
}

Expected<StoreLayout> layout_for(const std::string& path, std::size_t max_path_bytes) {
  auto normalized = normalize_and_validate_path(path, max_path_bytes);
  if (!normalized) {
    return normalized.error();
  }
  StoreLayout layout;
  layout.root = normalized.value();
  layout.descriptor = join_path(layout.root, "LEDGER.STORE");
  layout.manifest_a = join_path(layout.root, "MANIFEST.A");
  layout.manifest_b = join_path(layout.root, "MANIFEST.B");
  layout.manifest_staging = join_path(layout.root, "MANIFEST.STG");
  layout.lease = join_path(layout.root, "WRITER.LEASE");
  layout.segments_directory = join_path(layout.root, "SEGMENTS");
  return layout;
}

Expected<Descriptor> decode_descriptor(const std::uint8_t* data, std::size_t length,
                                      std::string* failure) {
  auto header = validate_common_header(data, length, kDescriptorMagic, kStoreFormatVersion,
                                       static_cast<std::uint32_t>(kDescriptorHeaderBytes), failure);
  if (!header) {
    return header.error();
  }
  if (length != kDescriptorBytes) {
    if (failure != nullptr) {
      *failure = "descriptor length is not " + std::to_string(kDescriptorBytes) + " bytes";
    }
    return make_error(StatusCode::StoreTruncated, "descriptor length is not the fixed size");
  }
  const Digest256 stored = Sha256::hash(data, kDescriptorHeaderBytes);
  if (!(stored == stored_digest(data + kDescriptorHeaderBytes))) {
    if (failure != nullptr) {
      *failure = "descriptor digest mismatch";
    }
    return make_error(StatusCode::DigestMismatch, "descriptor digest mismatch");
  }
  ByteReader reader(data + 24, kDescriptorHeaderBytes - 24);
  Descriptor descriptor;
  std::uint8_t uuid_bytes[16];
  if (!reader.bytes(uuid_bytes, 16)) {
    return make_error(StatusCode::StoreTruncated, "descriptor store identifier is truncated");
  }
  StoreUuid::Storage storage{};
  for (std::size_t index = 0; index < 16; ++index) {
    storage[index] = uuid_bytes[index];
  }
  descriptor.store = StoreUuid(storage);
  std::uint32_t canonical_unit = 0;
  std::uint32_t reserved = 0;
  if (!reader.u32(canonical_unit) || !reader.u32(reserved)) {
    return make_error(StatusCode::StoreTruncated, "descriptor fields are truncated");
  }
  if (canonical_unit != 1) {
    if (failure != nullptr) {
      *failure = "unsupported canonical unit code " + std::to_string(canonical_unit);
    }
    return make_error(StatusCode::StoreFormatUnsupported, "unsupported canonical unit code");
  }
  descriptor.canonical_unit = canonical_unit;
  descriptor.version = kStoreFormatVersion;
  return descriptor;
}

Expected<std::vector<std::uint8_t>> encode_descriptor(const Descriptor& descriptor) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(kDescriptorBytes));
  ByteWriter writer(bytes);
  write_common_header(writer, kDescriptorMagic, kStoreFormatVersion,
                      static_cast<std::uint32_t>(kDescriptorHeaderBytes));
  writer.bytes(descriptor.store.storage().data(), 16);
  writer.u32(descriptor.canonical_unit);
  writer.u32(0);
  writer.u64(0);
  writer.u64(0);
  if (bytes.size() != kDescriptorHeaderBytes) {
    return make_error(StatusCode::InternalError, "descriptor header encoding is not 64 bytes");
  }
  const Digest256 digest = Sha256::hash(bytes.data(), bytes.size());
  writer.bytes(digest.storage().data(), digest.storage().size());
  if (bytes.size() != kDescriptorBytes) {
    return make_error(StatusCode::InternalError, "descriptor encoding is not 96 bytes");
  }
  return bytes;
}

Expected<Descriptor> read_descriptor(const StoreLayout& layout) {
  auto kind = inspect_path(layout.descriptor);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == PathKind::Missing) {
    return make_error(StatusCode::StoreNotFound, "store descriptor is missing: " + layout.descriptor);
  }
  if (kind.value() != PathKind::RegularFile) {
    return make_error(StatusCode::StorePathNotDirectory,
                      "store descriptor is not a regular file");
  }
  auto file = open_file(layout.descriptor, OpenMode::ReadOnly);
  if (!file) {
    return file.error();
  }
  auto bytes = read_exact(file.value(), 0, kDescriptorBytes, kDescriptorBytes);
  if (!bytes) {
    return bytes.error();
  }
  std::string failure;
  auto descriptor = decode_descriptor(bytes.value().data(), bytes.value().size(), &failure);
  if (!descriptor) {
    return make_error(descriptor.error().code, failure.empty() ? descriptor.error().detail : failure);
  }
  return descriptor.value();
}

Expected<std::vector<std::uint8_t>> encode_manifest(const StoreUuid& store,
                                                    const Manifest& manifest) {
  if (manifest.segments.size() > kMaxSegmentsPerManifest) {
    return make_error(StatusCode::LimitExceeded, "manifest segment table exceeds the format limit");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(kManifestHeaderBytes +
                                         (manifest.segments.size() * kManifestSegmentBytes) +
                                         kManifestTrailerBytes));
  ByteWriter writer(bytes);
  write_common_header(writer, kManifestMagic, kStoreFormatVersion,
                      static_cast<std::uint32_t>(kManifestHeaderBytes));
  writer.bytes(store.storage().data(), 16);
  writer.bytes(manifest.incarnation.storage().data(), 16);
  writer.u64(manifest.generation);
  writer.u64(manifest.head_sequence);
  writer.u64(manifest.entry_count);
  writer.u64(manifest.retired_floor_sequence);
  writer.u64(manifest.seq_at_floor);
  writer.bytes(manifest.chain_head.storage().data(), 32);
  writer.bytes(manifest.chain_at_floor.storage().data(), 32);
  writer.u64(manifest.writer_epoch);
  writer.u64(manifest.segments.size());
  writer.u64(manifest.active_segment_id);
  writer.u64(manifest.active_first_sequence);
  writer.u64(manifest.active_segment_bytes);
  writer.u64(manifest.active_segment_entries);
  writer.u64(0);
  writer.u64(0);
  if (bytes.size() != kManifestHeaderBytes) {
    return make_error(StatusCode::InternalError, "manifest header encoding is not 224 bytes");
  }
  for (const SegmentInfo& segment : manifest.segments) {
    writer.u64(segment.id);
    writer.u64(segment.first_sequence);
    writer.u64(segment.entry_count);
    writer.u64(segment.committed_bytes);
    writer.bytes(segment.digest.storage().data(), 32);
  }
  // Both integrity values cover exactly the bytes that precede the trailer.
  const std::uint32_t crc = crc32(bytes.data(), bytes.size());
  const Digest256 digest = Sha256::hash(bytes.data(), bytes.size());
  writer.bytes(kTrailerMagic, 8);  // shared trailer magic for all durable structures
  writer.u32(crc);
  writer.u32(0);
  writer.bytes(digest.storage().data(), 32);
  if (bytes.size() != kManifestHeaderBytes + (manifest.segments.size() * kManifestSegmentBytes) +
                          kManifestTrailerBytes) {
    return make_error(StatusCode::InternalError, "manifest encoding length is inconsistent");
  }
  return bytes;
}

Expected<Manifest> decode_manifest(const std::uint8_t* data, std::size_t length,
                                   const StoreUuid& store, std::string* failure) {
  auto header = validate_common_header(data, length, kManifestMagic, kStoreFormatVersion,
                                       static_cast<std::uint32_t>(kManifestHeaderBytes), failure);
  if (!header) {
    return header.error();
  }
  if (length < kManifestHeaderBytes + kManifestTrailerBytes) {
    if (failure != nullptr) {
      *failure = "manifest is shorter than header plus trailer";
    }
    return make_error(StatusCode::StoreTruncated, "manifest is shorter than header plus trailer");
  }

  ByteReader reader(data + 24, length - 24);
  std::uint8_t store_bytes[16];
  std::uint8_t incarnation_bytes[16];
  if (!reader.bytes(store_bytes, 16) || !reader.bytes(incarnation_bytes, 16)) {
    return make_error(StatusCode::StoreTruncated, "manifest identity fields are truncated");
  }
  StoreUuid manifest_store;
  StoreUuid::Storage store_storage{};
  LedgerIncarnation::Storage incarnation_storage{};
  for (std::size_t index = 0; index < 16; ++index) {
    store_storage[index] = store_bytes[index];
    incarnation_storage[index] = incarnation_bytes[index];
  }
  manifest_store = StoreUuid(store_storage);
  if (!(manifest_store == store)) {
    if (failure != nullptr) {
      *failure = "manifest belongs to a different store";
    }
    return make_error(StatusCode::StoreSwapped, "manifest belongs to a different store");
  }

  Manifest manifest;
  manifest.incarnation = LedgerIncarnation(incarnation_storage);
  std::uint64_t segment_count = 0;
  std::uint8_t chain_head_bytes[32];
  std::uint8_t chain_floor_bytes[32];
  if (!reader.u64(manifest.generation) || !reader.u64(manifest.head_sequence) ||
      !reader.u64(manifest.entry_count) || !reader.u64(manifest.retired_floor_sequence) ||
      !reader.u64(manifest.seq_at_floor) || !reader.bytes(chain_head_bytes, 32) ||
      !reader.bytes(chain_floor_bytes, 32) || !reader.u64(manifest.writer_epoch) ||
      !reader.u64(segment_count) || !reader.u64(manifest.active_segment_id) ||
      !reader.u64(manifest.active_first_sequence) || !reader.u64(manifest.active_segment_bytes) ||
      !reader.u64(manifest.active_segment_entries)) {
    return make_error(StatusCode::StoreTruncated, "manifest header is truncated");
  }
  std::uint64_t reserved_word0 = 0;
  std::uint64_t reserved_word1 = 0;
  if (!reader.u64(reserved_word0) || !reader.u64(reserved_word1)) {
    return make_error(StatusCode::StoreTruncated, "manifest reserved fields are truncated");
  }
  if (reserved_word0 != 0 || reserved_word1 != 0) {
    if (failure != nullptr) {
      *failure = "manifest reserved header fields must be zero";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest reserved header fields must be zero");
  }
  Digest256::Storage head_storage{};
  Digest256::Storage floor_storage{};
  for (std::size_t index = 0; index < 32; ++index) {
    head_storage[index] = chain_head_bytes[index];
    floor_storage[index] = chain_floor_bytes[index];
  }
  manifest.chain_head = Digest256(head_storage);
  manifest.chain_at_floor = Digest256(floor_storage);

  if (segment_count > kMaxSegmentsPerManifest) {
    if (failure != nullptr) {
      *failure = "manifest segment count exceeds the format limit";
    }
    return make_error(StatusCode::LimitExceeded, "manifest segment count exceeds the format limit");
  }
  const std::uint64_t expected_length =
      kManifestHeaderBytes + (segment_count * kManifestSegmentBytes) + kManifestTrailerBytes;
  if (length != expected_length) {
    if (failure != nullptr) {
      *failure = "manifest length " + std::to_string(length) + " does not match the declared " +
                 std::to_string(expected_length);
    }
    return make_error(StatusCode::StoreTruncated, "manifest length does not match its segment table");
  }

  const std::size_t trailer_offset = length - static_cast<std::size_t>(kManifestTrailerBytes);
  if (!matches(data + trailer_offset, kManifestTrailerBytes, kTrailerMagic, 8)) {
    if (failure != nullptr) {
      *failure = "manifest trailer magic mismatch";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest trailer magic mismatch");
  }
  ByteReader trailer_reader(data + trailer_offset + 8, kManifestTrailerBytes - 8);
  std::uint32_t stored_crc = 0;
  std::uint32_t reserved = 0;
  std::uint8_t digest_bytes[32];
  if (!trailer_reader.u32(stored_crc) || !trailer_reader.u32(reserved) ||
      !trailer_reader.bytes(digest_bytes, 32)) {
    return make_error(StatusCode::StoreTruncated, "manifest trailer is truncated");
  }
  if (reserved != 0) {
    if (failure != nullptr) {
      *failure = "manifest trailer reserved field must be zero";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest trailer reserved field must be zero");
  }
  if (stored_crc != crc32(data, trailer_offset)) {
    if (failure != nullptr) {
      *failure = "manifest CRC mismatch";
    }
    return make_error(StatusCode::CrcMismatch, "manifest CRC mismatch");
  }
  Digest256::Storage digest_storage{};
  for (std::size_t index = 0; index < 32; ++index) {
    digest_storage[index] = digest_bytes[index];
  }
  if (Digest256(digest_storage) != Sha256::hash(data, trailer_offset)) {
    if (failure != nullptr) {
      *failure = "manifest digest mismatch";
    }
    return make_error(StatusCode::DigestMismatch, "manifest digest mismatch");
  }

  manifest.segments.reserve(static_cast<std::size_t>(segment_count));
  std::uint64_t expected_sequence = manifest.retired_floor_sequence == 0
                                        ? 1
                                        : manifest.retired_floor_sequence + 1;
  std::uint64_t counted_entries = 0;
  std::uint64_t previous_id = 0;
  for (std::uint64_t index = 0; index < segment_count; ++index) {
    SegmentInfo segment;
    std::uint8_t segment_digest[32];
    if (!reader.u64(segment.id) || !reader.u64(segment.first_sequence) ||
        !reader.u64(segment.entry_count) || !reader.u64(segment.committed_bytes) ||
        !reader.bytes(segment_digest, 32)) {
      return make_error(StatusCode::StoreTruncated, "manifest segment table is truncated");
    }
    if (segment.entry_count == 0) {
      if (failure != nullptr) {
        *failure = "manifest lists a sealed segment with no entries";
      }
      return make_error(StatusCode::StoreCorrupt, "manifest lists a sealed segment with no entries");
    }
    if (segment.id <= previous_id) {
      if (failure != nullptr) {
        *failure = "manifest segment identifiers are not strictly increasing";
      }
      return make_error(StatusCode::SequenceReordered,
                        "manifest segment identifiers are not strictly increasing");
    }
    if (segment.first_sequence != expected_sequence) {
      if (failure != nullptr) {
        *failure = "manifest segment sequence ranges are not contiguous";
      }
      return make_error(StatusCode::SequenceGap, "manifest segment sequence ranges are not contiguous");
    }
    Digest256::Storage storage{};
    for (std::size_t byte = 0; byte < 32; ++byte) {
      storage[byte] = segment_digest[byte];
    }
    segment.digest = Digest256(storage);
    expected_sequence = segment.first_sequence + segment.entry_count;
    counted_entries += segment.entry_count;
    previous_id = segment.id;
    manifest.segments.push_back(segment);
  }

  if (manifest.active_segment_id == 0) {
    if (failure != nullptr) {
      *failure = "manifest has no active segment";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest has no active segment");
  }
  if (manifest.active_segment_id <= previous_id) {
    if (failure != nullptr) {
      *failure = "active segment identifier does not follow the sealed segments";
    }
    return make_error(StatusCode::SequenceReordered,
                      "active segment identifier does not follow the sealed segments");
  }
  if (manifest.active_segment_entries > 0) {
    if (manifest.active_first_sequence != expected_sequence) {
      if (failure != nullptr) {
        *failure = "active segment sequence range does not follow the sealed segments";
      }
      return make_error(StatusCode::SequenceGap, "active segment sequence range is not contiguous");
    }
    counted_entries += manifest.active_segment_entries;
  }
  if (counted_entries != manifest.entry_count) {
    if (failure != nullptr) {
      *failure = "manifest entry count does not match its segment table";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest entry count does not match its segment table");
  }
  if (manifest.seq_at_floor != manifest.retired_floor_sequence) {
    if (failure != nullptr) {
      *failure = "manifest retirement floor fields disagree";
    }
    return make_error(StatusCode::StoreCorrupt, "manifest retirement floor fields disagree");
  }
  if (manifest.chain_at_floor.is_zero() != (manifest.retired_floor_sequence == 0)) {
    if (failure != nullptr) {
      *failure = "manifest retirement chain anchor is inconsistent with the floor";
    }
    return make_error(StatusCode::StoreCorrupt,
                      "manifest retirement chain anchor is inconsistent with the floor");
  }
  std::uint64_t expected_head = manifest.retired_floor_sequence;
  if (!manifest.segments.empty()) {
    expected_head = manifest.segments.back().last_sequence();
  }
  if (manifest.active_segment_entries > 0) {
    expected_head = manifest.active_first_sequence + manifest.active_segment_entries - 1;
  }
  if (expected_head != manifest.head_sequence) {
    if (failure != nullptr) {
      *failure = "manifest head sequence does not match its entry accounting";
    }
    return make_error(StatusCode::StoreCorrupt,
                      "manifest head sequence does not match its entry accounting");
  }
  if (manifest.chain_head.is_zero() != (manifest.entry_count == 0)) {
    if (failure != nullptr) {
      *failure = "manifest chain head is inconsistent with the entry count";
    }
    return make_error(StatusCode::StoreCorrupt,
                      "manifest chain head is inconsistent with the entry count");
  }
  return manifest;
}

Expected<std::vector<std::uint8_t>> encode_lease(const LeaseRecord& lease) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(kLeaseBytes));
  ByteWriter writer(bytes);
  write_common_header(writer, kLeaseMagic, kStoreFormatVersion,
                      static_cast<std::uint32_t>(kLeaseHeaderBytes));
  writer.bytes(lease.store.storage().data(), 16);
  writer.bytes(lease.writer.storage().data(), 16);
  writer.u64(lease.epoch);
  writer.u64(lease.process_id);
  writer.u64(lease.floor_generation);
  writer.u64(lease.written_unix_ms);
  writer.u64(0);
  if (bytes.size() != kLeaseHeaderBytes) {
    return make_error(StatusCode::InternalError, "lease header encoding is not 96 bytes");
  }
  const Digest256 digest = Sha256::hash(bytes.data(), bytes.size());
  writer.bytes(digest.storage().data(), 32);
  if (bytes.size() != kLeaseBytes) {
    return make_error(StatusCode::InternalError, "lease encoding is not 128 bytes");
  }
  return bytes;
}

Expected<LeaseRecord> decode_lease(const std::uint8_t* data, std::size_t length,
                                  std::string* failure) {
  auto header = validate_common_header(data, length, kLeaseMagic, kStoreFormatVersion,
                                       static_cast<std::uint32_t>(kLeaseHeaderBytes), failure);
  if (!header) {
    return header.error();
  }
  if (length != kLeaseBytes) {
    if (failure != nullptr) {
      *failure = "lease length is not the fixed size";
    }
    return make_error(StatusCode::StoreTruncated, "lease length is not the fixed size");
  }
  if (!(Sha256::hash(data, kLeaseHeaderBytes) == stored_digest(data + kLeaseHeaderBytes))) {
    if (failure != nullptr) {
      *failure = "lease digest mismatch";
    }
    return make_error(StatusCode::DigestMismatch, "lease digest mismatch");
  }
  ByteReader reader(data + 24, kLeaseHeaderBytes - 24);
  std::uint8_t store_bytes[16];
  std::uint8_t writer_bytes[16];
  if (!reader.bytes(store_bytes, 16) || !reader.bytes(writer_bytes, 16)) {
    return make_error(StatusCode::StoreTruncated, "lease identity fields are truncated");
  }
  StoreUuid::Storage store_storage{};
  WriterId::Storage writer_storage{};
  for (std::size_t index = 0; index < 16; ++index) {
    store_storage[index] = store_bytes[index];
    writer_storage[index] = writer_bytes[index];
  }
  LeaseRecord lease;
  lease.store = StoreUuid(store_storage);
  lease.writer = WriterId(writer_storage);
  if (!reader.u64(lease.epoch) || !reader.u64(lease.process_id) ||
      !reader.u64(lease.floor_generation) || !reader.u64(lease.written_unix_ms)) {
    return make_error(StatusCode::StoreTruncated, "lease fields are truncated");
  }
  return lease;
}

Expected<bool> read_lease_from(FileHandle& file, LeaseRecord* lease, std::string* failure) {
  auto bytes = read_exact(file, 0, kLeaseBytes, kLeaseBytes);
  if (!bytes) {
    if (failure != nullptr) {
      *failure = bytes.error().detail;
    }
    return false;
  }
  auto decoded = decode_lease(bytes.value().data(), bytes.value().size(), failure);
  if (!decoded) {
    return false;
  }
  if (lease != nullptr) {
    *lease = decoded.value();
  }
  return true;
}

bool lease_authorizes(const LeaseRecord& lease, const WriterId& writer, WriterEpoch epoch) noexcept {
  return lease.epoch == epoch.value() && lease.writer == writer;
}

Expected<bool> read_lease(const StoreLayout& layout, LeaseRecord* lease, std::string* failure) {
  auto kind = inspect_path(layout.lease);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == PathKind::Missing) {
    if (failure != nullptr) {
      *failure = "lease file is absent";
    }
    return false;
  }
  if (kind.value() != PathKind::RegularFile) {
    if (failure != nullptr) {
      *failure = "lease path is not a regular file";
    }
    return make_error(StatusCode::StorePathUnsafe, "lease path is not a regular file");
  }
  auto file = open_file(layout.lease, OpenMode::ReadOnly);
  if (!file) {
    if (failure != nullptr) {
      *failure = file.error().detail;
    }
    return false;
  }
  return read_lease_from(file.value(), lease, failure);
}

namespace {

Expected<ManifestSlot> read_slot(const StoreLayout& layout, const std::string& file_name,
                                 const std::string& path, const StoreUuid& store) {
  ManifestSlot slot;
  slot.name = file_name;
  auto kind = inspect_path(path);
  if (!kind) {
    slot.failure = kind.error().detail;
    return slot;
  }
  if (kind.value() == PathKind::Missing) {
    slot.failure = "slot is absent";
    return slot;
  }
  // The path exists: report it as present even when it is unusable, so the
  // verification path can surface the reason instead of silently ignoring it.
  slot.present = true;
  if (kind.value() != PathKind::RegularFile) {
    slot.failure = "slot is not a regular file";
    return slot;
  }
  auto file = open_file(path, OpenMode::ReadOnly);
  if (!file) {
    slot.failure = file.error().detail;
    return slot;
  }
  auto size = file_size(file.value());
  if (!size) {
    slot.failure = size.error().detail;
    return slot;
  }
  if (size.value() > kMaxManifestBytes) {
    slot.failure = "slot exceeds the maximum manifest size";
    return slot;
  }
  auto bytes = read_exact(file.value(), 0, size.value(), kMaxManifestBytes);
  if (!bytes) {
    slot.failure = bytes.error().detail;
    return slot;
  }
  std::string failure;
  auto manifest = decode_manifest(bytes.value().data(), bytes.value().size(), store, &failure);
  if (!manifest) {
    slot.failure = failure.empty() ? manifest.error().detail : failure;
    return slot;
  }
  slot.valid = true;
  slot.manifest = manifest.value();
  (void)layout;
  return slot;
}

}  // namespace

Expected<Manifest> load_best_manifest(const StoreLayout& layout, const StoreUuid& store,
                                      ManifestSlot* slot_a, ManifestSlot* slot_b) {
  auto first = read_slot(layout, "MANIFEST.A", layout.manifest_a, store);
  if (!first) {
    return first.error();
  }
  auto second = read_slot(layout, "MANIFEST.B", layout.manifest_b, store);
  if (!second) {
    return second.error();
  }
  if (slot_a != nullptr) {
    *slot_a = first.value();
  }
  if (slot_b != nullptr) {
    *slot_b = second.value();
  }
  const bool a_ok = first.value().valid;
  const bool b_ok = second.value().valid;
  if (!a_ok && !b_ok) {
    std::string detail = "no valid manifest generation: A[" + first.value().failure + "] B[" +
                         second.value().failure + "]";
    return make_error(StatusCode::ManifestMissing, detail);
  }
  if (a_ok && b_ok) {
    return first.value().manifest.generation >= second.value().manifest.generation
               ? first.value().manifest
               : second.value().manifest;
  }
  return a_ok ? first.value().manifest : second.value().manifest;
}

std::vector<std::uint8_t> encode_segment_header(const StoreUuid& store, std::uint64_t segment_id,
                                                std::uint64_t first_sequence) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(kSegmentHeaderBytes));
  ByteWriter writer(bytes);
  write_common_header(writer, kSegmentMagic, kStoreFormatVersion,
                      static_cast<std::uint32_t>(kSegmentHeaderBytes));
  writer.bytes(store.storage().data(), 16);
  writer.u64(segment_id);
  writer.u64(first_sequence);
  writer.u64(0);
  return bytes;
}

Expected<std::uint64_t> decode_segment_header(const std::uint8_t* data, std::size_t length,
                                              const StoreUuid& store, std::uint64_t expected_id,
                                              std::string* failure) {
  auto header = validate_common_header(data, length, kSegmentMagic, kStoreFormatVersion,
                                       static_cast<std::uint32_t>(kSegmentHeaderBytes), failure);
  if (!header) {
    return header.error();
  }
  if (length < kSegmentHeaderBytes) {
    return make_error(StatusCode::StoreTruncated, "segment header is truncated");
  }
  ByteReader reader(data + 24, kSegmentHeaderBytes - 24);
  std::uint8_t store_bytes[16];
  if (!reader.bytes(store_bytes, 16)) {
    return make_error(StatusCode::StoreTruncated, "segment identity is truncated");
  }
  StoreUuid::Storage storage{};
  for (std::size_t index = 0; index < 16; ++index) {
    storage[index] = store_bytes[index];
  }
  if (!(StoreUuid(storage) == store)) {
    if (failure != nullptr) {
      *failure = "segment belongs to a different store";
    }
    return make_error(StatusCode::StoreSwapped, "segment belongs to a different store");
  }
  std::uint64_t segment_id = 0;
  std::uint64_t first_sequence = 0;
  std::uint64_t reserved = 0;
  if (!reader.u64(segment_id) || !reader.u64(first_sequence) || !reader.u64(reserved)) {
    return make_error(StatusCode::StoreTruncated, "segment header fields are truncated");
  }
  if (reserved != 0) {
    if (failure != nullptr) {
      *failure = "segment header reserved field must be zero";
    }
    return make_error(StatusCode::StoreCorrupt, "segment header reserved field must be zero");
  }
  if (segment_id != expected_id) {
    if (failure != nullptr) {
      *failure = "segment file name does not match its embedded identifier";
    }
    return make_error(StatusCode::StoreSwapped,
                      "segment file name does not match its embedded identifier");
  }
  return first_sequence;
}

std::vector<std::uint8_t> encode_segment_trailer(std::uint64_t record_count,
                                                 std::uint64_t last_sequence,
                                                 const Digest256& digest) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(kSegmentTrailerBytes));
  ByteWriter writer(bytes);
  writer.bytes(kTrailerMagic, 8);
  writer.u64(record_count);
  writer.u64(last_sequence);
  writer.u64(0);
  writer.u64(0);
  writer.bytes(digest.storage().data(), 32);
  return bytes;
}

Expected<SegmentTrailer> decode_segment_trailer(const std::uint8_t* data, std::size_t length,
                                                std::string* failure) {
  if (length != kSegmentTrailerBytes) {
    if (failure != nullptr) {
      *failure = "segment trailer is not the fixed size";
    }
    return make_error(StatusCode::StoreTruncated, "segment trailer is not the fixed size");
  }
  if (!matches(data, length, kTrailerMagic, 8)) {
    if (failure != nullptr) {
      *failure = "segment trailer magic mismatch";
    }
    return make_error(StatusCode::StoreCorrupt, "segment trailer magic mismatch");
  }
  ByteReader reader(data + 8, length - 8);
  std::uint64_t reserved0 = 0;
  std::uint64_t reserved1 = 0;
  std::uint8_t digest_bytes[32];
  SegmentTrailer trailer;
  if (!reader.u64(trailer.record_count) || !reader.u64(trailer.last_sequence) ||
      !reader.u64(reserved0) || !reader.u64(reserved1) || !reader.bytes(digest_bytes, 32)) {
    if (failure != nullptr) {
      *failure = "segment trailer is truncated";
    }
    return make_error(StatusCode::StoreTruncated, "segment trailer is truncated");
  }
  if (reserved0 != 0 || reserved1 != 0) {
    if (failure != nullptr) {
      *failure = "segment trailer reserved fields must be zero";
    }
    return make_error(StatusCode::StoreCorrupt, "segment trailer reserved fields must be zero");
  }
  Digest256::Storage storage{};
  for (std::size_t index = 0; index < 32; ++index) {
    storage[index] = digest_bytes[index];
  }
  trailer.digest = Digest256(storage);
  return trailer;
}

Digest256 compute_entry_hash(std::uint64_t sequence, const std::vector<std::uint8_t>& content) {
  Sha256 hasher;
  std::uint8_t prefix[8];
  for (int index = 0; index < 8; ++index) {
    prefix[index] = static_cast<std::uint8_t>((sequence >> (8 * index)) & 0xFFu);
  }
  hasher.update(kEntryHashDomain.data(), kEntryHashDomain.size());
  const std::uint8_t separator = 0;
  hasher.update(&separator, 1);
  hasher.update(prefix, sizeof(prefix));
  hasher.update(content.data(), content.size());
  return hasher.finish();
}

Digest256 compute_chain_hash(const Digest256& previous_chain, const Digest256& entry_hash) {
  Sha256 hasher;
  hasher.update(kChainDomain.data(), kChainDomain.size());
  const std::uint8_t separator = 0;
  hasher.update(&separator, 1);
  hasher.update(previous_chain.storage().data(), 32);
  hasher.update(entry_hash.storage().data(), 32);
  return hasher.finish();
}

std::vector<std::uint8_t> encode_record(std::uint64_t sequence, const Digest256& prev_chain,
                                        const Digest256& entry_hash,
                                        const std::vector<std::uint8_t>& content) {
  std::vector<std::uint8_t> body;
  body.reserve(content.size() + 80);
  ByteWriter body_writer(body);
  body_writer.u64(sequence);
  body_writer.bytes(prev_chain.storage().data(), 32);
  body_writer.bytes(entry_hash.storage().data(), 32);
  body_writer.u32(static_cast<std::uint32_t>(content.size()));
  body_writer.bytes(content.data(), content.size());

  std::vector<std::uint8_t> frame;
  frame.reserve(body.size() + 12);
  ByteWriter writer(frame);
  writer.bytes(kRecordMagic, 4);
  writer.u32(static_cast<std::uint32_t>(body.size()));
  writer.u32(crc32(body.data(), body.size()));
  writer.bytes(body.data(), body.size());
  return frame;
}

Expected<FramedRecord> read_record_at(FileHandle& file, std::uint64_t offset,
                                      std::uint64_t max_record_bytes) {
  auto header = read_exact(file, offset, kRecordHeaderBytes, kRecordHeaderBytes);
  if (!header) {
    return header.error();
  }
  if (!matches(header.value().data(), kRecordHeaderBytes, kRecordMagic, 4)) {
    return make_error(StatusCode::RecordFramingInvalid,
                      "record magic mismatch at offset " + std::to_string(offset));
  }
  ByteReader reader(header.value().data() + 4, kRecordHeaderBytes - 4);
  std::uint32_t body_length = 0;
  std::uint32_t stored_crc = 0;
  if (!reader.u32(body_length) || !reader.u32(stored_crc)) {
    return make_error(StatusCode::StoreTruncated, "record header is truncated");
  }
  if (body_length < 76 || body_length + kRecordHeaderBytes > max_record_bytes) {
    return make_error(StatusCode::RecordFramingInvalid,
                      "record body length " + std::to_string(body_length) +
                          " is outside the permitted range");
  }
  auto body = read_exact(file, offset + kRecordHeaderBytes, body_length, max_record_bytes);
  if (!body) {
    return body.error();
  }
  if (crc32(body.value().data(), body.value().size()) != stored_crc) {
    return make_error(StatusCode::CrcMismatch,
                      "record CRC mismatch at offset " + std::to_string(offset));
  }
  ByteReader body_reader(body.value().data(), body.value().size());
  FramedRecord record;
  record.offset = offset;
  record.framed_length = static_cast<std::uint32_t>(body_length + kRecordHeaderBytes);
  std::uint8_t prev_bytes[32];
  std::uint8_t hash_bytes[32];
  std::uint32_t content_length = 0;
  if (!body_reader.u64(record.sequence) || !body_reader.bytes(prev_bytes, 32) ||
      !body_reader.bytes(hash_bytes, 32) || !body_reader.u32(content_length)) {
    return make_error(StatusCode::StoreTruncated, "record body is truncated");
  }
  if (content_length > ModelLimits::kMaxContentBytes ||
      static_cast<std::uint64_t>(content_length) + 76 != body_length) {
    return make_error(StatusCode::RecordFramingInvalid,
                      "record content length is inconsistent with the body length");
  }
  Digest256::Storage prev_storage{};
  Digest256::Storage hash_storage{};
  for (std::size_t index = 0; index < 32; ++index) {
    prev_storage[index] = prev_bytes[index];
    hash_storage[index] = hash_bytes[index];
  }
  record.prev_chain = Digest256(prev_storage);
  record.entry_hash = Digest256(hash_storage);
  record.content.resize(content_length);
  if (content_length > 0 && !body_reader.bytes(record.content.data(), content_length)) {
    return make_error(StatusCode::StoreTruncated, "record content is truncated");
  }
  if (body_reader.remaining() != 0) {
    return make_error(StatusCode::RecordFramingInvalid, "record body has trailing bytes");
  }
  return record;
}

Sha256 make_segment_hasher() {
  Sha256 hasher;
  hasher.update(kSegmentDigestDomain.data(), kSegmentDigestDomain.size());
  const std::uint8_t separator = 0;
  hasher.update(&separator, 1);
  return hasher;
}

}  // namespace detail
}  // namespace energy_ledger
