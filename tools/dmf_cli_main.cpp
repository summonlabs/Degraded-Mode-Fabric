// Degraded Mode Fabric - operator command line client.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Live subcommands talk to a running coordinator over the framed protocol;
// "verify" opens the durable store offline and reports what actually survived.
// Nothing here decides policy: it installs definitions and reports answers.
#include <iostream>
#include <string>

#include "dmf/runtime.hpp"
#include "dmf/store.hpp"
#include "tool_support.hpp"

namespace {

using dmf::Status;

struct Connection {
  dmf::Client client{};
  bool ready = false;
};

bool connect(const dmf::tool::Args& args, Connection& connection) {
  dmf::ClientConfig config;
  config.host = args.get("host", "127.0.0.1");
  config.port = args.get_port("port", 0);
  config.token = args.get("token");
  config.role = args.get("role", "admin");
  config.process = dmf::ProcessId::from_value(args.get_u64("process", 3));
  config.boot = dmf::BootIncarnation::from_value(args.get_u64("boot", 1));
  config.principal = dmf::PrincipalId::from_value(args.get_u64("principal", 3));
  Status status = connection.client.connect(config);
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return false;
  }
  dmf::HelloResponse hello;
  status = connection.client.handshake(hello);
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return false;
  }
  connection.ready = true;
  std::cout << "HELLO session=" << dmf::to_hex(hello.session.value()) << std::endl;
  return true;
}

int report(const Status& status) {
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return 1;
  }
  std::cout << "OK" << std::endl;
  return 0;
}

int command_status(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  auto reply = connection.client.call(dmf::MessageType::QueryStatus, {});
  if (!reply.ok()) return report(reply.status());
  dmf::StatusBody body;
  const Status decoded = dmf::decode_object_body(reply.value(), body);
  if (!decoded.ok()) return report(decoded);
  std::cout << "boot=" << dmf::to_hex(body.boot.value()) << " term=" << dmf::to_hex(body.term.value())
            << " fabric=" << dmf::to_hex(body.fabric_generation.value())
            << " capacity=" << dmf::to_hex(body.capacity_generation.value())
            << " evidence=" << dmf::to_hex(body.evidence_generation.value()) << std::endl;
  std::cout << "policy_installed=" << (body.policy_installed ? 1 : 0)
            << " policy_digest=" << body.policy_digest << std::endl;
  std::cout << "contracts=" << body.retained_contracts << " live_grants=" << body.live_grants
            << " retained_grants=" << body.retained_grants << " fences=" << body.retained_fences
            << " decisions=" << body.retained_decisions << std::endl;
  std::cout << "journal_records=" << body.journal_records
            << " journal_bytes=" << body.journal_bytes_in_segment
            << " accounting_closed=" << (body.accounting_closed ? 1 : 0) << std::endl;
  std::cout << "recovery=" << body.recovery << " detail=" << body.recovery_detail << std::endl;
  return 0;
}

int command_install_policy(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  dmf::Policy policy = dmf::tool::standard_policy(
      dmf::PolicyId::from_value(args.get_u64("id", 1)),
      dmf::PolicyGeneration::from_value(args.get_u64("generation", 1)));
  policy.default_ttl_ticks = dmf::Tick{args.get_u64("ttl", 50)};
  policy.minimum_dwell_ticks = args.get_u32("dwell", 0);
  policy.minimum_protected_served = args.get_u32("min-protected", 0);
  policy.search_node_budget = args.get_u64("search-budget", 20000);
  policy.evidence_freshness_ticks = args.get_u64("freshness", 50);
  if (args.has("no-protect-first")) policy.protect_first = false;
  auto reply = connection.client.call(dmf::MessageType::InstallPolicy,
                                      dmf::tool::encode_object_body(policy));
  return report(reply.ok() ? Status{} : reply.status());
}

int command_register_contract(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  dmf::ServiceClass service_class = dmf::ServiceClass::Standard;
  if (!dmf::tool::parse_service_class(args.get("class", "standard"), service_class)) {
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=class" << std::endl;
    return 1;
  }
  const std::uint64_t bandwidth = args.get_u64("bandwidth", 10000);
  const dmf::ServiceContract contract = dmf::tool::make_contract(
      dmf::ContractId::from_value(args.get_u64("id", 1)),
      dmf::ContractGeneration::from_value(args.get_u64("generation", 1)),
      dmf::ScopeId::from_value(args.get_u64("scope", 1)),
      dmf::SubjectId::from_value(args.get_u64("subject", 1)),
      dmf::SubjectGeneration::from_value(args.get_u64("subject-generation", 1)), service_class,
      args.get_u32("priority", 10), bandwidth, args.get_u64("latency", 5000),
      args.get_u64("floor", bandwidth / 2), args.get_u32("max-concessions", 3));
  auto reply = connection.client.call(dmf::MessageType::RegisterContract,
                                      dmf::tool::encode_object_body(contract));
  return report(reply.ok() ? Status{} : reply.status());
}

int command_decide(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  auto reply = connection.client.call(
      dmf::MessageType::EvaluateContract,
      dmf::tool::encode_contract_request(args.get_u64("contract", 1)));
  if (!reply.ok()) return report(reply.status());
  dmf::DecisionBody body;
  const Status decoded = dmf::decode_object_body(reply.value(), body);
  if (!decoded.ok()) return report(decoded);
  std::cout << body.decision.explain() << std::endl;
  std::cout << "PLAN " << body.plan.render() << std::endl;
  return dmf::is_authorising(body.decision.outcome) ? 0 : 3;
}

