// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "energy_ledger/test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ledger_internal.hpp"

namespace energy_ledger {
namespace test_support {

namespace {

Expected<detail::StoreLayout> layout_of(const std::string& store_path) {
  return detail::layout_for(store_path, 512);
}

}  // namespace

Expected<std::vector<std::uint8_t>> read_file_bytes(const std::string& path) {
  auto file = detail::open_file(path, detail::OpenMode::ReadOnly);
  if (!file) {
    return file.error();
  }
  auto size = detail::file_size(file.value());
  if (!size) {
    return size.error();
  }
  return detail::read_exact(file.value(), 0, size.value(), 1024ull * 1024ull * 1024ull);
}

Expected<void> write_file_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  auto file = detail::open_file(path, detail::OpenMode::ReadWriteCreate);
  if (!file) {
    return file.error();
  }
  auto truncated = detail::truncate_file(file.value(), 0);
  if (!truncated) {
    return truncated.error();
  }
  auto written = detail::write_all(file.value(), bytes.data(), bytes.size());
  if (!written) {
    return written.error();
  }
  return detail::flush_durable(file.value());
}

Expected<void> append_file_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  auto file = detail::open_file(path, detail::OpenMode::ReadWriteCreate);
  if (!file) {
    return file.error();
  }
  auto seeked = detail::seek_to_end(file.value());
  if (!seeked) {
    return seeked.error();
  }
  auto written = detail::write_all(file.value(), bytes.data(), bytes.size());
  if (!written) {
    return written.error();
  }
  return detail::flush_durable(file.value());
}

Expected<void> truncate_file_to(const std::string& path, std::uint64_t length) {
  auto file = detail::open_file(path, detail::OpenMode::ReadWriteExisting);
  if (!file) {
    return file.error();
  }
  auto truncated = detail::truncate_file(file.value(), length);
  if (!truncated) {
    return truncated.error();
  }
  return detail::flush_durable(file.value());
}

Expected<void> flip_bit(const std::string& path, std::uint64_t byte_offset, std::uint8_t bit_index) {
  if (bit_index > 7) {
    return make_error(StatusCode::InvalidArgument, "bit index must be 0..7");
  }
  auto file = detail::open_file(path, detail::OpenMode::ReadWriteExisting);
  if (!file) {
    return file.error();
  }
  std::uint8_t byte = 0;
  auto read = detail::read_at(file.value(), byte_offset, &byte, 1);
  if (!read) {
    return read.error();
  }
  if (read.value() != 1) {
    return make_error(StatusCode::StoreTruncated, "bit flip target is past the end of the file");
  }
  byte = static_cast<std::uint8_t>(byte ^ (1u << bit_index));
  auto written = detail::write_at(file.value(), byte_offset, &byte, 1);
  if (!written) {
    return written.error();
  }
  return detail::flush_durable(file.value());
}

Expected<void> remove_file(const std::string& path) { return detail::delete_file(path); }

Expected<void> remove_directory_tree(const std::string& path) {
  auto kind = detail::inspect_path(path);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == detail::PathKind::Missing) {
    return Expected<void>();
  }
  if (kind.value() == detail::PathKind::ReparsePoint) {
    return make_error(StatusCode::StoreReparsePointRejected,
                      "refusing to recurse into a reparse point");
  }
  if (kind.value() != detail::PathKind::Directory) {
    return detail::delete_file(path);
  }
  auto entries = detail::list_directory_entries(path);
  if (!entries) {
    return entries.error();
  }
  for (const std::string& entry : entries.value()) {
    auto removed = remove_directory_tree(detail::join_path(path, entry));
    if (!removed) {
      return removed.error();
    }
  }
  return detail::remove_directory(path);
}

Expected<void> create_directory(const std::string& path) { return detail::ensure_directory(path); }

Expected<void> copy_file(const std::string& from, const std::string& to) {
  auto bytes = read_file_bytes(from);
  if (!bytes) {
    return bytes.error();
  }
  return write_file_bytes(to, bytes.value());
}

Expected<bool> file_exists(const std::string& path) {
  auto kind = detail::inspect_path(path);
  if (!kind) {
    return kind.error();
  }
  return kind.value() != detail::PathKind::Missing;
}

Expected<std::uint64_t> file_size(const std::string& path) {
  auto file = detail::open_file(path, detail::OpenMode::ReadOnly);
  if (!file) {
    return file.error();
  }
  return detail::file_size(file.value());
}

Expected<void> force_lease_epoch(const std::string& store_path, WriterEpoch epoch) {
  auto layout = layout_of(store_path);
  if (!layout) {
    return layout.error();
  }
  auto descriptor = detail::read_descriptor(layout.value());
  if (!descriptor) {
    return descriptor.error();
  }
  detail::LeaseRecord lease;
  lease.store = descriptor.value().store;
  lease.writer = derive_writer_id(descriptor.value().store, epoch);
  lease.epoch = epoch.value();
  lease.process_id = detail::current_process_id();
  lease.floor_generation = 0;
  lease.written_unix_ms = static_cast<std::uint64_t>(detail::unix_time_milliseconds());
  auto encoded = detail::encode_lease(lease);
  if (!encoded) {
    return encoded.error();
  }
  return write_file_bytes(layout.value().lease, encoded.value());
}

Expected<std::uint64_t> read_lease_epoch(const std::string& store_path) {
  auto layout = layout_of(store_path);
  if (!layout) {
    return layout.error();
  }
  detail::LeaseRecord lease;
  std::string failure;
  auto ok = detail::read_lease(layout.value(), &lease, &failure);
  if (!ok) {
    return ok.error();
  }
  if (!ok.value()) {
    return make_error(StatusCode::StoreNotFound, "lease is unavailable: " + failure);
  }
  return lease.epoch;
}

Expected<StoreUuid> read_store_uuid(const std::string& store_path) {
  auto layout = layout_of(store_path);
  if (!layout) {
    return layout.error();
  }
  auto descriptor = detail::read_descriptor(layout.value());
  if (!descriptor) {
    return descriptor.error();
  }
  return descriptor.value().store;
}

Expected<LedgerIncarnation> read_ledger_incarnation(const std::string& store_path) {
  auto layout = layout_of(store_path);
  if (!layout) {
    return layout.error();
  }
  auto descriptor = detail::read_descriptor(layout.value());
  if (!descriptor) {
    return descriptor.error();
  }
  auto manifest = detail::load_best_manifest(layout.value(), descriptor.value().store, nullptr, nullptr);
  if (!manifest) {
    return manifest.error();
  }
  return manifest.value().incarnation;
}

Expected<std::vector<std::string>> list_segment_files(const std::string& store_path) {
  auto layout = layout_of(store_path);
  if (!layout) {
    return layout.error();
  }
  auto entries = detail::list_directory_entries(layout.value().segments_directory);
  if (!entries) {
    return entries.error();
  }
  std::vector<std::string> segments;
  for (const std::string& entry : entries.value()) {
    if (entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".ELS") == 0) {
      segments.push_back(entry);
    }
  }
  std::sort(segments.begin(), segments.end());
  return segments;
}

void sleep_milliseconds(std::uint64_t milliseconds) {
  detail::os_sleep_milliseconds(milliseconds);
}

}  // namespace test_support
}  // namespace energy_ledger
