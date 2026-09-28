// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Mutation path: store initialization, recovery, the append/commit protocol,
// writer authority and retention.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "byte_io.hpp"
#include "energy_ledger/canonical.hpp"
#include "ledger_internal.hpp"

namespace energy_ledger {
namespace {

using namespace detail;

constexpr std::uint64_t kMinSegmentBytes = 4096;
constexpr std::uint64_t kMaxSegmentBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxLedgerBytes = 1024ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxEntryLimit = 4000000;
constexpr std::size_t kMaxIdempotencyWindow = 1000000;

Expected<void> validate_limits(const StoreLimits& limits) {
  if (limits.max_entries == 0 || limits.max_entries > kMaxEntryLimit) {
    return make_error(StatusCode::LimitExceeded,
                      "max_entries must be within 1.." + std::to_string(kMaxEntryLimit));
  }
  if (limits.max_segment_bytes < kMinSegmentBytes || limits.max_segment_bytes > kMaxSegmentBytes) {
    return make_error(StatusCode::LimitExceeded,
                      "max_segment_bytes must be within " + std::to_string(kMinSegmentBytes) +
                          ".." + std::to_string(kMaxSegmentBytes));
  }
  if (limits.max_ledger_bytes < limits.max_segment_bytes || limits.max_ledger_bytes > kMaxLedgerBytes) {
    return make_error(StatusCode::LimitExceeded,
                      "max_ledger_bytes must be at least max_segment_bytes and at most " +
                          std::to_string(kMaxLedgerBytes));
  }
  if (limits.idempotency_window > kMaxIdempotencyWindow) {
    return make_error(StatusCode::LimitExceeded,
                      "idempotency_window exceeds " + std::to_string(kMaxIdempotencyWindow));
  }
  if (limits.max_request_id_bytes == 0 || limits.max_request_id_bytes > 256) {
    return make_error(StatusCode::LimitExceeded, "max_request_id_bytes must be within 1..256");
  }
  if (limits.max_path_bytes == 0 || limits.max_path_bytes > 4096) {
    return make_error(StatusCode::LimitExceeded, "max_path_bytes must be within 1..4096");
  }
  return Expected<void>();
}

void inject(const FaultInjection& fault, FaultInjection::Stage stage) {
  if (fault.terminate_at == stage) {
    terminate_process_now(fault.exit_code);
  }
}

/// Size of a segment file without holding it open.
Expected<std::uint64_t> file_size_of_segment(const StoreLayout& layout, std::uint64_t segment_id) {
  auto file = open_file(layout.segment_file(segment_id), OpenMode::ReadOnly);
  if (!file) {
    return file.error();
  }
  return file_size(file.value());
}

/// Removes segment files that no committed manifest generation references.
/// They can only be the residue of an interrupted rotation and are never read.
Expected<std::uint64_t> discard_orphan_segments(Ledger::Impl& impl, bool discard) {
  auto entries = list_directory_entries(impl.layout.segments_directory);
  if (!entries) {
    return entries.error();
  }
  std::uint64_t orphans = 0;
  for (const std::string& name : entries.value()) {
    auto identifier = parse_segment_file_name(name);
    if (!identifier) {
      continue;
    }
    bool known = identifier.value() == impl.manifest.active_segment_id;
    for (const SegmentInfo& segment : impl.manifest.segments) {
      if (segment.id == identifier.value()) {
        known = true;
        break;
      }
    }
    if (known) {
      continue;
    }
    ++orphans;
    if (discard) {
      auto removed = delete_file(join_path(impl.layout.segments_directory, name));
      if (!removed) {
        return removed.error();
      }
    }
  }
  return orphans;
}

Expected<void> read_back_and_compare(FileHandle& file, const std::vector<std::uint8_t>& expected,
                                     const char* what) {
  auto size = file_size(file);
  if (!size) {
    return size.error();
  }
  if (size.value() != expected.size()) {
    return make_error(StatusCode::StoreTruncated,
                      std::string(what) + " size after write is " + std::to_string(size.value()) +
                          " but " + std::to_string(expected.size()) + " bytes were written");
  }
  auto read = read_exact(file, 0, size.value(), static_cast<std::uint64_t>(expected.size()));
  if (!read) {
    return read.error();
  }
  if (read.value() != expected) {
    return make_error(StatusCode::DigestMismatch,
                      std::string(what) + " read-back does not match the bytes written");
  }
  return Expected<void>();
}

/// Publication protocol: stage, flush, verify, publish the generation slot,
/// flush (the commit point), verify. Never reports success before the slot is
/// durable and re-verified.
Expected<void> publish_manifest(Ledger::Impl& impl, const Manifest& next,
                                const FaultInjection& fault) {
  auto encoded = encode_manifest(impl.descriptor.store, next);
  if (!encoded) {
    return encoded.error();
  }
  const std::vector<std::uint8_t>& bytes = encoded.value();

  {
    auto staging = open_file(impl.layout.manifest_staging, OpenMode::ReadWriteCreate);
    if (!staging) {
      return staging.error();
    }
    auto truncated = truncate_file(staging.value(), 0);
    if (!truncated) {
      return truncated.error();
    }
    auto written = write_all(staging.value(), bytes.data(), bytes.size());
    if (!written) {
      return written.error();
    }
    inject(fault, FaultInjection::Stage::AfterStagingManifestWrite);
    auto flushed = flush_durable(staging.value());
    if (!flushed) {
      return flushed.error();
    }
    inject(fault, FaultInjection::Stage::AfterStagingManifestFlush);
    auto verified = read_back_and_compare(staging.value(), bytes, "staging manifest");
    if (!verified) {
      return verified.error();
    }
  }

  const std::string& slot_path = (next.generation % 2 == 1) ? impl.layout.manifest_a
                                                             : impl.layout.manifest_b;
  auto slot = open_file(slot_path, OpenMode::ReadWriteCreate);
  if (!slot) {
    return slot.error();
  }
  auto truncated = truncate_file(slot.value(), 0);
  if (!truncated) {
    return truncated.error();
  }
  auto written = write_all(slot.value(), bytes.data(), bytes.size());
  if (!written) {
    return written.error();
  }
  inject(fault, FaultInjection::Stage::AfterSlotWrite);
  auto flushed = flush_durable(slot.value());
  if (!flushed) {
    return flushed.error();
  }
  inject(fault, FaultInjection::Stage::AfterSlotFlush);
  auto verified = read_back_and_compare(slot.value(), bytes, "manifest slot");
  if (!verified) {
    return verified.error();
  }
  return Expected<void>();
}

Expected<void> rollback_staged(Ledger::Impl& impl, const StagedMutation& staged) {
  if (!staged.armed) {
    return Expected<void>();
  }
  if (staged.rotated) {
    const std::uint64_t created_id = impl.manifest.active_segment_id;
    impl.active_file.close();
    auto removed = delete_file(impl.layout.segment_file(created_id));
    if (!removed) {
      impl.poisoned = true;
      return removed.error();
    }
    const std::string previous_path = impl.layout.segment_file(staged.previous_active_id);
    auto reopened = open_file(previous_path, OpenMode::ReadWriteExisting);
    if (!reopened) {
      impl.poisoned = true;
      return reopened.error();
    }
    impl.active_file = std::move(reopened).value();
  }
  auto truncated = truncate_file(impl.active_file, staged.previous_active_bytes);
  if (!truncated) {
    impl.poisoned = true;
    return truncated.error();
  }
  auto flushed = flush_durable(impl.active_file);
  if (!flushed) {
    impl.poisoned = true;
    return flushed.error();
  }
  impl.manifest = staged.previous_manifest;
  impl.segment_hasher = staged.previous_hasher;
  return Expected<void>();
}

Expected<void> rotate_segment(Ledger::Impl& impl, StagedMutation& staged) {
  const Digest256 digest = impl.segment_hasher.finish();
  auto trailer = encode_segment_trailer(impl.manifest.active_segment_entries,
                                        impl.manifest.head_sequence, digest);
  auto written = write_all(impl.active_file, trailer.data(), trailer.size());
  if (!written) {
    return written.error();
  }
  auto flushed = flush_durable(impl.active_file);
  if (!flushed) {
    return flushed.error();
  }

  SegmentInfo sealed;
  sealed.id = impl.manifest.active_segment_id;
  sealed.first_sequence = impl.manifest.active_first_sequence;
  sealed.entry_count = impl.manifest.active_segment_entries;
  sealed.committed_bytes = impl.manifest.active_segment_bytes + trailer.size();
  sealed.digest = digest;
  impl.manifest.segments.push_back(sealed);
  staged.rotated = true;
  impl.active_file.close();

  const std::uint64_t new_id = sealed.id + 1;
  const std::uint64_t first_sequence = impl.manifest.head_sequence + 1;
  auto created = open_file(impl.layout.segment_file(new_id), OpenMode::ReadWriteCreate);
  if (!created) {
    impl.poisoned = true;
    return created.error();
  }
  auto truncated = truncate_file(created.value(), 0);
  if (!truncated) {
    impl.poisoned = true;
    return truncated.error();
  }
  auto header = encode_segment_header(impl.descriptor.store, new_id, first_sequence);
  auto header_written = write_all(created.value(), header.data(), header.size());
  if (!header_written) {
    impl.poisoned = true;
    return header_written.error();
  }
  auto header_flushed = flush_durable(created.value());
  if (!header_flushed) {
    impl.poisoned = true;
    return header_flushed.error();
  }

  impl.active_file = std::move(created).value();
  impl.manifest.active_segment_id = new_id;
  impl.manifest.active_first_sequence = first_sequence;
  impl.manifest.active_segment_bytes = header.size();
  impl.manifest.active_segment_entries = 0;
  impl.segment_hasher = make_segment_hasher();
  impl.segment_hasher.update(header.data(), header.size());
  return Expected<void>();
}

Expected<void> write_lease(Ledger::Impl& impl, WriterEpoch epoch, std::uint64_t floor_generation) {
  LeaseRecord lease;
  lease.store = impl.descriptor.store;
  lease.writer = derive_writer_id(impl.descriptor.store, epoch);
  lease.epoch = epoch.value();
  lease.process_id = current_process_id();
  lease.floor_generation = floor_generation;
  lease.written_unix_ms = static_cast<std::uint64_t>(unix_time_milliseconds());
  auto encoded = encode_lease(lease);
  if (!encoded) {
    return encoded.error();
  }
  auto truncated = truncate_file(impl.lease_file, 0);
  if (!truncated) {
    return truncated.error();
  }
  auto written = write_all(impl.lease_file, encoded.value().data(), encoded.value().size());
  if (!written) {
    return written.error();
  }
  auto flushed = flush_durable(impl.lease_file);
  if (!flushed) {
    return flushed.error();
  }
  auto verified = read_back_and_compare(impl.lease_file, encoded.value(), "writer lease");
  if (!verified) {
    return verified.error();
  }
  impl.writer_epoch = epoch;
  impl.writer_id = lease.writer;
  impl.floor_generation = floor_generation;
  return Expected<void>();
}

/// Streams a region of a file into a hasher in bounded chunks, so a segment
/// digest never requires materialising the whole segment in memory.
Expected<void> hash_region(FileHandle& file, std::uint64_t offset, std::uint64_t length,
                           Sha256& hasher, std::uint64_t max_bytes) {
  if (length > max_bytes) {
    return make_error(StatusCode::LimitExceeded, "hashed region exceeds the configured limit");
  }
  constexpr std::uint64_t kChunk = 1u * 1024u * 1024u;
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min(kChunk, std::max<std::uint64_t>(length, 1))));
  std::uint64_t consumed = 0;
  while (consumed < length) {
    const std::uint64_t request = std::min(kChunk, length - consumed);
    auto read = read_at(file, offset + consumed, buffer.data(), static_cast<std::size_t>(request));
    if (!read) {
      return read.error();
    }
    if (read.value() == 0) {
      return make_error(StatusCode::StoreTruncated, "file ended while hashing a committed region");
    }
    hasher.update(buffer.data(), read.value());
    consumed += read.value();
  }
  return Expected<void>();
}

