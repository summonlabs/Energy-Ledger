// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Narrow operating-system abstraction. This is the only place in the library
// that talks to the platform: files, durable flushes, exclusive/shared file
// locking, directories and process identity.

#ifndef ENERGY_LEDGER_SRC_OS_FILE_HPP
#define ENERGY_LEDGER_SRC_OS_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "energy_ledger/expected.hpp"

namespace energy_ledger {
namespace detail {

enum class OpenMode {
  ReadOnly,
  ReadWriteCreate,
  ReadWriteExisting,
};

enum class LockMode {
  Shared,
  Exclusive,
};

enum class PathKind {
  Missing,
  Directory,
  RegularFile,
  ReparsePoint,
  Other,
};

/// Owning native file handle. Movable, not copyable, closes on destruction.
class FileHandle {
 public:
  FileHandle() noexcept = default;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  ~FileHandle();

  bool is_valid() const noexcept { return handle_ != nullptr; }
  void close() noexcept;
  void* native() const noexcept { return handle_; }
  void adopt(void* handle) noexcept;

 private:
  void* handle_ = nullptr;
};

Expected<FileHandle> open_file(const std::string& path, OpenMode mode);
Expected<void> write_all(FileHandle& file, const void* data, std::size_t length);
Expected<void> write_at(FileHandle& file, std::uint64_t offset, const void* data, std::size_t length);
/// Reads up to length bytes at offset; a short result means end of file.
Expected<std::size_t> read_at(FileHandle& file, std::uint64_t offset, void* buffer,
                              std::size_t length);
/// Reads exactly length bytes, refusing when the file is shorter or the request
/// exceeds max_bytes.
Expected<std::vector<std::uint8_t>> read_exact(FileHandle& file, std::uint64_t offset,
                                               std::uint64_t length, std::uint64_t max_bytes);
Expected<void> flush_durable(FileHandle& file);
Expected<std::uint64_t> file_size(const FileHandle& file);
Expected<void> truncate_file(FileHandle& file, std::uint64_t length);
Expected<void> seek_to_end(FileHandle& file);

/// Non-blocking lock acquisition. Returns false when the region is held by
/// another handle (in this or another process).
Expected<bool> try_lock(FileHandle& file, LockMode mode);
Expected<void> release_lock(FileHandle& file);

/// Validates and normalizes an operator supplied path. Rejects traversal,
/// device names, alternate data streams, overlong input, malformed UTF-8 and
/// unsafe device paths before any file-system call is made.
Expected<std::string> normalize_and_validate_path(const std::string& path, std::size_t max_bytes);

Expected<PathKind> inspect_path(const std::string& path);
Expected<bool> path_exists(const std::string& path);
Expected<void> ensure_directory(const std::string& path);
Expected<std::vector<std::string>> list_directory_entries(const std::string& path);
Expected<void> delete_file(const std::string& path);
Expected<void> remove_directory(const std::string& path);
Expected<void> move_file_replace(const std::string& from, const std::string& to);

/// Joins a validated directory with a single, already safe child name.
std::string join_path(const std::string& directory, const std::string& child);

std::uint64_t current_process_id() noexcept;
/// Terminates the current process immediately without unwinding, atexit
/// handlers or interactive error reporting. Used only for durability testing.
void terminate_process_now(int exit_code) noexcept;
void os_sleep_milliseconds(std::uint64_t milliseconds) noexcept;
std::int64_t unix_time_milliseconds() noexcept;

}  // namespace detail
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_SRC_OS_FILE_HPP
