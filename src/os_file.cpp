// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "os_file.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "energy_ledger/strong.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace energy_ledger {
namespace detail {
namespace {

constexpr std::uint64_t kMaxIoChunk = 4u * 1024u * 1024u;

#ifdef _WIN32

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), wide.data(), needed);
  if (written != needed) {
    return std::wstring();
  }
  return wide;
}

std::string describe_last_error(const char* operation) {
  const DWORD code = GetLastError();
  return std::string(operation) + " failed with Windows error " + std::to_string(code);
}

bool is_reserved_device_name(const std::string& component) {
  std::string stem = component;
  const std::size_t dot = stem.find('.');
  if (dot != std::string::npos) {
    stem = stem.substr(0, dot);
  }
  for (char& character : stem) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
    return true;
  }
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
    return stem[3] >= '1' && stem[3] <= '9';
  }
  return false;
}

#else

std::string describe_errno(const char* operation) {
  return std::string(operation) + " failed with errno " + std::to_string(errno);
}

#endif

}  // namespace

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

FileHandle::~FileHandle() { close(); }

void FileHandle::close() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
#endif
  handle_ = nullptr;
}

void FileHandle::adopt(void* handle) noexcept {
  close();
  handle_ = handle;
}

Expected<std::string> normalize_and_validate_path(const std::string& path, std::size_t max_bytes) {
  if (path.empty()) {
    return make_error(StatusCode::StorePathUnsafe, "store path must not be empty");
  }
  if (path.size() > max_bytes) {
    return make_error(StatusCode::StorePathTooLong,
                      "store path exceeds " + std::to_string(max_bytes) + " bytes");
  }
  if (!is_valid_utf8(path)) {
    return make_error(StatusCode::StorePathUnsafe, "store path is not well formed UTF-8");
  }
  for (char character : path) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte == 0x7Fu) {
      return make_error(StatusCode::StorePathUnsafe,
                        "store path contains a control character");
    }
  }

  std::string normalized;
  normalized.reserve(path.size());
  for (char character : path) {
    normalized.push_back(character == '/' ? static_cast<char>('\\') : character);
  }

#ifdef _WIN32
  if (normalized.size() >= 4 && normalized.compare(0, 4, "\\\\?\\") == 0) {
    return make_error(StatusCode::StorePathUnsafe, "device paths are not accepted");
  }
  if (normalized.size() >= 4 && normalized.compare(0, 4, "\\\\.\\") == 0) {
    return make_error(StatusCode::StorePathUnsafe, "device paths are not accepted");
  }
#endif

  // Split into components, validating each one.
  std::size_t index = 0;
  std::size_t drive_prefix = 0;
#ifdef _WIN32
  if (normalized.size() >= 2 && normalized[1] == ':' &&
      ((normalized[0] >= 'A' && normalized[0] <= 'Z') ||
       (normalized[0] >= 'a' && normalized[0] <= 'z'))) {
    drive_prefix = 2;
    index = 2;
  }
#endif

  std::vector<std::string> components;
  while (index <= normalized.size()) {
    std::size_t next = normalized.find('\\', index);
    if (next == std::string::npos) {
      next = normalized.size();
    }
    const std::string component = normalized.substr(index, next - index);
    if (!component.empty()) {
      if (component == ".") {
        return make_error(StatusCode::StorePathUnsafe, "'.' path components are not accepted");
      }
      if (component == "..") {
        return make_error(StatusCode::StorePathUnsafe, "'..' path components are not accepted");
      }
      for (char character : component) {
        if (character == ':' || character == '<' || character == '>' || character == '"' ||
            character == '|' || character == '?' || character == '*') {
          return make_error(StatusCode::StorePathUnsafe,
                            "store path component contains a reserved character");
        }
      }
#ifdef _WIN32
      if (component.back() == '.' || component.back() == ' ') {
        return make_error(StatusCode::StorePathUnsafe,
                          "store path component ends with a dot or a space");
      }
      if (is_reserved_device_name(component)) {
        return make_error(StatusCode::StorePathUnsafe,
                          "store path component is a reserved device name");
      }
#endif
      components.push_back(component);
    }
    if (next == normalized.size()) {
      break;
    }
    index = next + 1;
  }

  if (components.empty()) {
    return make_error(StatusCode::StorePathUnsafe, "store path has no usable component");
  }

  std::string result = normalized.substr(0, drive_prefix);
  for (std::size_t position = 0; position < components.size(); ++position) {
    if (position > 0 || (!result.empty() && result.back() != '\\')) {
      result.push_back('\\');
    }
    result += components[position];
  }
  return result;
}

