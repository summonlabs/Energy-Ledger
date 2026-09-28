// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/process.hpp"

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <vector>
extern char** environ;
#endif

namespace eltest {

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
  *this = std::move(other);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close_handles();
    process_ = other.process_;
    stdout_read_ = other.stdout_read_;
    stdin_write_ = other.stdin_write_;
    process_id_ = other.process_id_;
    reaped_ = other.reaped_;
    exit_code_ = other.exit_code_;
    other.process_ = nullptr;
    other.stdout_read_ = nullptr;
    other.stdin_write_ = nullptr;
    other.process_id_ = 0;
    other.reaped_ = true;
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (!reaped_ && valid()) {
    terminate();
    wait();
  }
  close_handles();
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

std::string narrow(const char* text, std::size_t length) { return std::string(text, length); }

}  // namespace

bool ChildProcess::spawn(const std::string& executable, const std::string& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  if (CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0) == 0) {
    return false;
  }
  if (CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) == 0) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return false;
  }
  SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = "\"" + executable + "\"";
  if (!arguments.empty()) {
    command_line += " ";
    command_line += arguments;
  }
  std::wstring wide_command = widen(command_line);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;
  startup.hStdInput = child_stdin_read;

  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &information);
  CloseHandle(child_stdout_write);
  CloseHandle(child_stdin_read);
  if (created == 0) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdin_write);
    return false;
  }
  CloseHandle(information.hThread);
  process_ = information.hProcess;
  process_id_ = information.dwProcessId;
  stdout_read_ = child_stdout_read;
  stdin_write_ = child_stdin_write;
  reaped_ = false;
  return true;
}

bool ChildProcess::valid() const { return process_ != nullptr; }

std::uint64_t ChildProcess::process_id() const { return process_id_; }

std::string ChildProcess::read_line() {
  std::string line;
  if (stdout_read_ == nullptr) {
    return line;
  }
  char character = 0;
  DWORD received = 0;
  for (;;) {
    const BOOL ok = ReadFile(static_cast<HANDLE>(stdout_read_), &character, 1, &received, nullptr);
    if (ok == 0 || received == 0) {
      stdout_ended_ = true;
      break;
    }
    if (character == '\n') {
      break;
    }
    if (character != '\r') {
      line.push_back(character);
    }
  }
  return line;
}

void ChildProcess::write_line(const std::string& text) {
  if (stdin_write_ == nullptr) {
    return;
  }
  const std::string payload = text + "\n";
  DWORD written = 0;
  WriteFile(static_cast<HANDLE>(stdin_write_), payload.data(),
            static_cast<DWORD>(payload.size()), &written, nullptr);
  FlushFileBuffers(static_cast<HANDLE>(stdin_write_));
}

int ChildProcess::wait() {
  if (process_ == nullptr) {
    return -1;
  }
  if (reaped_) {
    return exit_code_;
  }
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
  reaped_ = true;
  return exit_code_;
}

void ChildProcess::terminate() {
  if (process_ != nullptr && !reaped_) {
    TerminateProcess(static_cast<HANDLE>(process_), 0xDEADu);
  }
}

void ChildProcess::close_handles() {
  if (stdout_read_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (stdin_write_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(stdin_write_));
    stdin_write_ = nullptr;
  }
  if (process_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

#else

bool ChildProcess::spawn(const std::string& executable, const std::string& arguments) {
  int out_pipe[2];
  int in_pipe[2];
  if (pipe(out_pipe) != 0) {
    return false;
  }
  if (pipe(in_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, in_pipe[1]);

  std::vector<std::string> tokens;
  tokens.push_back(executable);
  std::string current;
  for (char character : arguments) {
    if (character == ' ') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  std::vector<char*> argv;
  for (std::string& token : tokens) {
    argv.push_back(token.data());
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int status =
      posix_spawn(&child, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out_pipe[1]);
  close(in_pipe[0]);
  if (status != 0) {
    close(out_pipe[0]);
    close(in_pipe[1]);
    return false;
  }
  process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
  process_id_ = static_cast<std::uint64_t>(child);
  stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(out_pipe[0]));
  stdin_write_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(in_pipe[1]));
  reaped_ = false;
  return true;
}

bool ChildProcess::valid() const { return process_ != nullptr; }

std::uint64_t ChildProcess::process_id() const { return process_id_; }

std::string ChildProcess::read_line() {
  std::string line;
  if (stdout_read_ == nullptr) {
    return line;
  }
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_));
  char character = 0;
  for (;;) {
    const ssize_t received = ::read(descriptor, &character, 1);
    if (received <= 0) {
      stdout_ended_ = true;
      break;
    }
    if (character == '\n') {
      break;
    }
    if (character != '\r') {
      line.push_back(character);
    }
  }
  return line;
}

void ChildProcess::write_line(const std::string& text) {
  if (stdin_write_ == nullptr) {
    return;
  }
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_));
  const std::string payload = text + "\n";
  ssize_t written = 0;
  while (written < static_cast<ssize_t>(payload.size())) {
    const ssize_t accepted = ::write(descriptor, payload.data() + written, payload.size() - written);
    if (accepted <= 0) {
      break;
    }
    written += accepted;
  }
}

int ChildProcess::wait() {
  if (process_ == nullptr) {
    return -1;
  }
  if (reaped_) {
    return exit_code_;
  }
  int status = 0;
  ::waitpid(static_cast<pid_t>(process_id_), &status, 0);
  reaped_ = true;
  if (WIFEXITED(status)) {
    exit_code_ = WEXITSTATUS(status);
  } else {
    exit_code_ = 128 + WTERMSIG(status);
  }
  return exit_code_;
}

void ChildProcess::terminate() {
  if (process_ != nullptr && !reaped_) {
    ::kill(static_cast<pid_t>(process_id_), SIGKILL);
  }
}

void ChildProcess::close_handles() {
  if (stdout_read_ != nullptr) {
    close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
    stdout_read_ = nullptr;
  }
  if (stdin_write_ != nullptr) {
    close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_)));
    stdin_write_ = nullptr;
  }
  process_ = nullptr;
}

#endif

}  // namespace eltest
