// Degraded Mode Fabric - real multiprocess suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real independent OS processes over real loopback sockets, hard-killed at
// meaningful durable boundaries. Threads are not a substitute for this.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/process.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

std::string read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string text;
  std::getline(stream, text);
  return text;
}

/// A coordinator running as a separate OS process.
class CoordinatorProcess {
 public:
  Status start(const std::string& root, std::vector<std::string> extra = {}) {
    ready_ = (std::filesystem::path(root).parent_path() / "ready.txt").string();
    std::error_code error;
    std::filesystem::remove(ready_, error);
    std::vector<std::string> arguments{"--root", root, "--allow-anonymous", "--port", "0",
                                       "--ready-file", ready_};
    for (std::string& argument : extra) arguments.push_back(std::move(argument));
    auto spawned = ChildProcess::spawn(tool_path("dmf_coordinator"), arguments);
    if (!spawned.ok()) return spawned.status();
    process_ = std::move(spawned.value());
    // Block until the child publishes its barrier. No timeout: a hang here is a
    // defect in the coordinator, not something to abandon.
    for (;;) {
      auto line = process_.read_line();
      if (!line.ok()) return line.status();
      if (line.value().rfind("DMF_READY", 0) == 0) {
        const std::size_t position = line.value().find("port=");
        if (position != std::string::npos) {
          port_ = static_cast<std::uint16_t>(std::stoi(line.value().substr(position + 5)));
        }
        return Status{};
      }
      if (line.value().rfind("DMF_ERROR", 0) == 0) {
        return Status(ErrorCode::IoError, line.value());
      }
    }
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] Result<std::uint32_t> hard_kill() { return process_.terminate(); }
  [[nodiscard]] Result<std::uint32_t> wait() { return process_.wait(); }
  [[nodiscard]] bool running() const noexcept { return process_.running(); }

 private:
  ChildProcess process_{};
  std::string ready_{};
  std::uint16_t port_ = 0;
};

/// Runs a tool to completion and returns its exit code and first output line.
struct ToolRun {
  std::uint32_t exit_code = 0;
  std::vector<std::string> lines{};

