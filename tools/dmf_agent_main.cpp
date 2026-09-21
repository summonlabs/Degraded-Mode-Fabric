// Degraded Mode Fabric - service agent process.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Asks the coordinator what degraded service remains legally supportable for one
// contract, then walks the authority ladder: acquire, acknowledge, report the
// effect, and optionally request restoration. The agent never decides for
// itself; it only reports what the coordinator authorised.
//
// Output, one flushed line per record:
//   HELLO session=<hex> term=<hex> boot=<hex>
//   DECISION outcome=<OUTCOME> reason=<REASON> authority=<LEVEL> approved={...}
//            plan=<STATUS> escalation=<0|1>
//   GRANT id=<hex> attempt=<hex> sequence=<n> state=<STATE> degraded={...}
//   ACK attempt=<hex> sequence=<n> state=<STATE>
//   APPLY attempt=<hex> sequence=<n> state=<STATE>
//   RESTORATION outcome=<OUTCOME> reason=<REASON>
//   DONE
// Exit codes: 0 authorising, 3 refused/escalated, 1 transport or protocol error.
#include <string>

#include "tool_support.hpp"

namespace {

int run(const dmf::tool::Args& args) {
  const std::uint16_t port = args.get_port("port", 0);
  if (port == 0 || !args.has("contract")) {
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=port-and-contract-required" << std::endl;
    return 1;
  }
  dmf::ClientConfig config;
  config.host = args.get("host", "127.0.0.1");
  config.port = port;
  config.token = args.get("token");
  config.role = args.get("role", "operator");
  config.process = dmf::ProcessId::from_value(args.get_u64("process", 2));
  config.boot = dmf::BootIncarnation::from_value(args.get_u64("boot", 1));
  config.principal = dmf::PrincipalId::from_value(args.get_u64("principal", 2));

  dmf::Client client;
  dmf::Status status = client.connect(config);
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return 1;
  }
  dmf::HelloResponse hello;
  status = client.handshake(hello);
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return 1;
  }
  std::cout << "HELLO session=" << dmf::to_hex(hello.session.value())
            << " term=" << dmf::to_hex(hello.term.value())
            << " boot=" << dmf::to_hex(hello.boot.value()) << std::endl;

  const std::uint64_t contract = args.get_u64("contract", 0);

  // A standalone evaluation first: it shows the decision without committing
  // any authority.
  {
    auto reply = client.call(dmf::MessageType::EvaluateContract, dmf::tool::encode_contract_request(contract));
    if (!reply.ok()) {
      dmf::tool::print_error(reply.status());
      return 1;
    }
    dmf::DecisionBody body;
    status = dmf::decode_object_body(reply.value(), body);
    if (!status.ok()) {
      dmf::tool::print_error(status);
      return 1;
    }
    std::cout << "DECISION outcome=" << dmf::to_string(body.decision.outcome)
              << " reason=" << dmf::to_string(body.decision.primary_reason)
              << " authority=" << dmf::to_string(body.decision.max_authority)
              << " approved=" << body.decision.approved.render()
              << " plan=" << dmf::to_string(body.decision.plan_status)
              << " escalation=" << (body.decision.escalation_required ? 1 : 0) << std::endl;
    if (!dmf::is_authorising(body.decision.outcome)) {
      std::cout << "DONE" << std::endl;
      return 3;
    }
  }

  auto acquire = client.call(dmf::MessageType::AcquireAuthority, dmf::tool::encode_contract_request(contract));
  if (!acquire.ok()) {
    dmf::tool::print_error(acquire.status());
    return 3;
  }
  dmf::GrantBody grant_body;
  status = dmf::decode_object_body(acquire.value(), grant_body);
  if (!status.ok()) {
    dmf::tool::print_error(status);
    return 1;
  }
  dmf::Grant grant = grant_body.grant;
  std::cout << "GRANT id=" << dmf::to_hex(grant.id.value())
            << " attempt=" << dmf::to_hex(grant.last_attempt.value())
            << " sequence=" << grant.sequence.value() << " state=" << dmf::to_string(grant.state)
            << " degraded=" << grant.degraded.render() << std::endl;

  const auto step = [&](dmf::MessageType type, const char* label) -> int {
    auto reply = client.call(type, dmf::tool::encode_attempt_body(grant.id.value(), grant.last_attempt.value()));
    if (!reply.ok()) {
      dmf::tool::print_error(reply.status());
      return 1;
    }
    dmf::GrantBody body;
    const dmf::Status decoded = dmf::decode_object_body(reply.value(), body);
    if (!decoded.ok()) {
      dmf::tool::print_error(decoded);
      return 1;
    }
    grant = body.grant;
    std::cout << label << " attempt=" << dmf::to_hex(grant.last_attempt.value())
              << " sequence=" << grant.sequence.value()
              << " state=" << dmf::to_string(grant.state) << std::endl;
    return 0;
  };

  if (args.has("ack") || args.has("apply") || args.has("restore")) {
    const int code = step(dmf::MessageType::AcknowledgeGrant, "ACK");
    if (code != 0) return code;
  }
  if (args.has("apply") || args.has("restore")) {
    const int code = step(dmf::MessageType::ReportApplied, "APPLY");
    if (code != 0) return code;
  }
  if (args.has("restore")) {
    auto reply = client.call(dmf::MessageType::RequestRestoration, dmf::tool::encode_id_body(grant.id.value()));
    if (!reply.ok()) {
      dmf::tool::print_error(reply.status());
      return 1;
    }
    dmf::RestorationBody body;
    status = dmf::decode_object_body(reply.value(), body);
    if (!status.ok()) {
      dmf::tool::print_error(status);
      return 1;
    }
    std::cout << "RESTORATION outcome=" << dmf::to_string(body.evaluation.outcome)
              << " reason=" << dmf::to_string(body.evaluation.primary_reason)
              << " evidence=" << dmf::to_string(body.evaluation.evidence_state) << std::endl;
  }
  if (args.has("reapply-attempt")) {
    // Deliberately replay an attempt identity to prove the coordinator rejects
    // it instead of applying it twice.
    auto reply = client.call(dmf::MessageType::ReportApplied,
                             dmf::tool::encode_attempt_body(grant.id.value(), grant.last_attempt.value()));
    std::cout << "REPLAY accepted=" << (reply.ok() ? 1 : 0) << std::endl;
  }
  if (args.has("hold")) {
    std::string line;
    while (dmf::tool::read_stdin_line(line)) {
      if (line == "quit") break;
    }
  }
  std::cout << "DONE" << std::endl;
  (void)client.close();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const dmf::tool::Args args(argc, argv);
  return run(args);
}