std::string join_path(const std::string& directory, const std::string& child) {
  if (directory.empty()) {
    return child;
  }
  if (directory.back() == '\\' || directory.back() == '/') {
    return directory + child;
  }
  return directory + "\\" + child;
}

Expected<FileHandle> open_file(const std::string& path, OpenMode mode) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  if (wide.empty()) {
    return make_error(StatusCode::StorePathUnsafe, "store path could not be converted to UTF-16");
  }
  DWORD desired = 0;
  DWORD creation = OPEN_EXISTING;
  switch (mode) {
    case OpenMode::ReadOnly:
      desired = GENERIC_READ;
      break;
    case OpenMode::ReadWriteCreate:
      desired = GENERIC_READ | GENERIC_WRITE;
      creation = OPEN_ALWAYS;
      break;
    case OpenMode::ReadWriteExisting:
      desired = GENERIC_READ | GENERIC_WRITE;
      break;
  }
  const HANDLE handle =
      CreateFileW(wide.c_str(), desired, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return make_error(StatusCode::StoreNotFound, "file not found: " + path);
    }
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(StatusCode::StoreBusy, "file is held by another handle: " + path);
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("CreateFileW") + " for " + path);
  }
  FileHandle file;
  file.adopt(handle);
  return file;
#else
  int flags = 0;
  switch (mode) {
    case OpenMode::ReadOnly: flags = O_RDONLY; break;
    case OpenMode::ReadWriteCreate: flags = O_RDWR | O_CREAT; break;
    case OpenMode::ReadWriteExisting: flags = O_RDWR; break;
  }
  const int descriptor = ::open(path.c_str(), flags, 0644);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return make_error(StatusCode::StoreNotFound, "file not found: " + path);
    }
    return make_error(StatusCode::StoreIoError, describe_errno("open") + " for " + path);
  }
  FileHandle file;
  file.adopt(reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor)));
  return file;
#endif
}

Expected<std::size_t> read_at(FileHandle& file, std::uint64_t offset, void* buffer,
                              std::size_t length) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "read on an invalid file handle");
  }
  if (length == 0) {
    return std::size_t{0};
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
  DWORD received = 0;
  const DWORD request = static_cast<DWORD>(std::min<std::size_t>(length, kMaxIoChunk));
  if (ReadFile(static_cast<HANDLE>(file.native()), buffer, request, &received, &overlapped) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_HANDLE_EOF) {
      return std::size_t{0};
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("ReadFile"));
  }
  return static_cast<std::size_t>(received);
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  const ssize_t received = ::pread(descriptor, buffer, std::min<std::size_t>(length, kMaxIoChunk),
                                   static_cast<off_t>(offset));
  if (received < 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("pread"));
  }
  return static_cast<std::size_t>(received);
#endif
}

Expected<std::size_t> read_at_impl(FileHandle& file, std::uint64_t offset, void* buffer,
                                   std::size_t length) {
  std::size_t total = 0;
  auto* target = static_cast<std::uint8_t*>(buffer);
  while (total < length) {
    auto chunk = read_at(file, offset + total, target + total, length - total);
    if (!chunk) {
      return chunk.error();
    }
    if (chunk.value() == 0) {
      break;
    }
    total += chunk.value();
  }
  return total;
}