/// Reads one segment, verifies its framing and digest, and appends every record
/// to the in-memory index while walking the integrity chain.
struct ChainWalker {
  Ledger::Impl& impl;
  Digest256 previous_chain;
  std::uint64_t expected_sequence = 0;
  std::uint64_t entries_seen = 0;
  std::uint64_t bytes_seen = 0;
  bool verify_records = true;

  Expected<void> add_record(FileHandle& file, std::uint64_t offset, std::uint64_t segment_id) {
    auto frame = read_record_at(file, offset, kMaxRecordBytes);
    if (!frame) {
      return frame.error();
    }
    FramedRecord& record = frame.value();
    if (record.sequence != expected_sequence) {
      return make_error(record.sequence < expected_sequence ? StatusCode::SequenceReordered
                                                            : StatusCode::SequenceGap,
                        "committed sequence " + std::to_string(record.sequence) +
                            " does not follow " + std::to_string(expected_sequence - 1));
    }
    if (!(record.prev_chain == previous_chain)) {
      return make_error(StatusCode::ChainBroken,
                        "entry " + std::to_string(record.sequence) +
                            " does not chain to its predecessor");
    }
    const Digest256 recomputed = compute_entry_hash(record.sequence, record.content);
    if (recomputed != record.entry_hash) {
      return make_error(StatusCode::DigestMismatch,
                        "entry " + std::to_string(record.sequence) +
                            " content digest does not match its stored entry hash");
    }
    auto decoded = decode_content(record.content.data(), record.content.size());
    if (!decoded) {
      return make_error(decoded.error().code,
                        "entry " + std::to_string(record.sequence) + ": " +
                            decoded.error().detail);
    }
    const EntryContent& entry = decoded.value();
    auto identity = event_identity(entry);
    if (!identity) {
      return identity.error();
    }
    auto digest = content_digest(entry);
    if (!digest) {
      return digest.error();
    }

    IndexRecord index;
    index.sequence = record.sequence;
    index.segment_id = segment_id;
    index.record_offset = offset;
    index.record_length = record.framed_length;
    index.content_length = static_cast<std::uint32_t>(record.content.size());
    index.object = impl.intern(entry.object.value());
    index.generation = impl.intern(entry.generation.value());
    index.interval = impl.intern(entry.interval.value());
    index.clock_domain = impl.intern(entry.clock_domain.value());
    index.source_family = impl.intern(entry.source_family.value());
    index.source_instance = impl.intern(entry.source_instance.value());
    index.authority = impl.intern(entry.authority.value());
    index.source_revision = entry.source_revision.value();
    index.source_generation = entry.source_generation.value();
    index.source_epoch = entry.source_epoch.value();
    index.target = entry.target.value();
    index.quantity = entry.quantity.joules();
    index.declared_value = entry.declared_value;
    index.interval_start = entry.interval_start.value();
    index.interval_end = entry.interval_end.value();
    index.observed_at = entry.observed_at.value();
    index.kind = static_cast<std::uint8_t>(entry.kind);
    index.quality = static_cast<std::uint8_t>(entry.quality);
    index.time_basis = static_cast<std::uint8_t>(entry.time_basis);
    index.authority_tier = entry.authority_tier.value();
    index.declared_unit = static_cast<std::uint8_t>(entry.declared_unit);
    index.content_digest = digest.value();
    index.entry_hash = record.entry_hash;
    index.prev_chain = record.prev_chain;
    index.chain_hash = compute_chain_hash(previous_chain, record.entry_hash);
    index.event_id = identity.value();

    if (impl.event_index.find(index.event_id) != impl.event_index.end()) {
      return make_error(StatusCode::DuplicateSequence,
                        "entry " + std::to_string(record.sequence) +
                            " repeats an event identity already committed in this incarnation");
    }
    impl.event_index.emplace(index.event_id, index.sequence);
    impl.records.push_back(index);

    previous_chain = index.chain_hash;
    ++expected_sequence;
    ++entries_seen;
    bytes_seen += record.framed_length;
    return Expected<void>();
  }

