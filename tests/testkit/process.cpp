// Degraded Mode Fabric - child process harness implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "testkit/process.hpp"

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dmf::test {

namespace {

std::string quote_argument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) {
    return argument;
  }
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (const char raw : argument) {
    if (raw == '\\') {
      ++backslashes;
      continue;
    }
    if (raw == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out.push_back(raw);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

}  // namespace

std::string tool_path(const std::string& name) {
  // Tools are built into the same directory as the test executable, so the
  // directory is derived from this process rather than baked in at compile
  // time: no absolute build path ever reaches the sources.
#ifdef _WIN32
  char buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return name + ".exe";
  const std::filesystem::path self{std::string(buffer, length)};
  const std::filesystem::path beside = self.parent_path() / (name + ".exe");
  std::error_code error;
  if (std::filesystem::exists(beside, error)) return beside.string();
  return (self.parent_path().parent_path() / (name + ".exe")).string();
#else
  std::error_code error;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) return name;
  return (self.parent_path() / name).string();
#endif
}

#ifdef _WIN32

ChildProcess::~ChildProcess() { close_handles(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
  *this = std::move(other);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close_handles();
    process_ = other.process_;
    stdout_read_ = other.stdout_read_;
    stdin_write_ = other.stdin_write_;
    stdout_write_ = other.stdout_write_;
    stdin_read_ = other.stdin_read_;
    pending_ = std::move(other.pending_);
    identifier_ = other.identifier_;
    running_ = other.running_;
    other.process_ = nullptr;
    other.stdout_read_ = nullptr;
    other.stdin_write_ = nullptr;
    other.stdout_write_ = nullptr;
    other.stdin_read_ = nullptr;
    other.identifier_ = 0;
    other.running_ = false;
  }
  return *this;
}

void ChildProcess::close_handles() {
  if (stdout_read_ != nullptr) CloseHandle(static_cast<HANDLE>(stdout_read_));
  if (stdin_write_ != nullptr) CloseHandle(static_cast<HANDLE>(stdin_write_));
  if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
  stdout_read_ = nullptr;
  stdin_write_ = nullptr;
  process_ = nullptr;
}

Result<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                         const std::vector<std::string>& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  // CreatePipe(&read, &write): the child inherits the write end of its stdout
  // and the read end of its stdin; the parent keeps the other two and marks them
  // non-inheritable.
  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  if (CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0) == 0) {
    return Status(ErrorCode::IoError, "cannot create the stdout pipe");
  }
  if (CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) == 0) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return Status(ErrorCode::IoError, "cannot create the stdin pipe");
  }
  SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line.append(quote_argument(argument));
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;
  startup.hStdInput = child_stdin_read;
  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  CloseHandle(child_stdout_write);
  CloseHandle(child_stdin_read);
  if (created == 0) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdin_write);
    return Status(ErrorCode::IoError, "cannot create the child process");
  }
  CloseHandle(information.hThread);
  ChildProcess child;
  child.process_ = information.hProcess;
  child.stdout_read_ = child_stdout_read;
  child.stdin_write_ = child_stdin_write;
  child.identifier_ = information.dwProcessId;
  child.running_ = true;
  return child;
}

Result<std::string> ChildProcess::read_line() {
  for (;;) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    if (stdout_read_ == nullptr) {
      return Status(ErrorCode::Shutdown, "child standard output is closed");
    }
    char buffer[4096];
    DWORD received = 0;
    const BOOL ok = ReadFile(static_cast<HANDLE>(stdout_read_), buffer, sizeof(buffer), &received,
                             nullptr);
    if (ok == 0 || received == 0) {
      const DWORD error = GetLastError();
      CloseHandle(static_cast<HANDLE>(stdout_read_));
      stdout_read_ = nullptr;
      if (error == ERROR_BROKEN_PIPE) {
        return Status(ErrorCode::Shutdown, "child closed its standard output");
      }
      return Status(ErrorCode::IoError, "cannot read from the child");
    }
    pending_.append(buffer, received);
  }
}