int command_acquire(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  auto reply = connection.client.call(
      dmf::MessageType::AcquireAuthority,
      dmf::tool::encode_contract_request(args.get_u64("contract", 1)));
  if (!reply.ok()) return report(reply.status());
  dmf::GrantBody body;
  const Status decoded = dmf::decode_object_body(reply.value(), body);
  if (!decoded.ok()) return report(decoded);
  std::cout << body.grant.explain() << std::endl;
  std::cout << "ATTEMPT " << dmf::to_hex(body.grant.last_attempt.value()) << std::endl;
  return 0;
}

int command_restore(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  auto reply = connection.client.call(dmf::MessageType::RequestRestoration,
                                      dmf::tool::encode_id_body(args.get_u64("grant", 1)));
  if (!reply.ok()) return report(reply.status());
  dmf::RestorationBody body;
  const Status decoded = dmf::decode_object_body(reply.value(), body);
  if (!decoded.ok()) return report(decoded);
  std::cout << body.evaluation.explain() << std::endl;
  return body.evaluation.outcome == dmf::RestorationOutcome::Proven ? 0 : 3;
}

int command_fence(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  dmf::FenceReason reason = dmf::FenceReason::Manual;
  if (!dmf::tool::parse_fence_reason(args.get("reason", "manual"), reason)) {
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=reason" << std::endl;
    return 1;
  }
  auto reply = connection.client.call(
      dmf::MessageType::FenceGrant,
      dmf::tool::encode_fence_body(args.get_u64("grant", 1), reason));
  return report(reply.ok() ? Status{} : reply.status());
}

int command_revalidate(const dmf::tool::Args& args) {
  Connection connection;
  if (!connect(args, connection)) return 1;
  auto reply = connection.client.call(dmf::MessageType::Revalidate, {});
  if (!reply.ok()) return report(reply.status());
  dmf::RevalidateBody body;
  const Status decoded = dmf::decode_object_body(reply.value(), body);
  if (!decoded.ok()) return report(decoded);
  std::cout << "evaluated=" << body.evaluated << " fenced=" << body.fenced
            << " expired=" << body.expired << std::endl;
  return 0;
}

/// Offline inspection of a durable store. This is the only subcommand that
/// reads the on-disk state directly, and it never mutates it.
int command_verify(const dmf::tool::Args& args) {
  dmf::StoreConfig config;
  config.root = args.get("root", "dmf-store");
  // Verification never mutates: the store is opened read-only so it cannot
  // create, append, truncate or remove anything while it is being inspected.
  config.read_only = true;
  auto store = dmf::StateStore::open(config);
  if (!store.ok()) {
    dmf::tool::print_error(store.status());
    return 4;
  }
  const dmf::DurableState& state = store.value()->state();
  std::cout << "recovery=" << dmf::to_string(store.value()->recovery())
            << " detail=" << store.value()->recovery_detail() << std::endl;
  std::cout << "boot=" << dmf::to_hex(state.boot.value()) << " term=" << dmf::to_hex(state.term.value())
            << " journal_records=" << state.journal_records
            << " boot_count=" << state.boot_count << std::endl;
  std::uint64_t live = 0;
  std::uint64_t terminated = 0;
  for (const dmf::Grant& grant : state.grants) {
    if (grant.live()) {
      ++live;
    } else {
      ++terminated;
    }
  }
  std::cout << "contracts=" << state.contracts.size() << " grants=" << state.grants.size()
            << " live=" << live << " terminated=" << terminated
            << " fences=" << state.fences.size() << " decisions=" << state.decisions.size()
            << std::endl;
  std::cout << "policy_installed=" << (state.policy.validate().ok() ? 1 : 0) << std::endl;
  const dmf::Accounting accounting(state.counters);
  dmf::ClosureInputs inputs;
  inputs.live_grants = live;
  inputs.retained_terminated_grants = terminated;
  inputs.retained_fences = state.fences.size();
  inputs.retained_decisions = state.decisions.size();
  const dmf::ClosureReport closure = accounting.check_closure(inputs);
  std::cout << "accounting_closed=" << (closure.closed ? 1 : 0) << std::endl;
  if (!closure.closed) std::cout << closure.render() << std::endl;
  const Status status = store.value()->close();
  (void)status;
  return closure.closed ? 0 : 5;
}

}  // namespace

int main(int argc, char** argv) {
  const dmf::tool::Args args(argc, argv);
  const std::vector<std::string>& positional = args.positional();
  if (positional.empty() || args.has("help")) {
    std::cout << "usage: dmf-cli <command> [options]\n"
                 "commands: status, install-policy, register-contract, decide, acquire,\n"
                 "          restore, fence, revalidate, verify, version\n";
    return positional.empty() ? 1 : 0;
  }
  const std::string& command = positional.front();
  if (command == "version") {
    std::cout << "dmf " << dmf::kVersionString << std::endl;
    return 0;
  }
  if (command == "status") return command_status(args);
  if (command == "install-policy") return command_install_policy(args);
  if (command == "register-contract") return command_register_contract(args);
  if (command == "decide") return command_decide(args);
  if (command == "acquire") return command_acquire(args);
  if (command == "restore") return command_restore(args);
  if (command == "fence") return command_fence(args);
  if (command == "revalidate") return command_revalidate(args);
  if (command == "verify") return command_verify(args);
  std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=unknown-command" << std::endl;
  return 1;
}
