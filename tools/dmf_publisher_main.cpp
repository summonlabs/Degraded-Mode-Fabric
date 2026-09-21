// Degraded Mode Fabric - fabric evidence publisher process.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Publishes a deterministic SYNTHETIC capability fixture for one scope. The
// runtime does not measure the fabric; this process stands in for an adjacent
// observer, exactly as the systems boundary requires. Physical fabric hardware
// is never claimed here.
//
// Output, one flushed line per record:
//   HELLO session=<hex>
//   PUBLISH <KIND> <STATE>
//   DONE published=<n>
//   DMF_ERROR code=<CODE> detail=<text>
#include <string>

#include "tool_support.hpp"

namespace {

int run(const dmf::tool::Args& args) {
  const std::uint16_t port = args.get_port("port", 0);
  if (port == 0) {
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=port-required" << std::endl;
    return 1;
  }
  dmf::ClientConfig config;
  config.host = args.get("host", "127.0.0.1");
  config.port = port;
  config.token = args.get("token");
  config.role = args.get("role", "operator");
  config.process = dmf::ProcessId::from_value(args.get_u64("process", 1));
  config.boot = dmf::BootIncarnation::from_value(args.get_u64("boot", 1));
  config.principal = dmf::PrincipalId::from_value(args.get_u64("principal", 1));

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
  std::cout << "HELLO session=" << dmf::to_hex(hello.session.value()) << std::endl;

  const dmf::ScopeId scope = dmf::ScopeId::from_value(args.get_u64("scope", 1));
  const dmf::PublisherId publisher = dmf::PublisherId::from_value(args.get_u64("publisher", 1));
  const dmf::EvidenceGeneration generation =
      dmf::EvidenceGeneration::from_value(args.get_u64("generation", 1));
  dmf::FabricTopologyClass topology = dmf::FabricTopologyClass::FullMesh;
  if (!dmf::tool::parse_topology(args.get("topology", "full"), topology)) {
    std::cout << "DMF_ERROR code=INVALID_ARGUMENT detail=topology" << std::endl;
    return 1;
  }
  struct KindPlan {
    dmf::EvidenceKind kind;
    std::uint64_t value;
  };
  const std::uint64_t durability = args.get_u64("durability", 1);
  const KindPlan plan[] = {
      {dmf::EvidenceKind::FabricTopology, static_cast<std::uint64_t>(topology)},
      {dmf::EvidenceKind::AggregateBandwidth, args.get_u64("bandwidth", 100000)},
      {dmf::EvidenceKind::RoundTripLatency, args.get_u64("rtt", 1000)},
      {dmf::EvidenceKind::PathDiversity, args.get_u64("paths", 4)},
      {dmf::EvidenceKind::Reachability, args.get_u64("reachability", 1000000)},
      {dmf::EvidenceKind::NodeCoverage, args.get_u64("coverage", 1000000)},
      {dmf::EvidenceKind::SynchronousDurability, durability > 1 ? 1 : durability},
  };
  std::uint64_t published = 0;
  for (const KindPlan& entry : plan) {
    dmf::EvidenceItem item;
    item.publisher = publisher;
    item.publisher_boot = config.boot;
    item.generation = generation;
    item.kind = entry.kind;
    item.scope = scope;
    item.state = dmf::EvidenceState::Known;
    item.value = entry.value;
    item.observed_tick = dmf::Tick{args.get_u64("observed-tick", 0)};
    item.origin = dmf::OriginClass::Synthetic;
    const std::vector<std::uint8_t> body = dmf::encode_object_body(item);
    auto reply = client.call(dmf::MessageType::PublishEvidence, body);
    if (!reply.ok()) {
      dmf::tool::print_error(reply.status());
      return 1;
    }
    ++published;
    std::cout << "PUBLISH " << dmf::to_string(entry.kind) << " KNOWN" << std::endl;
  }
  std::cout << "DONE published=" << published << std::endl;

  if (args.has("hold")) {
    std::string line;
    while (dmf::tool::read_stdin_line(line)) {
      if (line == "quit") break;
    }
  }
  (void)client.close();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const dmf::tool::Args args(argc, argv);
  return run(args);
}