Status ChildProcess::write_line(const std::string& line) {
  if (stdin_write_ == nullptr) return Status(ErrorCode::Shutdown, "child standard input is closed");
  std::string text = line;
  text.push_back('\n');
  std::size_t written = 0;
  while (written < text.size()) {
    DWORD sent = 0;
    const DWORD chunk = static_cast<DWORD>(text.size() - written);
    if (WriteFile(static_cast<HANDLE>(stdin_write_), text.data() + written, chunk, &sent, nullptr) ==
        0) {
      return Status(ErrorCode::IoError, "cannot write to the child");
    }
    written += sent;
  }
  FlushFileBuffers(static_cast<HANDLE>(stdin_write_));
  return Status{};
}

Result<std::uint32_t> ChildProcess::wait() {
  if (process_ == nullptr) return Status(ErrorCode::InvalidState, "child is not running");
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
    return Status(ErrorCode::IoError, "cannot read the child exit code");
  }
  running_ = false;
  return static_cast<std::uint32_t>(code);
}

Result<std::uint32_t> ChildProcess::terminate() {
  if (process_ == nullptr) return Status(ErrorCode::InvalidState, "child is not running");
  if (running_) {
    if (TerminateProcess(static_cast<HANDLE>(process_), 9) == 0) {
      return Status(ErrorCode::IoError, "cannot terminate the child");
    }
  }
  return wait();
}

#else  // POSIX: implemented for portability, not exercised on the Windows host.

ChildProcess::~ChildProcess() { close_handles(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close_handles();
    process_ = other.process_;
    stdout_read_ = other.stdout_read_;
    stdin_write_ = other.stdin_write_;
    other.process_ = nullptr;
    other.stdout_read_ = nullptr;
    other.stdin_write_ = nullptr;
    identifier_ = other.identifier_;
    running_ = other.running_;
    other.identifier_ = 0;
    other.running_ = false;
  }
  return *this;
}

void ChildProcess::close_handles() {
  if (stdout_read_ != nullptr) ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
  if (stdin_write_ != nullptr) ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_)));
  stdout_read_ = nullptr;
  stdin_write_ = nullptr;
  process_ = nullptr;
}

Result<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                         const std::vector<std::string>& arguments) {
  int out_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) return Status(ErrorCode::IoError, "cannot create the stdout pipe");
  if (::pipe(in_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return Status(ErrorCode::IoError, "cannot create the stdin pipe");
  }
  const pid_t pid = ::fork();
  if (pid < 0) return Status(ErrorCode::IoError, "cannot fork");
  if (pid == 0) {
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(out_pipe[1], STDERR_FILENO);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    std::vector<char*> argv;
    std::string program = executable;
    argv.push_back(program.data());
    std::vector<std::string> storage = arguments;
    for (std::string& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ChildProcess child;
  child.process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  child.stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(out_pipe[0]));
  child.stdin_write_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(in_pipe[1]));
  child.identifier_ = static_cast<std::uint32_t>(pid);
  child.running_ = true;
  return child;
}

Result<std::string> ChildProcess::read_line() {
  for (;;) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    if (stdout_read_ == nullptr) return Status(ErrorCode::Shutdown, "child stdout closed");
    char buffer[4096];
    const ssize_t received =
        ::read(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)), buffer, sizeof(buffer));
    if (received <= 0) {
      ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
      stdout_read_ = nullptr;
      return Status(ErrorCode::Shutdown, "child closed its standard output");
    }
    pending_.append(buffer, static_cast<std::size_t>(received));
  }
}

Status ChildProcess::write_line(const std::string& line) {
  if (stdin_write_ == nullptr) return Status(ErrorCode::Shutdown, "child stdin closed");
  std::string text = line;
  text.push_back('\n');
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_));
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t sent = ::write(descriptor, text.data() + written, text.size() - written);
    if (sent <= 0) return Status(ErrorCode::IoError, "cannot write to the child");
    written += static_cast<std::size_t>(sent);
  }
  return Status{};
}

Result<std::uint32_t> ChildProcess::wait() {
  if (process_ == nullptr) return Status(ErrorCode::InvalidState, "child is not running");
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) return Status(ErrorCode::IoError, "waitpid failed");
  running_ = false;
  if (WIFEXITED(status)) return static_cast<std::uint32_t>(WEXITSTATUS(status));
  return static_cast<std::uint32_t>(128 + WTERMSIG(status));
}

Result<std::uint32_t> ChildProcess::terminate() {
  if (process_ == nullptr) return Status(ErrorCode::InvalidState, "child is not running");
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  if (running_) ::kill(pid, SIGKILL);
  return wait();
}

#endif

}  // namespace dmf::test