  Expected<std::uint64_t> walk_segment(const std::string& path, std::uint64_t expected_segment_id,
                                       std::uint64_t committed_bytes, std::uint64_t end_offset,
                                       const Digest256* expected_digest, bool read_write) {
    auto file = open_file(path, read_write ? OpenMode::ReadWriteExisting : OpenMode::ReadOnly);
    if (!file) {
      return file.error();
    }
    auto size = file_size(file.value());
    if (!size) {
      return size.error();
    }
    if (size.value() < committed_bytes) {
      return make_error(StatusCode::StoreTruncated,
                        "segment " + std::to_string(expected_segment_id) + " holds " +
                            std::to_string(size.value()) + " bytes but the manifest commits " +
                            std::to_string(committed_bytes));
    }
    auto header = read_exact(file.value(), 0, kSegmentHeaderBytes, kSegmentHeaderBytes);
    if (!header) {
      return header.error();
    }
    std::string failure;
    auto first_sequence = decode_segment_header(header.value().data(), header.value().size(),
                                                impl.descriptor.store, expected_segment_id, &failure);
    if (!first_sequence) {
      return make_error(first_sequence.error().code,
                        failure.empty() ? first_sequence.error().detail : failure);
    }
    if (first_sequence.value() != 0) {
      const std::uint64_t expected_first = expected_sequence;
      if (first_sequence.value() != expected_first) {
        return make_error(StatusCode::SequenceGap,
                          "segment " + std::to_string(expected_segment_id) +
                              " declares first sequence " + std::to_string(first_sequence.value()) +
                              " but the chain expects " + std::to_string(expected_first));
      }
    }

    if (expected_digest != nullptr && verify_records) {
      Sha256 hasher = make_segment_hasher();
      auto hashed = hash_region(file.value(), 0, end_offset, hasher, kMaxLedgerBytes);
      if (!hashed) {
        return hashed.error();
      }
      if (hasher.finish() != *expected_digest) {
        return make_error(StatusCode::DigestMismatch,
                          "segment " + std::to_string(expected_segment_id) +
                              " digest does not match the manifest");
      }
    }

    std::uint64_t offset = kSegmentHeaderBytes;
    while (offset < end_offset) {
      const std::uint64_t remaining = end_offset - offset;
      if (remaining < kRecordHeaderBytes) {
        return make_error(StatusCode::StoreTruncated,
                          "segment " + std::to_string(expected_segment_id) +
                              " ends inside a record header");
      }
      auto added = add_record(file.value(), offset, expected_segment_id);
      if (!added) {
        return added.error();
      }
      const IndexRecord& last = impl.records.back();
      offset += last.record_length;
      if (offset > end_offset) {
        return make_error(StatusCode::StoreTruncated,
                          "segment " + std::to_string(expected_segment_id) +
                              " ends inside a record body");
      }
    }
    if (offset != end_offset) {
      return make_error(StatusCode::StoreTruncated,
                        "segment " + std::to_string(expected_segment_id) +
                            " committed length is not a whole number of records");
    }
    return offset;
  }
};

Expected<void> build_index(Ledger::Impl& impl, bool verify_records) {
  impl.records.clear();
  impl.event_index.clear();
  impl.interned.clear();
  impl.intern_map.clear();
  impl.records.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(impl.manifest.entry_count, 1000000ull)));

  ChainWalker walker{impl,
                     impl.manifest.retired_floor_sequence == 0 ? Digest256()
                                                               : impl.manifest.chain_at_floor,
                     impl.manifest.retired_floor_sequence + 1,
                     0,
                     0,
                     verify_records};

  for (const SegmentInfo& segment : impl.manifest.segments) {
    std::uint64_t prefix_end = segment.committed_bytes;
    // A sealed segment ends with a trailer that is not part of the digest.
    if (segment.committed_bytes >= kSegmentTrailerBytes) {
      auto file = open_file(impl.layout.segment_file(segment.id), OpenMode::ReadOnly);
      if (!file) {
        return file.error();
      }
      auto trailer = read_exact(file.value(), segment.committed_bytes - kSegmentTrailerBytes,
                                kSegmentTrailerBytes, kSegmentTrailerBytes);
      if (!trailer) {
        return trailer.error();
      }
      bool trailer_magic = true;
      for (std::size_t index = 0; index < 8; ++index) {
        if (trailer.value()[index] != kTrailerMagic[index]) {
          trailer_magic = false;
          break;
        }
      }
      if (!trailer_magic) {
        return make_error(StatusCode::StoreCorrupt,
                          "sealed segment " + std::to_string(segment.id) +
                              " does not end with a trailer");
      }
      std::string trailer_failure;
      auto decoded = decode_segment_trailer(trailer.value().data(), trailer.value().size(),
                                            &trailer_failure);
      if (!decoded) {
        return make_error(decoded.error().code,
                          "sealed segment " + std::to_string(segment.id) + ": " +
                              (trailer_failure.empty() ? decoded.error().detail : trailer_failure));
      }
      if (decoded.value().record_count != segment.entry_count ||
          decoded.value().last_sequence != segment.last_sequence() ||
          !(decoded.value().digest == segment.digest)) {
        return make_error(StatusCode::StoreCorrupt,
                          "sealed segment " + std::to_string(segment.id) +
                              " seal trailer disagrees with the manifest");
      }
      prefix_end = segment.committed_bytes - kSegmentTrailerBytes;
    }
    const std::size_t entries_before = impl.records.size();
    auto walked = walker.walk_segment(impl.layout.segment_file(segment.id), segment.id,
                                      segment.committed_bytes, prefix_end, &segment.digest, false);
    if (!walked) {
      return walked.error();
    }
    const std::size_t entries_added = impl.records.size() - entries_before;
    if (entries_added != segment.entry_count) {
      return make_error(StatusCode::StoreCorrupt,
                        "segment " + std::to_string(segment.id) + " holds " +
                            std::to_string(entries_added) +
                            " entries but the manifest declares " +
                            std::to_string(segment.entry_count));
    }
    if (entries_added > 0 && impl.records[entries_before].sequence != segment.first_sequence) {
      return make_error(StatusCode::SequenceGap,
                        "segment " + std::to_string(segment.id) +
                            " does not begin at its declared first sequence");
    }
  }

  if (impl.manifest.active_segment_entries > 0) {
    auto walked = walker.walk_segment(impl.layout.segment_file(impl.manifest.active_segment_id),
                                      impl.manifest.active_segment_id,
                                      impl.manifest.active_segment_bytes,
                                      impl.manifest.active_segment_bytes, nullptr, false);
    if (!walked) {
      return walked.error();
    }
  }

  if (impl.records.size() != impl.manifest.entry_count) {
    return make_error(StatusCode::StoreCorrupt,
                      "replay produced " + std::to_string(impl.records.size()) +
                          " entries but the manifest commits " +
                          std::to_string(impl.manifest.entry_count));
  }
  if (impl.manifest.entry_count > 0 && !(walker.previous_chain == impl.manifest.chain_head)) {
    return make_error(StatusCode::ChainBroken,
                      "replayed chain head does not match the committed manifest head");
  }
  return Expected<void>();
}

