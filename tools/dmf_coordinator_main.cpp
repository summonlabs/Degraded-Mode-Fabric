// Degraded Mode Fabric - coordinator service process.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Protocol on standard output, one record per line, flushed immediately:
//   DMF_READY port=<n> boot=<hex> term=<hex> recovery=<OUTCOME>
//   DMF_ERROR code=<CODE> detail=<text>
//   OK tick=<n> records=<n>
//   STATUS boot=<hex> term=<hex> live=<n> retained=<n> fences=<n> closed=<0|1>
//          policy=<0|1> contracts=<n>
//   RECOVERY <OUTCOME> <detail>
//
// Commands on standard input: "advance <n>", "status", "recovery", "tick",
// "shutdown", "quit". EOF is equivalent to "quit".
#include <fstream>
#include <string>

#include "tool_support.hpp"

namespace {

int run(const dmf::tool::Args& args) {
  dmf::CoordinatorConfig config;
  config.store.root = args.get("root", "dmf-store");
  config.server.port = args.get_port("port", 0);
  config.server.max_sessions = static_cast<std::size_t>(args.get_u64("max-sessions", 64));
  config.server.max_frame_payload = static_cast<std::uint32_t>(args.get_u64("max-frame", dmf::kMaxFramePayload));
  const std::string admin_token = args.get("token");
  const std::string operator_token = args.get("operator-token");
  const std::string observer_token = args.get("observer-token");
  if (!admin_token.empty()) config.principals[admin_token] = dmf::PrincipalRole::Administrator;
  if (!operator_token.empty()) config.principals[operator_token] = dmf::PrincipalRole::Operator;
  if (!observer_token.empty()) config.principals[observer_token] = dmf::PrincipalRole::Observer;
  config.allow_anonymous_sessions = args.has("allow-anonymous");
  config.max_evidence_items = static_cast<std::size_t>(args.get_u64("max-evidence", 1U << 14));
  config.max_live_grants = static_cast<std::size_t>(args.get_u64("max-grants", 200000));
  config.max_contracts = static_cast<std::size_t>(args.get_u64("max-contracts", 200000));
  config.max_decisions = static_cast<std::size_t>(args.get_u64("max-decisions", 200000));
  config.snapshot_interval_records = args.get_u64("snapshot-interval", 8192);
  config.store.crash_before_record = args.get_u64("crash-before-record", 0);
  config.store.crash_after_write_record = args.get_u64("crash-after-write-record", 0);
  config.store.crash_after_record = args.get_u64("crash-after-record", 0);
  config.store.max_records_per_segment = args.get_u64("records-per-segment", 200000);
  config.store.max_journal_bytes = args.get_u64("journal-bytes", 8ULL << 20);
  const std::string origin = args.get("origin", "synthetic");
  if (origin == "real") {
    config.origin = dmf::OriginClass::Real;
  } else if (origin == "unsupported") {
    config.origin = dmf::OriginClass::Unsupported;
  } else {
    config.origin = dmf::OriginClass::Synthetic;
  }

  auto created = dmf::Coordinator::create(config);
  if (!created.ok()) {
    dmf::tool::print_error(created.status());
    return 1;
  }
  std::unique_ptr<dmf::Coordinator> coordinator = std::move(created.value());
  const dmf::Status started = coordinator->start();
  if (!started.ok()) {
    dmf::tool::print_error(started);
    return 1;
  }
  const dmf::AuthorityVector authority = coordinator->authority();
  std::cout << "DMF_READY port=" << coordinator->port() << " boot=" << dmf::to_hex(authority.boot.value())
            << " term=" << dmf::to_hex(authority.coordinator_term.value())
            << " recovery=" << dmf::to_string(coordinator->recovery()) << std::endl;
  const std::string ready_file = args.get("ready-file");
  if (!ready_file.empty()) {
    // Barriers are published as files so a supervising test can block on the
    // child without inventing a timeout.
    std::ofstream stream(ready_file, std::ios::binary | std::ios::trunc);
    stream << coordinator->port();
    stream.flush();
  }
  if (args.has("exit-after-ready")) {
    (void)coordinator->stop();
    return 0;
  }

  std::string line;
  while (dmf::tool::read_stdin_line(line)) {
    if (line == "quit" || line == "stop") break;
    if (line.rfind("advance ", 0) == 0) {
      const std::uint64_t delta = std::strtoull(line.c_str() + 8, nullptr, 10);
      const dmf::Tick tick = coordinator->advance(delta);
      std::cout << "OK tick=" << tick.value << " records=" << coordinator->state().journal_records
                << std::endl;
      continue;
    }
    if (line == "shutdown") {
      std::cout << "OK stopping" << std::endl;
      break;
    }
    if (line == "status") {
      const dmf::StatusBody body = coordinator->describe(coordinator->now());
      std::cout << "STATUS boot=" << dmf::to_hex(body.boot.value())
                << " term=" << dmf::to_hex(body.term.value())
                << " live=" << body.live_grants << " retained=" << body.retained_grants
                << " fences=" << body.retained_fences << " closed=" << (body.accounting_closed ? 1 : 0)
                << " policy=" << (body.policy_installed ? 1 : 0)
                << " contracts=" << body.retained_contracts << std::endl;
      continue;
    }
    if (line == "recovery") {
      std::cout << "RECOVERY " << dmf::to_string(coordinator->recovery()) << " "
                << coordinator->state().journal_records << std::endl;
      continue;
    }
    if (line == "tick") {
      std::cout << "OK tick=" << coordinator->now().value
                << " records=" << coordinator->state().journal_records << std::endl;
      continue;
    }
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=unknown-command" << std::endl;
  }
  const dmf::Status status = coordinator->stop();
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const dmf::tool::Args args(argc, argv);
  if (args.has("help")) {
    std::cout << "usage: dmf-coordinator --root DIR [--port N]\n"
                 "         [--token ADMIN] [--operator-token OP] [--observer-token OB]\n"
                 "         [--allow-anonymous] [--origin synthetic|real|unsupported]\n"
                 "         [--crash-before-record N] [--crash-after-write-record N]"
                 " [--crash-after-record N]\n";

    return 0;
  }
  return run(args);
}