// Degraded Mode Fabric - shared deterministic fixtures for the test suites.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every fixture here is SYNTHETIC. Nothing in this header exercises physical
// fabric hardware, and no test may claim that it did.
#ifndef DMF_TESTKIT_FIXTURES_HPP
#define DMF_TESTKIT_FIXTURES_HPP

#include <string>
#include <vector>

#include "dmf/engine.hpp"
#include "dmf/runtime.hpp"

namespace dmf::test {

inline ScopeId scope_id(std::uint64_t value) { return ScopeId::from_value(value); }
inline SubjectId subject_id(std::uint64_t value) { return SubjectId::from_value(value); }
inline ContractId contract_id(std::uint64_t value) { return ContractId::from_value(value); }

/// The demonstration policy: rigid obligations first, bounded concessions for
/// everyone else, fail-closed on evidence that is not Known.
inline Policy demonstration_policy(PolicyGeneration generation = PolicyGeneration::from_value(1),
                                   std::uint64_t freshness_ticks = 1000,
                                   std::uint32_t minimum_protected_served = 0) {
  Policy policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = generation;
  policy.protect_first = true;
  policy.default_ttl_ticks = Tick{50};
  policy.minimum_ttl_ticks = Tick{5};
  policy.max_concessions_per_contract = 4;
  policy.minimum_dwell_ticks = 0;
  policy.search_node_budget = 20000;
  policy.evidence_freshness_ticks = freshness_ticks;
  policy.minimum_protected_served = minimum_protected_served;
  policy.unknown_evidence_action = DegradeAction::Refuse;
  policy.stale_evidence_action = DegradeAction::Refuse;
  policy.conflict_evidence_action = DegradeAction::Refuse;
  policy.invalid_evidence_action = DegradeAction::Refuse;
  policy.unsupported_evidence_action = DegradeAction::Refuse;

  PolicyRule degrade;
  degrade.id = RuleId::from_value(1);
  degrade.precedence = 10;
  degrade.action = DegradeAction::Degrade;
  policy.rules.push_back(degrade);

  PolicyRule protect;
  protect.id = RuleId::from_value(2);
  protect.precedence = 1;
  protect.service_class = ServiceClass::Protected;
  protect.action = DegradeAction::Protect;
  policy.rules.push_back(protect);

  ClassProfile rigid;
  rigid.service_class = ServiceClass::Protected;
  rigid.may_be_degraded = false;
  rigid.ceiling.set_max_concessions(0);
  policy.class_profiles.push_back(rigid);

  for (const ServiceClass service_class :
       {ServiceClass::Standard, ServiceClass::BestEffort, ServiceClass::Scavenger}) {
    ClassProfile profile;
    profile.service_class = service_class;
    profile.may_be_degraded = true;
    profile.ceiling.set_max_concessions(4);
    policy.class_profiles.push_back(profile);
  }
  return policy;
}

/// A contract whose obligations a fully capable scope satisfies exactly.
inline ServiceContract contract(std::uint64_t id, ServiceClass service_class, std::uint32_t priority,
                                std::uint64_t bandwidth_kbps, std::uint64_t latency_us,
                                std::uint64_t bandwidth_floor_kbps, std::uint32_t max_concessions,
                                std::uint64_t scope = 1) {
  ServiceContract value;
  value.id = contract_id(id);
  value.generation = ContractGeneration::from_value(1);
  value.scope = scope_id(scope);
  value.subject = subject_id(id);
  value.subject_generation = SubjectGeneration::from_value(1);
  value.service_class = service_class;
  value.non_degradable = service_class == ServiceClass::Protected;
  value.priority = priority;
  (void)value.original.insert(GuaranteeKind::Bandwidth, bandwidth_kbps);
  (void)value.original.insert(GuaranteeKind::LatencyP99, latency_us);
  (void)value.original.insert(GuaranteeKind::PathDiversity, 1);
  (void)value.original.insert(GuaranteeKind::Reachability, 900000);
  if (!value.protected_obligation()) {
    (void)value.envelope.insert(ConcessionBound{GuaranteeKind::Bandwidth, bandwidth_floor_kbps});
    (void)value.envelope.insert(ConcessionBound{GuaranteeKind::LatencyP99, 100000});
    value.envelope.set_max_concessions(max_concessions);
  }
  return value;
}

struct CapabilitySpec {
  std::uint64_t bandwidth_kbps = 100000;
  std::uint64_t rtt_us = 1000;
  std::uint32_t path_diversity = 4;
  std::uint32_t reachable_ppm = 1000000;
  std::uint32_t node_coverage_ppm = 1000000;
  bool synchronous_durability = true;
  FabricTopologyClass topology = FabricTopologyClass::FullMesh;
  EvidenceState state = EvidenceState::Known;
  CapacityGeneration generation = CapacityGeneration::from_value(1);
  Tick observed_tick{1};
  std::uint64_t scope = 1;
};

inline CapabilitySnapshot capability(const CapabilitySpec& spec) {
  CapabilitySnapshot snapshot;
  snapshot.scope = scope_id(spec.scope);
  snapshot.generation = spec.generation;
  snapshot.state = spec.state;
  snapshot.topology = spec.topology;
  snapshot.bandwidth_kbps = spec.bandwidth_kbps;
  snapshot.rtt_p99_us = spec.rtt_us;
  snapshot.path_diversity = spec.path_diversity;
  snapshot.reachable_ppm = spec.reachable_ppm;
  snapshot.node_coverage_ppm = spec.node_coverage_ppm;
  snapshot.synchronous_durability = spec.synchronous_durability;
  snapshot.observed_tick = spec.observed_tick;
  snapshot.origin = OriginClass::Synthetic;
  return snapshot;
}

inline EvidenceItem evidence_item(std::uint64_t id, std::uint64_t publisher, EvidenceKind kind,
                                  std::uint64_t value, std::uint64_t scope, Tick observed,
                                  BootIncarnation boot = BootIncarnation::from_value(1),
                                  EvidenceState state = EvidenceState::Known,
                                  EvidenceGeneration generation = EvidenceGeneration::from_value(1)) {
  EvidenceItem item;
  item.id = EvidenceId::from_value(id);
  item.publisher = PublisherId::from_value(publisher);
  item.publisher_boot = boot;
  item.generation = generation;
  item.kind = kind;
  item.scope = scope_id(scope);
  item.state = state;
  item.value = value;
  item.observed_tick = observed;
  item.origin = OriginClass::Synthetic;
  return item;
}

/// Publishes one complete SYNTHETIC capability observation for a scope.
inline Status publish_capability(Coordinator& coordinator, std::uint64_t scope,
                                 std::uint64_t publisher, const CapabilitySpec& spec,
                                 EvidenceGeneration generation = EvidenceGeneration::from_value(1)) {
  const BootIncarnation boot = coordinator.authority().boot;
  const Tick now = coordinator.now();
  struct Entry {
    EvidenceKind kind;
    std::uint64_t value;
  };
  const Entry entries[] = {
      {EvidenceKind::FabricTopology, static_cast<std::uint64_t>(spec.topology)},
      {EvidenceKind::AggregateBandwidth, spec.bandwidth_kbps},
      {EvidenceKind::RoundTripLatency, spec.rtt_us},
      {EvidenceKind::PathDiversity, spec.path_diversity},
      {EvidenceKind::Reachability, spec.reachable_ppm},
      {EvidenceKind::NodeCoverage, spec.node_coverage_ppm},
      {EvidenceKind::SynchronousDurability, spec.synchronous_durability ? 1ULL : 0ULL},
  };
  std::uint64_t ordinal = 1;
  for (const Entry& entry : entries) {
    const Status status = coordinator.publish_evidence(
        evidence_item(ordinal++, publisher, entry.kind, entry.value, scope, now, boot,
                      EvidenceState::Known, generation));
    if (!status.ok()) return status;
  }
  return Status{};
}

/// Builds the authority binding a standalone evaluation needs.
inline AuthorityBinding binding_for(const ServiceContract& contract, CapacityGeneration capacity) {
  AuthorityBinding binding;
  binding.vector.coordinator_term = CoordinatorTerm::from_value(1);
  binding.vector.boot = BootIncarnation::from_value(1);
  binding.vector.fabric = FabricGeneration::from_value(1);
  binding.vector.capacity = capacity;
  binding.vector.policy = PolicyGeneration::from_value(1);
  binding.vector.evidence = EvidenceGeneration::from_value(1);
  binding.contract = contract.id;
  binding.contract_generation = contract.generation;
  binding.subject = contract.subject;
  binding.subject_generation = contract.subject_generation;
  binding.scope = contract.scope;
  return binding;
}

/// Evaluates one contract against a snapshot without a store, a socket or a
/// coordinator. Deterministic: the identity counters are supplied by the caller.
inline Decision evaluate_direct(const ServiceContract& contract, const CapabilitySnapshot& snapshot,
                                const Policy& policy, std::uint64_t decision_ordinal,
                                Tick now = Tick{100}) {
  EvaluationContext context;
  context.binding = binding_for(contract, snapshot.generation);
  context.now = now;
  context.decision_id = DecisionId::from_value(decision_ordinal);
  context.plan_id = PlanId::from_value(decision_ordinal);
  context.provenance = ProvenanceClass::Synthetic;
  ContractEvaluationRequest request;
  request.contract = &contract;
  request.capability = &snapshot;
  request.policy = &policy;
  request.context = context;
  return evaluate_contract(request);
}

/// Allocates a whole scope directly, which is how contention is exercised.
inline Result<AllocationPlan> allocate_direct(const std::vector<const ServiceContract*>& contracts,
                                              const CapabilitySnapshot& snapshot,
                                              const Policy& policy, std::uint64_t plan_ordinal,
                                              Tick now = Tick{100}) {
  EvaluationContext context;
  context.binding.vector.coordinator_term = CoordinatorTerm::from_value(1);
  context.binding.vector.boot = BootIncarnation::from_value(1);
  context.binding.vector.fabric = FabricGeneration::from_value(1);
  context.binding.vector.capacity = snapshot.generation;
  context.binding.vector.policy = policy.generation;
  context.binding.vector.evidence = EvidenceGeneration::from_value(1);
  context.binding.scope = snapshot.scope;
  context.now = now;
  context.plan_id = PlanId::from_value(plan_ordinal);
  AllocationRequest request;
  request.scope = snapshot.scope;
  request.contracts = contracts;
  request.capability = &snapshot;
  request.policy = &policy;
  request.context = context;
  return allocate_scope(request);
}

/// The product-defining invariant, expressed once and reused everywhere.
inline bool never_exceeds(const GuaranteeSet& original, const GuaranteeSet& approved) {
  return weakens_or_equals(original, approved);
}

/// Evaluates through a live coordinator, returning the decision together with
/// the scope allocation that produced it.
inline Result<EvaluationView> evaluate_view(Coordinator& coordinator, ContractId contract) {
  return coordinator.evaluate_with_plan(contract);
}

}  // namespace dmf::test

#endif  // DMF_TESTKIT_FIXTURES_HPP