Expected<void> open_active_segment(Ledger::Impl& impl) {
  auto file = open_file(impl.layout.segment_file(impl.manifest.active_segment_id),
                        OpenMode::ReadWriteExisting);
  if (!file) {
    return file.error();
  }
  const std::uint64_t prefix_end = impl.manifest.active_segment_bytes;
  impl.segment_hasher = make_segment_hasher();
  auto hashed = hash_region(file.value(), 0, prefix_end, impl.segment_hasher, kMaxLedgerBytes);
  if (!hashed) {
    return hashed.error();
  }
  impl.active_file = std::move(file).value();
  auto seeked = seek_to_end(impl.active_file);
  if (!seeked) {
    return seeked.error();
  }
  return Expected<void>();
}

Expected<void> adopt_store(Ledger::Impl& impl, const OpenOptions& options) {
  auto descriptor = read_descriptor(impl.layout);
  if (!descriptor) {
    return descriptor.error();
  }
  impl.descriptor = descriptor.value();

  const bool read_write = options.mode == AccessMode::ReadWrite;
  auto lease_file = open_file(impl.layout.lease,
                              read_write ? OpenMode::ReadWriteCreate : OpenMode::ReadOnly);
  if (!lease_file) {
    return lease_file.error();
  }
  auto locked = try_lock(lease_file.value(), read_write ? LockMode::Exclusive : LockMode::Shared);
  if (!locked) {
    return locked.error();
  }
  if (!locked.value()) {
    return make_error(StatusCode::StoreBusy,
                      "another process holds writer authority for " + impl.path);
  }
  impl.lease_file = std::move(lease_file).value();
  impl.lease_held = true;

  ManifestSlot slot_a;
  ManifestSlot slot_b;
  auto manifest = load_best_manifest(impl.layout, impl.descriptor.store, &slot_a, &slot_b);
  if (!manifest) {
    return manifest.error();
  }
  impl.manifest = manifest.value();

  LeaseRecord lease;
  std::string lease_failure;
  auto lease_ok = read_lease_from(impl.lease_file, &lease, &lease_failure);
  if (!lease_ok) {
    return lease_ok.error();
  }
  std::uint64_t lease_epoch = 0;
  std::uint64_t lease_floor = 0;
  if (lease_ok.value()) {
    if (!(lease.store == impl.descriptor.store)) {
      return make_error(StatusCode::StoreSwapped, "writer lease belongs to a different store");
    }
    lease_epoch = lease.epoch;
    lease_floor = lease.floor_generation;
  } else if (options.recover_lease) {
    // Explicit administrative recovery. Authority is re-established from the
    // committed manifest: the epoch never decreases and the floor is raised to
    // the committed generation, so recovery cannot rewind the store.
    lease_epoch = impl.manifest.writer_epoch;
    lease_floor = std::max(lease_floor, impl.manifest.generation);
    impl.lease_recovered = true;
  }
  if (impl.manifest.generation < lease_floor) {
    return make_error(StatusCode::StoreRollbackDetected,
                      "committed generation " + std::to_string(impl.manifest.generation) +
                          " is older than the recorded floor " + std::to_string(lease_floor));
  }
  if (impl.manifest.writer_epoch > lease_epoch) {
    return make_error(StatusCode::StaleWriter,
                      "the committed manifest was published by writer epoch " +
                          std::to_string(impl.manifest.writer_epoch) +
                          " but the lease records only epoch " + std::to_string(lease_epoch) +
                          "; pass recover_lease to re-establish writer authority explicitly");
  }

  if (read_write) {
    if (lease_epoch == UINT64_MAX) {
      return make_error(StatusCode::ArithmeticOverflow, "writer epoch is exhausted");
    }
    const std::uint64_t next_epoch = lease_epoch + 1;
    const std::uint64_t floor = std::max(lease_floor, impl.manifest.generation);
    auto written = write_lease(impl, WriterEpoch(next_epoch), floor);
    if (!written) {
      return written.error();
    }
  } else {
    impl.writer_epoch = WriterEpoch(lease_epoch);
    impl.writer_id = derive_writer_id(impl.descriptor.store, WriterEpoch(lease_epoch));
    impl.floor_generation = lease_floor;
  }

  auto indexed = build_index(impl, options.verify_all_records);
  if (!indexed) {
    return indexed.error();
  }

  if (read_write) {
    auto orphans = discard_orphan_segments(impl, options.discard_staging_residue);
    if (!orphans) {
      return orphans.error();
    }
    impl.orphan_segments = orphans.value();
    auto active = open_active_segment(impl);
    if (!active) {
      return active.error();
    }
    auto size = file_size(impl.active_file);
    if (!size) {
      return size.error();
    }
    if (size.value() > impl.manifest.active_segment_bytes) {
      impl.residue_bytes = size.value() - impl.manifest.active_segment_bytes;
      if (options.discard_staging_residue) {
        auto truncated = truncate_file(impl.active_file, impl.manifest.active_segment_bytes);
        if (!truncated) {
          return truncated.error();
        }
        auto flushed = flush_durable(impl.active_file);
        if (!flushed) {
          return flushed.error();
        }
        auto seeked = seek_to_end(impl.active_file);
        if (!seeked) {
          return seeked.error();
        }
      }
    }
    auto staging_kind = inspect_path(impl.layout.manifest_staging);
    if (!staging_kind) {
      return staging_kind.error();
    }
    if (staging_kind.value() != PathKind::Missing && options.discard_staging_residue) {
      auto removed = delete_file(impl.layout.manifest_staging);
      if (!removed) {
        return removed.error();
      }
    }
  } else {
    // A read-only session reports residue but never modifies the store, so
    // recovered evidence cannot be silently rewritten by a reader.
    auto size = file_size_of_segment(impl.layout, impl.manifest.active_segment_id);
    if (!size) {
      return size.error();
    }
    if (size.value() > impl.manifest.active_segment_bytes) {
      impl.residue_bytes = size.value() - impl.manifest.active_segment_bytes;
    }
    auto orphans = discard_orphan_segments(impl, false);
    if (!orphans) {
      return orphans.error();
    }
    impl.orphan_segments = orphans.value();
    auto staging_kind = inspect_path(impl.layout.manifest_staging);
    if (!staging_kind) {
      return staging_kind.error();
    }
  }

  impl.open = true;
  return Expected<void>();
}