Expected<std::vector<std::uint8_t>> read_exact(FileHandle& file, std::uint64_t offset,
                                               std::uint64_t length, std::uint64_t max_bytes) {
  if (length > max_bytes) {
    return make_error(StatusCode::LimitExceeded,
                      "read of " + std::to_string(length) + " bytes exceeds the configured limit");
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length));
  if (length == 0) {
    return buffer;
  }
  auto read = read_at_impl(file, offset, buffer.data(), buffer.size());
  if (!read) {
    return read.error();
  }
  if (read.value() != buffer.size()) {
    return make_error(StatusCode::StoreTruncated,
                      "expected " + std::to_string(buffer.size()) + " bytes at offset " +
                          std::to_string(offset) + " but only " + std::to_string(read.value()) +
                          " were available");
  }
  return buffer;
}

Expected<void> write_all(FileHandle& file, const void* data, std::size_t length) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "write on an invalid file handle");
  }
  const auto* source = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < length) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(length - written, kMaxIoChunk));
#ifdef _WIN32
    DWORD accepted = 0;
    if (WriteFile(static_cast<HANDLE>(file.native()), source + written, request, &accepted,
                  nullptr) == 0) {
      return make_error(StatusCode::StoreIoError, describe_last_error("WriteFile"));
    }
    if (accepted == 0) {
      return make_error(StatusCode::StoreIoError, "WriteFile accepted zero bytes");
    }
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
    const ssize_t accepted = ::write(descriptor, source + written, request);
    if (accepted < 0) {
      return make_error(StatusCode::StoreIoError, describe_errno("write"));
    }
    if (accepted == 0) {
      return make_error(StatusCode::StoreIoError, "write accepted zero bytes");
    }
#endif
    written += static_cast<std::size_t>(accepted);
  }
  return Expected<void>();
}

Expected<void> write_at(FileHandle& file, std::uint64_t offset, const void* data,
                        std::size_t length) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "write on an invalid file handle");
  }
  if (length == 0) {
    return Expected<void>();
  }
  if (length > kMaxIoChunk) {
    return make_error(StatusCode::LimitExceeded, "single write exceeds the I/O chunk limit");
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
  DWORD accepted = 0;
  if (WriteFile(static_cast<HANDLE>(file.native()), data, static_cast<DWORD>(length), &accepted,
                &overlapped) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("WriteFile"));
  }
  if (accepted != length) {
    return make_error(StatusCode::StoreIoError, "WriteFile wrote a partial region");
  }
  return Expected<void>();
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  std::size_t written = 0;
  while (written < length) {
    const ssize_t accepted = ::pwrite(descriptor, static_cast<const std::uint8_t*>(data) + written,
                                      length - written, static_cast<off_t>(offset + written));
    if (accepted < 0) {
      return make_error(StatusCode::StoreIoError, describe_errno("pwrite"));
    }
    written += static_cast<std::size_t>(accepted);
  }
  return Expected<void>();
#endif
}

Expected<void> flush_durable(FileHandle& file) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "flush on an invalid file handle");
  }
#ifdef _WIN32
  if (FlushFileBuffers(static_cast<HANDLE>(file.native())) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("FlushFileBuffers"));
  }
  return Expected<void>();
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  if (::fsync(descriptor) != 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("fsync"));
  }
  return Expected<void>();
#endif
}

Expected<std::uint64_t> file_size(const FileHandle& file) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "size query on an invalid file handle");
  }
#ifdef _WIN32
  LARGE_INTEGER size{};
  if (GetFileSizeEx(static_cast<HANDLE>(file.native()), &size) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("GetFileSizeEx"));
  }
  return static_cast<std::uint64_t>(size.QuadPart);
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("fstat"));
  }
  return static_cast<std::uint64_t>(status.st_size);
#endif
}

Expected<void> truncate_file(FileHandle& file, std::uint64_t length) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "truncate on an invalid file handle");
  }