  [[nodiscard]] bool has(const std::string& needle) const {
    for (const std::string& line : lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

ToolRun run_tool(const std::string& tool, const std::vector<std::string>& arguments) {
  ToolRun run;
  auto spawned = ChildProcess::spawn(tool_path(tool), arguments);
  if (!spawned.ok()) {
    run.exit_code = 1;
    return run;
  }
  ChildProcess process = std::move(spawned.value());
  for (;;) {
    auto line = process.read_line();
    if (!line.ok()) break;
    run.lines.push_back(line.value());
  }
  auto code = process.wait();
  run.exit_code = code.ok() ? code.value() : 1;
  return run;
}

std::vector<std::string> coordinator_arguments(const std::string& root, std::uint16_t port) {
  return {"--root", root, "--port", std::to_string(port), "--allow-anonymous"};
}

}  // namespace

DMF_TEST(multiprocess, a_publisher_and_an_agent_drive_a_real_coordinator) {
  TempDir directory{"multiprocess"};
  const std::string root = directory.file("store");
  CoordinatorProcess coordinator;
  DMF_CHECK_OK(coordinator.start(root));
  DMF_CHECK(coordinator.port() != 0);

  const std::string port = std::to_string(coordinator.port());
  const ToolRun policy = run_tool("dmf_cli", {"install-policy", "--port", port, "--generation", "1"});
  DMF_CHECK_EQ(policy.exit_code, std::uint32_t{0});
  const ToolRun rigid =
      run_tool("dmf_cli", {"register-contract", "--port", port, "--id", "1", "--class", "protected",
                           "--bandwidth", "40000", "--latency", "2000", "--priority", "100"});
  DMF_CHECK_EQ(rigid.exit_code, std::uint32_t{0});
  const ToolRun flexible =
      run_tool("dmf_cli", {"register-contract", "--port", port, "--id", "2", "--class", "standard",
                           "--bandwidth", "4000", "--latency", "2000", "--floor", "1000",
                           "--priority", "10"});
  DMF_CHECK_EQ(flexible.exit_code, std::uint32_t{0});

  const ToolRun published =
      run_tool("dmf_publisher", {"--port", port, "--scope", "1", "--publisher", "1", "--bandwidth",
                                 "42000", "--rtt", "2000", "--paths", "4", "--reachability",
                                 "1000000", "--coverage", "1000000"});
  DMF_CHECK_EQ(published.exit_code, std::uint32_t{0});
  DMF_CHECK(published.has("DONE published=7"));

  const ToolRun protected_agent = run_tool("dmf_agent", {"--port", port, "--contract", "1"});
  DMF_CHECK_EQ(protected_agent.exit_code, std::uint32_t{0});
  DMF_CHECK(protected_agent.has("outcome=FULL"));

  const ToolRun standard_agent =
      run_tool("dmf_agent", {"--port", port, "--contract", "2", "--ack", "--apply", "--restore"});
  DMF_CHECK_EQ(standard_agent.exit_code, std::uint32_t{0});
  DMF_CHECK(standard_agent.has("outcome=DEGRADED"));
  DMF_CHECK(standard_agent.has("RESTORATION outcome=PROVEN"));

  const ToolRun status = run_tool("dmf_cli", {"status", "--port", port});
  DMF_CHECK_EQ(status.exit_code, std::uint32_t{0});
  DMF_CHECK(status.has("accounting_closed=1"));

  auto killed = coordinator.hard_kill();
  DMF_CHECK(killed.ok());

  // The store must be readable and closed after a hard kill, and a read-only
  // verification must not change it.
  const ToolRun verify = run_tool("dmf_cli", {"verify", "--root", root});
  DMF_CHECK_EQ(verify.exit_code, std::uint32_t{0});
  DMF_CHECK(verify.has("accounting_closed=1"));
  DMF_CHECK(verify.has("contracts=2"));
  const ToolRun verify_again = run_tool("dmf_cli", {"verify", "--root", root});
  DMF_CHECK_EQ(verify_again.exit_code, std::uint32_t{0});
}

DMF_TEST(multiprocess, a_crash_after_a_durable_commit_still_leaves_fenced_authority) {
  TempDir directory{"crashcommit"};
  const std::string root = directory.file("store");
  {
    CoordinatorProcess first;
    DMF_CHECK_OK(first.start(root));
    const std::string port = std::to_string(first.port());
    DMF_CHECK_EQ(run_tool("dmf_cli", {"install-policy", "--port", port, "--generation", "1"}).exit_code,
                 std::uint32_t{0});
    DMF_CHECK_EQ(run_tool("dmf_cli", {"register-contract", "--port", port, "--id", "1", "--class",
                                      "standard", "--bandwidth", "4000", "--latency", "2000",
                                      "--floor", "1000"}).exit_code,
                 std::uint32_t{0});
    DMF_CHECK_EQ(run_tool("dmf_publisher", {"--port", port, "--scope", "1", "--publisher", "1",
                                            "--bandwidth", "2000", "--rtt", "2000"})
                     .exit_code,
                 std::uint32_t{0});
    const ToolRun agent = run_tool("dmf_agent", {"--port", port, "--contract", "1", "--ack"});
    DMF_CHECK_EQ(agent.exit_code, std::uint32_t{0});
    DMF_CHECK(agent.has("ACK"));
    // Kill after the acknowledgement was durably committed.
    DMF_CHECK(first.hard_kill().ok());
  }
  {
    // Restart: the pre-restart grant must be fenced, never resumed.
    CoordinatorProcess second;
    DMF_CHECK_OK(second.start(root));
    const ToolRun status = run_tool("dmf_cli", {"status", "--port", std::to_string(second.port())});
    DMF_CHECK_EQ(status.exit_code, std::uint32_t{0});
    DMF_CHECK(status.has("live_grants=0"));
    DMF_CHECK(status.has("accounting_closed=1"));
    const ToolRun verify = run_tool("dmf_cli", {"verify", "--root", root});
    DMF_CHECK_EQ(verify.exit_code, std::uint32_t{0});
    DMF_CHECK(verify.has("live=0"));
    DMF_CHECK(!verify.has("fences=0"));
    DMF_CHECK(second.hard_kill().ok());
  }
}

DMF_TEST(multiprocess, crash_points_at_each_durable_boundary_are_conservative) {
  // The first four durable records are the boot advance and the three generation
  // bootstraps, all written before the coordinator becomes ready. Record five is
  // the policy installation triggered by the first client request, which is
  // therefore the boundary each crash point targets.
  struct Point {
    const char* flag;
    bool policy_committed;
  };
  const Point points[] = {
      {"--crash-before-record", false},
      {"--crash-after-write-record", true},
      {"--crash-after-record", true},
  };
  for (const Point& point : points) {
    TempDir directory{"crashpoint"};
    const std::string root = directory.file("store");
    CoordinatorProcess crashing;
    DMF_CHECK_OK(crashing.start(root, {point.flag, "5"}));

    // The request that would have become record five never gets an answer: the
    // process dies at the configured boundary while committing it.
    const ToolRun install =
        run_tool("dmf_cli", {"install-policy", "--port", std::to_string(crashing.port()),
                             "--generation", "1"});
    DMF_CHECK(install.exit_code != 0 || install.has("DMF_ERROR"));

    auto code = crashing.wait();
    DMF_CHECK(code.ok());
    // Either the injected boundary fired, or the process was hard-killed by the
    // harness; both are abrupt terminations with no clean shutdown.
    DMF_CHECK(code.value() == 70 || code.value() == 9 || code.value() == 1);

    // Whatever survived must open, must close, and must never contain a
    // half-written record. A process kill cannot lose a write that already
    // reached the operating system, so the write-then-flush ordering is what
    // makes this hold; only a machine-level failure could take the unflushed
    // tail, and that is exactly the torn tail the recovery path handles.
    const ToolRun verify = run_tool("dmf_cli", {"verify", "--root", root});
    DMF_CHECK_EQ(verify.exit_code, std::uint32_t{0});
    DMF_CHECK(verify.has("accounting_closed=1"));
    DMF_CHECK_EQ(verify.has("policy_installed=1"), point.policy_committed);

    // A restart over the crashed store must succeed, advance the incarnation and
    // still close its books.
    CoordinatorProcess restarted;
    DMF_CHECK_OK(restarted.start(root));
    const ToolRun status =
        run_tool("dmf_cli", {"status", "--port", std::to_string(restarted.port())});
    DMF_CHECK_EQ(status.exit_code, std::uint32_t{0});
    DMF_CHECK(status.has("accounting_closed=1"));
    DMF_CHECK(restarted.hard_kill().ok());
  }
}

DMF_TEST(multiprocess, a_second_coordinator_on_one_root_is_refused) {
  TempDir directory{"singlewriter"};
  const std::string root = directory.file("store");
  CoordinatorProcess first;
  DMF_CHECK_OK(first.start(root));
  auto second_spawned = ChildProcess::spawn(
      tool_path("dmf_coordinator"),
      {"--root", root, "--allow-anonymous", "--port", "0"});
  DMF_CHECK(second_spawned.ok());
  ChildProcess second = std::move(second_spawned.value());
  bool refused = false;
  for (;;) {
    auto line = second.read_line();
    if (!line.ok()) break;
    if (line.value().rfind("DMF_ERROR", 0) == 0 &&
        line.value().find("ALREADY_EXISTS") != std::string::npos) {
      refused = true;
    }
  }
  auto code = second.wait();
  DMF_CHECK(code.ok());
  DMF_CHECK(refused);
  DMF_CHECK_EQ(code.value(), std::uint32_t{1});
  DMF_CHECK(first.hard_kill().ok());
}

DMF_TEST(multiprocess, killing_a_publisher_leaves_the_coordinator_serving) {
  TempDir directory{"publisherdeath"};
  const std::string root = directory.file("store");
  CoordinatorProcess coordinator;
  DMF_CHECK_OK(coordinator.start(root));
  const std::string port = std::to_string(coordinator.port());
  DMF_CHECK_EQ(run_tool("dmf_cli", {"install-policy", "--port", port, "--generation", "1"}).exit_code,
               std::uint32_t{0});

  auto spawned = ChildProcess::spawn(tool_path("dmf_publisher"),
                                     {"--port", port, "--scope", "1", "--publisher", "1", "--hold"});
  DMF_CHECK(spawned.ok());
  ChildProcess publisher = std::move(spawned.value());
  bool published = false;
  for (;;) {
    auto line = publisher.read_line();
    if (!line.ok()) break;
    if (line.value().rfind("DONE published=7", 0) == 0) {
      published = true;
      break;
    }
  }
  DMF_CHECK(published);
  auto killed = publisher.terminate();
  DMF_CHECK(killed.ok());

  // The coordinator must still answer after a publisher dies mid-session.
  const ToolRun status = run_tool("dmf_cli", {"status", "--port", port});
  DMF_CHECK_EQ(status.exit_code, std::uint32_t{0});
  DMF_CHECK(status.has("accounting_closed=1"));
  DMF_CHECK(coordinator.hard_kill().ok());
}