Expected<void> initialize_store(Ledger::Impl& impl, const CreateOptions& options) {
  auto kind = inspect_path(impl.layout.root);
  if (!kind) {
    return kind.error();
  }
  switch (kind.value()) {
    case PathKind::ReparsePoint:
      return make_error(StatusCode::StoreReparsePointRejected,
                        "store root is a reparse point: " + impl.layout.root);
    case PathKind::Directory:
      break;
    case PathKind::Missing: {
      auto created = ensure_directory(impl.layout.root);
      if (!created) {
        return created.error();
      }
      break;
    }
    default:
      return make_error(StatusCode::StorePathNotDirectory,
                        "store root exists and is not a directory: " + impl.layout.root);
  }

  auto descriptor_kind = inspect_path(impl.layout.descriptor);
  if (!descriptor_kind) {
    return descriptor_kind.error();
  }
  if (descriptor_kind.value() != PathKind::Missing) {
    if (options.fail_if_exists) {
      return make_error(StatusCode::StoreAlreadyExists,
                        "a store descriptor already exists at " + impl.layout.descriptor);
    }
    OpenOptions reopen;
    reopen.mode = AccessMode::ReadWrite;
    reopen.limits = options.limits;
    return adopt_store(impl, reopen);
  }

  auto segments_kind = inspect_path(impl.layout.segments_directory);
  if (!segments_kind) {
    return segments_kind.error();
  }
  if (segments_kind.value() == PathKind::Missing) {
    auto created = ensure_directory(impl.layout.segments_directory);
    if (!created) {
      return created.error();
    }
  } else if (segments_kind.value() != PathKind::Directory) {
    return make_error(StatusCode::StorePathNotDirectory, "SEGMENTS path is not a directory");
  }

  Descriptor descriptor;
  descriptor.store = generate_store_uuid();
  descriptor.canonical_unit = 1;
  auto encoded = encode_descriptor(descriptor);
  if (!encoded) {
    return encoded.error();
  }
  auto descriptor_file = open_file(impl.layout.descriptor, OpenMode::ReadWriteCreate);
  if (!descriptor_file) {
    return descriptor_file.error();
  }
  auto truncated = truncate_file(descriptor_file.value(), 0);
  if (!truncated) {
    return truncated.error();
  }
  auto written = write_all(descriptor_file.value(), encoded.value().data(), encoded.value().size());
  if (!written) {
    return written.error();
  }
  auto flushed = flush_durable(descriptor_file.value());
  if (!flushed) {
    return flushed.error();
  }
  auto verified = read_back_and_compare(descriptor_file.value(), encoded.value(), "descriptor");
  if (!verified) {
    return verified.error();
  }
  descriptor_file.value().close();
  impl.descriptor = descriptor;

  auto lease_file = open_file(impl.layout.lease, OpenMode::ReadWriteCreate);
  if (!lease_file) {
    return lease_file.error();
  }
  auto locked = try_lock(lease_file.value(), LockMode::Exclusive);
  if (!locked) {
    return locked.error();
  }
  if (!locked.value()) {
    return make_error(StatusCode::StoreBusy,
                      "another process holds writer authority for " + impl.path);
  }
  impl.lease_file = std::move(lease_file).value();
  impl.lease_held = true;
  auto lease_written = write_lease(impl, WriterEpoch(1), 0);
  if (!lease_written) {
    return lease_written.error();
  }

  const std::uint64_t segment_id = 1;
  auto segment_file = open_file(impl.layout.segment_file(segment_id), OpenMode::ReadWriteCreate);
  if (!segment_file) {
    return segment_file.error();
  }
  auto segment_truncated = truncate_file(segment_file.value(), 0);
  if (!segment_truncated) {
    return segment_truncated.error();
  }
  auto header = encode_segment_header(descriptor.store, segment_id, 1);
  auto header_written = write_all(segment_file.value(), header.data(), header.size());
  if (!header_written) {
    return header_written.error();
  }
  auto header_flushed = flush_durable(segment_file.value());
  if (!header_flushed) {
    return header_flushed.error();
  }
  impl.active_file = std::move(segment_file).value();
  impl.segment_hasher = make_segment_hasher();
  impl.segment_hasher.update(header.data(), header.size());

  Manifest manifest;
  manifest.generation = 1;
  manifest.incarnation = generate_ledger_incarnation();
  manifest.head_sequence = 0;
  manifest.entry_count = 0;
  manifest.retired_floor_sequence = 0;
  manifest.seq_at_floor = 0;
  manifest.writer_epoch = impl.writer_epoch.value();
  manifest.active_segment_id = segment_id;
  manifest.active_first_sequence = 1;
  manifest.active_segment_bytes = header.size();
  manifest.active_segment_entries = 0;
  impl.manifest = manifest;

  FaultInjection none;
  auto published = publish_manifest(impl, manifest, none);
  if (!published) {
    return published.error();
  }
  impl.open = true;
  return Expected<void>();
}

}  // namespace

Ledger::Ledger(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Ledger::Ledger(Ledger&& other) noexcept = default;
Ledger& Ledger::operator=(Ledger&& other) noexcept = default;
Ledger::~Ledger() = default;

Expected<Ledger> Ledger::create(const std::string& path, const CreateOptions& options) {
  auto limits = validate_limits(options.limits);
  if (!limits) {
    return limits.error();
  }
  auto impl = std::make_unique<Impl>();
  impl->limits = options.limits;
  impl->mode = AccessMode::ReadWrite;
  auto layout = layout_for(path, options.limits.max_path_bytes);
  if (!layout) {
    return layout.error();
  }
  impl->layout = layout.value();
  impl->path = impl->layout.root;
  auto initialized = initialize_store(*impl, options);
  if (!initialized) {
    return initialized.error();
  }
  return Ledger(std::move(impl));
}

Expected<Ledger> Ledger::open(const std::string& path, const OpenOptions& options) {
  auto limits = validate_limits(options.limits);
  if (!limits) {
    return limits.error();
  }
  auto impl = std::make_unique<Impl>();
  impl->limits = options.limits;
  impl->mode = options.mode;
  auto layout = layout_for(path, options.limits.max_path_bytes);
  if (!layout) {
    return layout.error();
  }
  impl->layout = layout.value();
  impl->path = impl->layout.root;

  auto kind = inspect_path(impl->layout.root);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == PathKind::ReparsePoint) {
    return make_error(StatusCode::StoreReparsePointRejected,
                      "store root is a reparse point: " + impl->layout.root);
  }
  if (kind.value() == PathKind::Missing) {
    return make_error(StatusCode::StoreNotFound, "store directory is missing: " + impl->path);
  }
  if (kind.value() != PathKind::Directory) {
    return make_error(StatusCode::StorePathNotDirectory,
                      "store root is not a directory: " + impl->path);
  }

  auto adopted = adopt_store(*impl, options);
  if (!adopted) {
    return adopted.error();
  }
  return Ledger(std::move(impl));
}

Expected<bool> Ledger::is_initialized(const std::string& path) {
  auto layout = layout_for(path, 512);
  if (!layout) {
    return layout.error();
  }
  auto kind = inspect_path(layout.value().descriptor);
  if (!kind) {
    return kind.error();
  }
  return kind.value() == PathKind::RegularFile;
}