#ifdef _WIN32
  const HANDLE handle = static_cast<HANDLE>(file.native());
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("SetFilePointerEx"));
  }
  if (SetEndOfFile(handle) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("SetEndOfFile"));
  }
  return Expected<void>();
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  if (::ftruncate(descriptor, static_cast<off_t>(length)) != 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("ftruncate"));
  }
  return Expected<void>();
#endif
}

Expected<void> seek_to_end(FileHandle& file) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "seek on an invalid file handle");
  }
#ifdef _WIN32
  LARGE_INTEGER distance{};
  if (SetFilePointerEx(static_cast<HANDLE>(file.native()), distance, nullptr, FILE_END) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("SetFilePointerEx"));
  }
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  if (::lseek(descriptor, 0, SEEK_END) < 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("lseek"));
  }
#endif
  return Expected<void>();
}

Expected<bool> try_lock(FileHandle& file, LockMode mode) {
  if (!file.is_valid()) {
    return make_error(StatusCode::InternalError, "lock on an invalid file handle");
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (mode == LockMode::Exclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (LockFileEx(static_cast<HANDLE>(file.native()), flags, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
      return false;
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("LockFileEx"));
  }
  return true;
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  const int operation = (mode == LockMode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
  if (::flock(descriptor, operation) != 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return false;
    }
    return make_error(StatusCode::StoreIoError, describe_errno("flock"));
  }
  return true;
#endif
}

Expected<void> release_lock(FileHandle& file) {
  if (!file.is_valid()) {
    return Expected<void>();
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  if (UnlockFileEx(static_cast<HANDLE>(file.native()), 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_LOCKED) {
      return Expected<void>();
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("UnlockFileEx"));
  }
  return Expected<void>();
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(file.native()));
  if (::flock(descriptor, LOCK_UN) != 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("flock unlock"));
  }
  return Expected<void>();
#endif
}

Expected<PathKind> inspect_path(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  if (wide.empty()) {
    return make_error(StatusCode::StorePathUnsafe, "path could not be converted to UTF-16");
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return PathKind::Missing;
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("GetFileAttributesW"));
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return PathKind::ReparsePoint;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return PathKind::Directory;
  }
  return PathKind::RegularFile;
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT) {
      return PathKind::Missing;
    }
    return make_error(StatusCode::StoreIoError, describe_errno("lstat"));
  }
  if (S_ISLNK(status.st_mode)) {
    return PathKind::ReparsePoint;
  }
  if (S_ISDIR(status.st_mode)) {
    return PathKind::Directory;
  }
  if (S_ISREG(status.st_mode)) {
    return PathKind::RegularFile;
  }
  return PathKind::Other;
#endif
}

Expected<bool> path_exists(const std::string& path) {
  auto kind = inspect_path(path);
  if (!kind) {
    return kind.error();
  }
  return kind.value() != PathKind::Missing;
}

namespace {

/// Creates one directory level. The parent must already exist.
Expected<void> create_single_directory(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  if (wide.empty()) {
    return make_error(StatusCode::StorePathUnsafe, "path could not be converted to UTF-16");
  }
  if (CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD code = GetLastError();
    if (code != ERROR_ALREADY_EXISTS) {
      return make_error(StatusCode::StoreIoError, describe_last_error("CreateDirectoryW"));
    }
  }
  return Expected<void>();
#else
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return make_error(StatusCode::StoreIoError, describe_errno("mkdir"));
  }
  return Expected<void>();
#endif
}

}  // namespace

