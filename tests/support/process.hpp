// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multi-process support for the authority and crash-semantics suites.
// Windows uses CreateProcessW with anonymous pipes; POSIX uses fork/exec with
// pipes. Reads are blocking, so the suites never need a deadline.

#ifndef ENERGY_LEDGER_TESTS_PROCESS_HPP
#define ENERGY_LEDGER_TESTS_PROCESS_HPP

#include <cstdint>
#include <string>

namespace eltest {

class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  /// Starts a child. Returns false when the process could not be created.
  bool spawn(const std::string& executable, const std::string& arguments);
  bool valid() const;

  /// Blocking read of one line from the child's standard output. Returns an
  /// empty string at end of file.
  std::string read_line();

  /// True once read_line has reached the end of the child's output stream.
  bool stdout_ended() const noexcept { return stdout_ended_; }

  /// Blocking write of one line to the child's standard input.
  void write_line(const std::string& text);

  /// Blocking wait for exit; returns the exit code.
  int wait();

  /// Forces the process to end without reporting it as a clean exit.
  void terminate();

  std::uint64_t process_id() const;

 private:
  void close_handles();

  void* process_ = nullptr;
  void* stdout_read_ = nullptr;
  void* stdin_write_ = nullptr;
  std::uint64_t process_id_ = 0;
  bool reaped_ = false;
  bool stdout_ended_ = false;
  int exit_code_ = 0;
};

}  // namespace eltest

#endif  // ENERGY_LEDGER_TESTS_PROCESS_HPP
