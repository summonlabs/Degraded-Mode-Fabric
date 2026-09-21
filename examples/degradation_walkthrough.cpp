// Degraded Mode Fabric - degradation walkthrough.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A single translation unit that shows the product-defining proposition end to
// end with no store and no sockets: given reduced capability, what remains
// supportable, what is withdrawn, and what must be refused.
//
// Every fixture in this program is SYNTHETIC. No physical fabric hardware is
// exercised.
#include <iostream>
#include <string>
#include <vector>

#include "dmf/engine.hpp"

namespace {

dmf::CapabilitySnapshot capability(std::uint64_t bandwidth_kbps, std::uint64_t rtt_us,
                                   dmf::FabricTopologyClass topology) {
  dmf::CapabilitySnapshot snapshot;
  snapshot.scope = dmf::ScopeId::from_value(1);
  snapshot.generation = dmf::CapacityGeneration::from_value(1);
  snapshot.state = dmf::EvidenceState::Known;
  snapshot.topology = topology;
  snapshot.bandwidth_kbps = bandwidth_kbps;
  snapshot.rtt_p99_us = rtt_us;
  snapshot.path_diversity = 4;
  snapshot.reachable_ppm = 1000000;
  snapshot.node_coverage_ppm = 1000000;
  snapshot.synchronous_durability = true;
  snapshot.observed_tick = dmf::Tick{10};
  snapshot.origin = dmf::OriginClass::Synthetic;
  return snapshot;
}

dmf::ServiceContract contract(dmf::ContractId id, dmf::ServiceClass service_class,
                              std::uint32_t priority, std::uint64_t bandwidth,
                              std::uint64_t floor) {
  dmf::ServiceContract value;
  value.id = id;
  value.generation = dmf::ContractGeneration::from_value(1);
  value.scope = dmf::ScopeId::from_value(1);
  value.subject = dmf::SubjectId::from_value(id.value());
  value.subject_generation = dmf::SubjectGeneration::from_value(1);
  value.service_class = service_class;
  value.non_degradable = service_class == dmf::ServiceClass::Protected;
  value.priority = priority;
  (void)value.original.insert(dmf::GuaranteeKind::Bandwidth, bandwidth);
  (void)value.original.insert(dmf::GuaranteeKind::LatencyP99, 2000);
  (void)value.original.insert(dmf::GuaranteeKind::Reachability, 999000);
  if (!value.protected_obligation()) {
    (void)value.envelope.insert(dmf::ConcessionBound{dmf::GuaranteeKind::Bandwidth, floor});
    (void)value.envelope.insert(
        dmf::ConcessionBound{dmf::GuaranteeKind::LatencyP99, 50000});
    value.envelope.set_max_concessions(3);
  }
  return value;
}

dmf::Policy policy() {
  dmf::Policy value;
  value.id = dmf::PolicyId::from_value(1);
  value.generation = dmf::PolicyGeneration::from_value(1);
  value.protect_first = true;
  value.default_ttl_ticks = dmf::Tick{100};
  value.max_concessions_per_contract = 3;
  value.search_node_budget = 20000;
  value.evidence_freshness_ticks = 1000;

  dmf::PolicyRule degrade;
  degrade.id = dmf::RuleId::from_value(1);
  degrade.precedence = 10;
  degrade.action = dmf::DegradeAction::Degrade;
  value.rules.push_back(degrade);

  dmf::PolicyRule protect;
  protect.id = dmf::RuleId::from_value(2);
  protect.precedence = 1;
  protect.service_class = dmf::ServiceClass::Protected;
  protect.action = dmf::DegradeAction::Protect;
  value.rules.push_back(protect);

  dmf::ClassProfile protected_profile;
  protected_profile.service_class = dmf::ServiceClass::Protected;
  protected_profile.may_be_degraded = false;
  protected_profile.ceiling.set_max_concessions(0);
  value.class_profiles.push_back(protected_profile);
  for (const dmf::ServiceClass service_class :
       {dmf::ServiceClass::Standard, dmf::ServiceClass::BestEffort}) {
    dmf::ClassProfile profile;
    profile.service_class = service_class;
    profile.may_be_degraded = true;
    profile.ceiling.set_max_concessions(3);
    value.class_profiles.push_back(profile);
  }
  return value;
}

void show(const char* label, const dmf::Decision& decision) {
  std::cout << "--- " << label << " ---\n";
  std::cout << decision.explain() << '\n';
  std::cout << "authorising=" << (dmf::is_authorising(decision.outcome) ? "yes" : "no")
            << " escalation=" << (decision.escalation_required ? "yes" : "no") << "\n\n";
}

}  // namespace

int main() {
  const dmf::Policy active_policy = policy();
  const dmf::EvidenceVector evidence;
  const dmf::ServiceContract protected_contract =
      contract(dmf::ContractId::from_value(1), dmf::ServiceClass::Protected, 100, 40000, 0);
  const dmf::ServiceContract flexible_contract =
      contract(dmf::ContractId::from_value(2), dmf::ServiceClass::Standard, 10, 4000, 1000);

  std::uint64_t decision_counter = 0;
  const auto evaluate = [&](const dmf::ServiceContract& target,
                            const dmf::CapabilitySnapshot& snapshot) {
    dmf::EvaluationContext context;
    context.binding.vector.coordinator_term = dmf::CoordinatorTerm::from_value(1);
    context.binding.vector.boot = dmf::BootIncarnation::from_value(1);
    context.binding.vector.fabric = dmf::FabricGeneration::from_value(1);
    context.binding.vector.capacity = snapshot.generation;
    context.binding.vector.policy = active_policy.generation;
    context.binding.vector.evidence = dmf::EvidenceGeneration::from_value(1);
    context.binding.contract = target.id;
    context.binding.contract_generation = target.generation;
    context.binding.subject = target.subject;
    context.binding.subject_generation = target.subject_generation;
    context.binding.scope = target.scope;
    context.now = dmf::Tick{100};
    context.decision_id = dmf::DecisionId::from_value(++decision_counter);
    context.plan_id = dmf::PlanId::from_value(decision_counter);
    context.provenance = dmf::ProvenanceClass::Synthetic;

    dmf::ContractEvaluationRequest request;
    request.contract = &target;
    request.capability = &snapshot;
    request.evidence = &evidence;
    request.policy = &active_policy;
    request.context = context;
    return dmf::evaluate_contract(request);
  };

  show("full capability, both contracts",
       evaluate(protected_contract, capability(100000, 500, dmf::FabricTopologyClass::FullMesh)));
  show("degraded capability, standard traffic",
       evaluate(flexible_contract, capability(2500, 1500, dmf::FabricTopologyClass::PartialMesh)));
  show("degraded capability, protected traffic",
       evaluate(protected_contract, capability(2500, 1500, dmf::FabricTopologyClass::PartialMesh)));
  show("partitioned fabric, protected traffic",
       evaluate(protected_contract, capability(500, 900000, dmf::FabricTopologyClass::Partitioned)));

  dmf::CapabilitySnapshot unknown;
  unknown.scope = dmf::ScopeId::from_value(1);
  unknown.generation = dmf::CapacityGeneration::from_value(2);
  unknown.state = dmf::EvidenceState::Unknown;
  unknown.origin = dmf::OriginClass::Synthetic;
  show("no observation for the scope", evaluate(flexible_contract, unknown));
  return 0;
}