Expected<void> ensure_directory(const std::string& path) {
  auto kind = inspect_path(path);
  if (!kind) {
    return kind.error();
  }
  if (kind.value() == PathKind::Directory) {
    return Expected<void>();
  }
  if (kind.value() != PathKind::Missing) {
    return make_error(StatusCode::StorePathNotDirectory,
                      "path exists and is not a directory: " + path);
  }

  // Create every missing level in order. Each level is validated as it is
  // reached, so a file in the middle of the path is reported rather than
  // silently skipped.
  std::size_t index = 0;
  std::string prefix;
#ifdef _WIN32
  if (path.size() >= 2 && path[1] == ':') {
    prefix = path.substr(0, 2);
    index = 2;
    if (index < path.size() && (path[index] == '\\' || path[index] == '/')) {
      prefix.push_back('\\');
      ++index;
    }
  } else if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
    // UNC prefix: keep the server and share components together.
    std::size_t server_end = path.find('\\', 2);
    if (server_end == std::string::npos) {
      return make_error(StatusCode::StorePathUnsafe, "incomplete UNC path");
    }
    std::size_t share_end = path.find('\\', server_end + 1);
    if (share_end == std::string::npos) {
      share_end = path.size();
    }
    prefix = path.substr(0, share_end);
    index = share_end;
  }
#endif
  while (index < path.size()) {
    std::size_t next = path.find_first_of("\\/", index);
    if (next == std::string::npos) {
      next = path.size();
    }
    const std::string component = path.substr(index, next - index);
    if (!component.empty()) {
      if (!prefix.empty() && prefix.back() != '\\' && prefix.back() != '/') {
        prefix += "\\";
      }
      prefix += component;
      auto level = inspect_path(prefix);
      if (!level) {
        return level.error();
      }
      if (level.value() == PathKind::Missing) {
        auto created = create_single_directory(prefix);
        if (!created) {
          return created.error();
        }
      } else if (level.value() != PathKind::Directory) {
        return make_error(StatusCode::StorePathNotDirectory,
                          "path component exists and is not a directory: " + prefix);
      }
    }
    if (next == path.size()) {
      break;
    }
    index = next + 1;
  }
  return Expected<void>();
}

Expected<std::vector<std::string>> list_directory_entries(const std::string& path) {
  std::vector<std::string> entries;
#ifdef _WIN32
  WIN32_FIND_DATAW data{};
  const std::wstring pattern = widen(path) + L"\\*";
  HANDLE search = FindFirstFileW(pattern.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      return entries;
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("FindFirstFileW"));
  }
  do {
    const std::wstring name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
      continue;
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), utf8.data(),
                        needed, nullptr, nullptr);
    entries.push_back(utf8);
  } while (FindNextFileW(search, &data) != 0);
  FindClose(search);
#else
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return make_error(StatusCode::StoreIoError, describe_errno("opendir"));
  }
  while (struct dirent* entry = ::readdir(directory)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    entries.push_back(name);
  }
  ::closedir(directory);
#endif
  std::sort(entries.begin(), entries.end());
  return entries;
}

Expected<void> delete_file(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  if (DeleteFileW(wide.c_str()) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      return Expected<void>();
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("DeleteFileW"));
  }
  return Expected<void>();
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return make_error(StatusCode::StoreIoError, describe_errno("unlink"));
  }
  return Expected<void>();
#endif
}

Expected<void> remove_directory(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = widen(path);
  if (RemoveDirectoryW(wide.c_str()) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Expected<void>();
    }
    return make_error(StatusCode::StoreIoError, describe_last_error("RemoveDirectoryW"));
  }
  return Expected<void>();
#else
  if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
    return make_error(StatusCode::StoreIoError, describe_errno("rmdir"));
  }
  return Expected<void>();
#endif
}

Expected<void> move_file_replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
  const std::wstring wide_from = widen(from);
  const std::wstring wide_to = widen(to);
  if (MoveFileExW(wide_from.c_str(), wide_to.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return make_error(StatusCode::StoreIoError, describe_last_error("MoveFileExW"));
  }
  return Expected<void>();
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return make_error(StatusCode::StoreIoError, describe_errno("rename"));
  }
  return Expected<void>();
#endif
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

void terminate_process_now(int exit_code) noexcept {
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
  // TerminateProcess on the current process does not return.
  for (;;) {
    Sleep(1000);
  }
#else
  ::_exit(exit_code);
#endif
}

void os_sleep_milliseconds(std::uint64_t milliseconds) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::int64_t unix_time_milliseconds() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace detail
}  // namespace energy_ledger
