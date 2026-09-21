// Degraded Mode Fabric - child process harness for multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real independent OS processes, real loopback sockets, real hard kills. The
// harness blocks indefinitely on every wait: a hang is a defect, not something
// a watchdog should paper over.
#ifndef DMF_TESTKIT_PROCESS_HPP
#define DMF_TESTKIT_PROCESS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/core.hpp"

namespace dmf::test {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Spawns \p executable with \p arguments, capturing its standard output and
  /// feeding its standard input.
  static Result<ChildProcess> spawn(const std::string& executable,
                                    const std::vector<std::string>& arguments);

  /// Reads one line (without the terminator) from the child's standard output.
  /// Blocks until a line is available or the child closes the stream.
  [[nodiscard]] Result<std::string> read_line();
  /// Writes one line to the child's standard input and flushes.
  [[nodiscard]] Status write_line(const std::string& line);
  /// Waits for the child to exit and returns its exit code.
  [[nodiscard]] Result<std::uint32_t> wait();
  /// Terminates the child abruptly, then waits for it.
  [[nodiscard]] Result<std::uint32_t> terminate();
  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] std::uint32_t identifier() const noexcept { return identifier_; }

 private:
  void close_handles();

  void* process_ = nullptr;   // HANDLE on Windows
  void* stdout_read_ = nullptr;
  void* stdin_write_ = nullptr;
  void* stdout_write_ = nullptr;
  void* stdin_read_ = nullptr;
  std::string pending_{};
  std::uint32_t identifier_ = 0;
  bool running_ = false;
};

/// Absolute path of a tool built next to this test executable.
std::string tool_path(const std::string& name);

}  // namespace dmf::test

#endif  // DMF_TESTKIT_PROCESS_HPP