Expected<AppendResult> Ledger::append(const AppendRequest& request) {
  Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  if (impl.mode != AccessMode::ReadWrite) {
    return make_error(StatusCode::StoreReadOnly, "ledger was opened read-only");
  }
  if (impl.poisoned) {
    return make_error(StatusCode::StoreCorrupt,
                      "ledger refused the append because a previous publication failed; reopen the "
                      "store");
  }

  auto valid = validate_content(request.content);
  if (!valid) {
    return valid.error();
  }
  if (request.request_id.size() > impl.limits.max_request_id_bytes) {
    return make_error(StatusCode::LimitExceeded, "request id exceeds the configured length limit");
  }
  for (char character : request.request_id) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu) {
      return make_error(StatusCode::InvalidArgument, "request id must be printable ASCII");
    }
  }

  auto identity = event_identity(request.content);
  if (!identity) {
    return identity.error();
  }
  auto digest = content_digest(request.content);
  if (!digest) {
    return digest.error();
  }

  // 1. Idempotent replay is resolved before any authority or generation check,
  //    so a retry of an accepted attempt can never be rejected as stale.
  if (!request.request_id.empty()) {
    auto found = impl.request_index.find(request.request_id);
    if (found != impl.request_index.end()) {
      if (!(found->second.result.content_digest == digest.value())) {
        return make_error(StatusCode::RequestIdConflict,
                          "request id '" + request.request_id +
                              "' was already used for different content");
      }
      AppendResult replay = found->second.result;
      replay.disposition = AppendDisposition::ReplayedByRequestId;
      replay.head = SequenceNumber(impl.manifest.head_sequence);
      return replay;
    }
  }
  {
    auto found = impl.event_index.find(identity.value());
    if (found != impl.event_index.end()) {
      const IndexRecord* record = impl.find_record(found->second);
      if (record == nullptr) {
        return make_error(StatusCode::EntryRetired,
                          "the entry with this event identity was retired by retention");
      }
      AppendResult replay;
      replay.disposition = AppendDisposition::ReplayedByEventId;
      replay.sequence = SequenceNumber(record->sequence);
      replay.event_id = record->event_id;
      replay.content_digest = record->content_digest;
      replay.chain_hash = record->chain_hash;
      replay.generation = ManifestGeneration(impl.manifest.generation);
      replay.head = SequenceNumber(impl.manifest.head_sequence);
      return replay;
    }
  }

  // 2. Authority the caller planned against.
  if (request.has_expected_incarnation &&
      !(request.expected_incarnation == impl.manifest.incarnation)) {
    return make_error(StatusCode::IncarnationMismatch,
                      "request was planned against a different ledger incarnation");
  }
  if (request.has_expected_head && !(request.expected_head == SequenceNumber(impl.manifest.head_sequence))) {
    return make_error(StatusCode::StaleAuthority,
                      "request was planned against head " +
                          std::to_string(request.expected_head.value()) + " but the committed head is " +
                          std::to_string(impl.manifest.head_sequence));
  }
  if (request.has_expected_generation &&
      request.expected_generation.value() != impl.manifest.generation) {
    return make_error(StatusCode::StaleAuthority,
                      "request was planned against generation " +
                          std::to_string(request.expected_generation.value()) +
                          " but the committed generation is " +
                          std::to_string(impl.manifest.generation));
  }

  // 3. Bounds.
  if (impl.manifest.entry_count >= impl.limits.max_entries) {
    return make_error(StatusCode::LimitExceeded,
                      "ledger holds the maximum number of entries (" +
                          std::to_string(impl.limits.max_entries) + ")");
  }

  // 4. Adjustment targets.
  if (is_adjustment(request.content.kind)) {
    const std::uint64_t target = request.content.target.value();
    if (target == 0 || target >= impl.next_sequence()) {
      if (target != 0 && target <= impl.manifest.retired_floor_sequence) {
        return make_error(StatusCode::EntryRetired,
                          "target sequence " + std::to_string(target) + " was retired");
      }
      return make_error(StatusCode::TargetNotInLedger,
                        "target sequence " + std::to_string(target) + " is not committed");
    }
    const IndexRecord* record = impl.find_record(target);
    if (record == nullptr) {
      if (target <= impl.manifest.retired_floor_sequence) {
        return make_error(StatusCode::EntryRetired,
                          "target sequence " + std::to_string(target) +
                              " was retired by retention");
      }
      return make_error(StatusCode::TargetNotInLedger,
                        "target sequence " + std::to_string(target) + " is not committed");
    }
    if (!(impl.text(record->object) == request.content.object.value()) ||
        !(impl.text(record->generation) == request.content.generation.value()) ||
        !(impl.text(record->interval) == request.content.interval.value()) ||
        !(impl.text(record->clock_domain) == request.content.clock_domain.value())) {
      return make_error(StatusCode::TargetKeyMismatch,
                        "adjustment key does not match the accounting key of the target entry");
    }
    if (request.content.time_basis != static_cast<TimeBasis>(record->time_basis)) {
      return make_error(StatusCode::TimeBasisMismatch,
                        "adjustment time basis does not match the target entry");
    }
    if (request.content.kind == EntryKind::Supersession &&
        request.content.authority_tier.value() < record->authority_tier) {
      return make_error(StatusCode::UnauthorizedSupersession,
                        "supersession authority tier is lower than the authority tier of the "
                        "entry it targets");
    }
  }

  // 5. Encode the record and prepare the staging snapshot.
  auto content_bytes = encode_content(request.content);
  if (!content_bytes) {
    return content_bytes.error();
  }
  const std::uint64_t sequence = impl.next_sequence();
  const Digest256 entry_hash = compute_entry_hash(sequence, content_bytes.value());
  const Digest256 previous_chain =
      impl.manifest.head_sequence == 0
          ? (impl.manifest.retired_floor_sequence == 0 ? Digest256() : impl.manifest.chain_at_floor)
          : impl.manifest.chain_head;
  const Digest256 chain_hash = compute_chain_hash(previous_chain, entry_hash);
  const std::vector<std::uint8_t> frame =
      encode_record(sequence, previous_chain, entry_hash, content_bytes.value());
  if (frame.size() > ModelLimits::kMaxRecordBytes) {
    return make_error(StatusCode::LimitExceeded, "encoded record exceeds the record size limit");
  }

  // 6. Fence: re-read the lease. A writer that lost its authority refuses to
  //    publish, even if the operating-system lock was bypassed.
  {
    LeaseRecord current;
    std::string failure;
    auto lease_ok = read_lease_from(impl.lease_file, &current, &failure);
    if (!lease_ok) {
      impl.poisoned = true;
      return lease_ok.error();
    }
    if (!lease_ok.value() || !lease_authorizes(current, impl.writer_id, impl.writer_epoch)) {
      return make_error(StatusCode::StaleWriter,
                        "writer lease is no longer held by this writer session");
    }
  }

  StagedMutation staged;
  staged.previous_manifest = impl.manifest;
  staged.previous_hasher = impl.segment_hasher;
  staged.previous_active_bytes = impl.manifest.active_segment_bytes;
  staged.previous_active_id = impl.manifest.active_segment_id;
  staged.previous_active_first_sequence = impl.manifest.active_first_sequence;
  staged.previous_active_entries = impl.manifest.active_segment_entries;
  staged.armed = true;

  const bool rotation_needed =
      impl.manifest.active_segment_entries > 0 &&
      impl.manifest.active_segment_bytes + frame.size() > impl.limits.max_segment_bytes;
  if (rotation_needed && impl.manifest.segments.size() + 1 >= kMaxSegmentsPerManifest) {
    return make_error(StatusCode::LimitExceeded,
                      "manifest segment table is full; compact the store before appending");
  }
  if (impl.committed_bytes_total() + frame.size() > impl.limits.max_ledger_bytes) {
    return make_error(StatusCode::LimitExceeded, "ledger has reached its committed byte ceiling");
  }

  if (rotation_needed) {
    auto rotated = rotate_segment(impl, staged);
    if (!rotated) {
      auto rolled = rollback_staged(impl, staged);
      if (!rolled) {
        return rolled.error();
      }
      return rotated.error();
    }
  }

  auto fail_before_commit = [&](const Error& error) -> Expected<AppendResult> {
    auto rolled = rollback_staged(impl, staged);
    if (!rolled) {
      return rolled.error();
    }
    return error;
  };

  const std::uint64_t record_offset = impl.manifest.active_segment_bytes;
  inject(request.fault, FaultInjection::Stage::BeforeRecordWrite);
  auto written = write_all(impl.active_file, frame.data(), frame.size());
  if (!written) {
    return fail_before_commit(written.error());
  }
  impl.segment_hasher.update(frame.data(), frame.size());
  impl.manifest.active_segment_bytes += frame.size();
  impl.manifest.active_segment_entries += 1;
  impl.manifest.entry_count += 1;
  impl.manifest.head_sequence = sequence;
  impl.manifest.chain_head = chain_hash;
  inject(request.fault, FaultInjection::Stage::AfterRecordWrite);

  auto flushed = flush_durable(impl.active_file);
  if (!flushed) {
    return fail_before_commit(flushed.error());
  }
  inject(request.fault, FaultInjection::Stage::AfterRecordFlush);

  {
    auto read_back = read_exact(impl.active_file, record_offset,
                                static_cast<std::uint64_t>(frame.size()), kMaxRecordBytes);
    if (!read_back) {
      return fail_before_commit(read_back.error());
    }
    if (read_back.value() != frame) {
      return fail_before_commit(make_error(StatusCode::DigestMismatch,
                                           "staged record read-back does not match the bytes written"));
    }
  }

  impl.manifest.generation += 1;
  impl.manifest.writer_epoch = impl.writer_epoch.value();
  auto published = publish_manifest(impl, impl.manifest, request.fault);
  if (!published) {
    return fail_before_commit(published.error());
  }
  // ---- commit point: the manifest slot is durable and re-verified ----
  staged.armed = false;
  inject(request.fault, FaultInjection::Stage::AfterHeadCommit);

  IndexRecord index;
  index.sequence = sequence;
  index.segment_id = impl.manifest.active_segment_id;
  index.record_offset = record_offset;
  index.record_length = static_cast<std::uint32_t>(frame.size());
  index.content_length = static_cast<std::uint32_t>(content_bytes.value().size());
  index.object = impl.intern(request.content.object.value());
  index.generation = impl.intern(request.content.generation.value());
  index.interval = impl.intern(request.content.interval.value());
  index.clock_domain = impl.intern(request.content.clock_domain.value());
  index.source_family = impl.intern(request.content.source_family.value());
  index.source_instance = impl.intern(request.content.source_instance.value());
  index.authority = impl.intern(request.content.authority.value());
  index.source_revision = request.content.source_revision.value();
  index.source_generation = request.content.source_generation.value();
  index.source_epoch = request.content.source_epoch.value();
  index.target = request.content.target.value();
  index.quantity = request.content.quantity.joules();
  index.declared_value = request.content.declared_value;
  index.interval_start = request.content.interval_start.value();
  index.interval_end = request.content.interval_end.value();
  index.observed_at = request.content.observed_at.value();
  index.kind = static_cast<std::uint8_t>(request.content.kind);
  index.quality = static_cast<std::uint8_t>(request.content.quality);
  index.time_basis = static_cast<std::uint8_t>(request.content.time_basis);
  index.authority_tier = request.content.authority_tier.value();
  index.declared_unit = static_cast<std::uint8_t>(request.content.declared_unit);
  index.content_digest = digest.value();
  index.entry_hash = entry_hash;
  index.prev_chain = previous_chain;
  index.chain_hash = chain_hash;
  index.event_id = identity.value();
  impl.records.push_back(index);
  impl.event_index.emplace(index.event_id, index.sequence);

  AppendResult result;
  result.disposition = AppendDisposition::Committed;
  result.sequence = SequenceNumber(sequence);
  result.event_id = identity.value();
  result.content_digest = digest.value();
  result.chain_hash = chain_hash;
  result.generation = ManifestGeneration(impl.manifest.generation);
  result.head = SequenceNumber(impl.manifest.head_sequence);

  if (!request.request_id.empty() && impl.limits.idempotency_window > 0) {
    IdempotencyRecord record;
    record.request_id = request.request_id;
    record.attempt = request.attempt.value();
    record.result = result;
    impl.request_index.emplace(record.request_id, record);
    impl.request_order.push_back(record.request_id);
    while (impl.request_order.size() > impl.limits.idempotency_window) {
      const std::string& oldest = impl.request_order.front();
      impl.request_index.erase(oldest);
      impl.request_order.pop_front();
    }
  }

  inject(request.fault, FaultInjection::Stage::BeforeResultReturn);
  return result;
}

Expected<EntryView> Ledger::inspect(SequenceNumber sequence) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  const IndexRecord* record = impl.find_record(sequence.value());
  if (record == nullptr) {
    if (sequence.value() != 0 && sequence.value() <= impl.manifest.retired_floor_sequence) {
      return make_error(StatusCode::EntryRetired,
                        "entry " + std::to_string(sequence.value()) + " was retired by retention");
    }
    return make_error(StatusCode::EntryNotFound,
                      "no committed entry with sequence " + std::to_string(sequence.value()));
  }
  return make_entry_view(impl, *record);
}

Expected<EntryView> Ledger::find_by_event_id(const EventId& id) const {
  const Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  auto found = impl.event_index.find(id);
  if (found == impl.event_index.end()) {
    return make_error(StatusCode::EntryNotFound, "no committed entry with that event identity");
  }
  const IndexRecord* record = impl.find_record(found->second);
  if (record == nullptr) {
    return make_error(StatusCode::EntryRetired, "the entry was retired by retention");
  }
  return make_entry_view(impl, *record);
}

SequenceNumber Ledger::head_sequence() const { return SequenceNumber(impl_->manifest.head_sequence); }
ManifestGeneration Ledger::generation() const {
  return ManifestGeneration(impl_->manifest.generation);
}
LedgerIncarnation Ledger::incarnation() const { return impl_->manifest.incarnation; }
StoreUuid Ledger::store_uuid() const { return impl_->descriptor.store; }
AccessMode Ledger::mode() const { return impl_->mode; }
WriterEpoch Ledger::writer_epoch() const { return impl_->writer_epoch; }
bool Ledger::is_open() const { return impl_->open; }
const StoreLimits& Ledger::limits() const { return impl_->limits; }
const std::string& Ledger::path() const { return impl_->path; }

Expected<CompactionResult> Ledger::compact(const CompactionOptions& options) {
  Impl& impl = *impl_;
  if (!impl.open) {
    return make_error(StatusCode::LedgerClosed, "ledger is closed");
  }
  if (impl.mode != AccessMode::ReadWrite) {
    return make_error(StatusCode::StoreReadOnly, "ledger was opened read-only");
  }
  const std::uint64_t keep = std::max<std::uint64_t>(options.keep_segments, 1);
  if (options.retain_from.value() <= impl.manifest.retired_floor_sequence) {
    return make_error(StatusCode::InvalidArgument,
                      "retain_from is not above the current retirement floor");
  }
  if (options.retain_from.value() > impl.manifest.head_sequence + 1) {
    return make_error(StatusCode::InvalidArgument, "retain_from is beyond the committed head");
  }
  if (impl.manifest.segments.size() <= keep) {
    CompactionResult result;
    result.generation = ManifestGeneration(impl.manifest.generation);
    result.segments_retired = 0;
    result.bytes_reclaimed = 0;
    result.floor_sequence = SequenceNumber(impl.manifest.retired_floor_sequence);
    result.chain_at_floor = impl.manifest.chain_at_floor;
    return result;
  }

  std::uint64_t retire_count = 0;
  std::uint64_t reclaimed = 0;
  std::uint64_t new_floor = impl.manifest.retired_floor_sequence;
  Digest256 new_anchor = impl.manifest.chain_at_floor;
  for (std::size_t index = 0; index + keep < impl.manifest.segments.size(); ++index) {
    const SegmentInfo& segment = impl.manifest.segments[index];
    if (segment.last_sequence() + 1 > options.retain_from.value()) {
      break;
    }
    const IndexRecord* last = impl.find_record(segment.last_sequence());
    if (last == nullptr) {
      return make_error(StatusCode::InternalError, "retirement anchor is missing from the index");
    }
    new_anchor = last->chain_hash;
    new_floor = segment.last_sequence();
    reclaimed += segment.committed_bytes;
    ++retire_count;
  }
  if (retire_count == 0) {
    CompactionResult result;
    result.generation = ManifestGeneration(impl.manifest.generation);
    result.segments_retired = 0;
    result.bytes_reclaimed = 0;
    result.floor_sequence = SequenceNumber(impl.manifest.retired_floor_sequence);
    result.chain_at_floor = impl.manifest.chain_at_floor;
    return result;
  }

  StagedMutation staged;
  staged.previous_manifest = impl.manifest;
  staged.armed = true;

  const std::uint64_t retired_entries =
      new_floor - staged.previous_manifest.retired_floor_sequence;
  if (retired_entries > impl.manifest.entry_count) {
    return make_error(StatusCode::InternalError,
                      "retention would retire more entries than the ledger retains");
  }
  impl.manifest.segments.erase(impl.manifest.segments.begin(),
                               impl.manifest.segments.begin() + static_cast<std::ptrdiff_t>(retire_count));
  // entry_count counts the entries the store still holds; head_sequence stays
  // the absolute, monotonic position of the last committed entry.
  impl.manifest.entry_count -= retired_entries;
  impl.manifest.retired_floor_sequence = new_floor;
  impl.manifest.seq_at_floor = new_floor;
  impl.manifest.chain_at_floor = new_anchor;
  impl.manifest.generation += 1;
  impl.manifest.writer_epoch = impl.writer_epoch.value();

  FaultInjection none;
  auto published = publish_manifest(impl, impl.manifest, none);
  if (!published) {
    impl.manifest = staged.previous_manifest;
    return published.error();
  }
  staged.armed = false;

  std::vector<std::uint64_t> retired_ids;
  retired_ids.reserve(retire_count);
  for (std::uint64_t index = 0; index < retire_count; ++index) {
    retired_ids.push_back(staged.previous_manifest.segments[static_cast<std::size_t>(index)].id);
  }
  for (std::uint64_t id : retired_ids) {
    auto removed = delete_file(impl.layout.segment_file(id));
    if (!removed) {
      impl.poisoned = true;
      return removed.error();
    }
  }

  impl.records.erase(impl.records.begin(),
                     impl.records.begin() + static_cast<std::ptrdiff_t>(new_floor -
                                                                       staged.previous_manifest
                                                                           .retired_floor_sequence));
  CompactionResult result;
  result.generation = ManifestGeneration(impl.manifest.generation);
  result.segments_retired = retire_count;
  result.bytes_reclaimed = reclaimed;
  result.floor_sequence = SequenceNumber(new_floor);
  result.chain_at_floor = new_anchor;
  return result;
}

Expected<void> Ledger::close() {
  Impl& impl = *impl_;
  if (!impl.open) {
    return Expected<void>();
  }
  if (impl.mode == AccessMode::ReadWrite && impl.lease_held && !impl.poisoned) {
    const std::uint64_t floor = std::max(impl.floor_generation, impl.manifest.generation);
    auto written = write_lease(impl, impl.writer_epoch, floor);
    if (!written) {
      impl.poisoned = true;
    }
  }
  impl.open = false;
  impl.active_file.close();
  if (impl.lease_held) {
    auto released = release_lock(impl.lease_file);
    (void)released;
    impl.lease_held = false;
  }
  impl.lease_file.close();
  return Expected<void>();
}

const char* to_string(FaultInjection::Stage stage) noexcept {
  switch (stage) {
    case FaultInjection::Stage::None: return "none";
    case FaultInjection::Stage::BeforeRecordWrite: return "before-record-write";
    case FaultInjection::Stage::AfterRecordWrite: return "after-record-write";
    case FaultInjection::Stage::AfterRecordFlush: return "after-record-flush";
    case FaultInjection::Stage::AfterStagingManifestWrite: return "after-staging-manifest-write";
    case FaultInjection::Stage::AfterStagingManifestFlush: return "after-staging-manifest-flush";
    case FaultInjection::Stage::AfterSlotWrite: return "after-slot-write";
    case FaultInjection::Stage::AfterSlotFlush: return "after-slot-flush";
    case FaultInjection::Stage::AfterHeadCommit: return "after-head-commit";
    case FaultInjection::Stage::BeforeResultReturn: return "before-result-return";
  }
  return "unknown";
}

Expected<FaultInjection::Stage> parse_fault_stage(std::string_view text) {
  using Stage = FaultInjection::Stage;
  const struct {
    const char* name;
    Stage stage;
  } kNames[] = {
      {"none", Stage::None},
      {"before-record-write", Stage::BeforeRecordWrite},
      {"after-record-write", Stage::AfterRecordWrite},
      {"after-record-flush", Stage::AfterRecordFlush},
      {"after-staging-manifest-write", Stage::AfterStagingManifestWrite},
      {"after-staging-manifest-flush", Stage::AfterStagingManifestFlush},
      {"after-slot-write", Stage::AfterSlotWrite},
      {"after-slot-flush", Stage::AfterSlotFlush},
      {"after-head-commit", Stage::AfterHeadCommit},
      {"before-result-return", Stage::BeforeResultReturn},
  };
  for (const auto& entry : kNames) {
    if (text == entry.name) {
      return entry.stage;
    }
  }
  std::uint32_t numeric = 0;
  bool digits = !text.empty();
  for (char character : text) {
    if (character < '0' || character > '9') {
      digits = false;
      break;
    }
    numeric = numeric * 10 + static_cast<std::uint32_t>(character - '0');
    if (numeric > 9) {
      digits = false;
      break;
    }
  }
  if (digits) {
    return static_cast<Stage>(numeric);
  }
  return make_error(StatusCode::InvalidArgument,
                    "unknown fault injection stage '" + std::string(text) + "'");
}

const char* to_string(AppendDisposition disposition) noexcept {
  switch (disposition) {
    case AppendDisposition::Committed: return "committed";
    case AppendDisposition::ReplayedByEventId: return "replayed-by-event-id";
    case AppendDisposition::ReplayedByRequestId: return "replayed-by-request-id";
  }
  return "unknown";
}

FormatInfo format_info() noexcept {
  FormatInfo info;
  info.descriptor_version = kStoreFormatVersion;
  info.manifest_version = kStoreFormatVersion;
  info.segment_version = kStoreFormatVersion;
  info.lease_version = kStoreFormatVersion;
  info.descriptor_bytes = static_cast<std::size_t>(kDescriptorBytes);
  info.manifest_header_bytes = static_cast<std::size_t>(kManifestHeaderBytes);
  info.segment_header_bytes = static_cast<std::size_t>(kSegmentHeaderBytes);
  info.lease_bytes = static_cast<std::size_t>(kLeaseBytes);
  info.little_endian_on_disk = true;
  return info;
}

}  // namespace energy_ledger
