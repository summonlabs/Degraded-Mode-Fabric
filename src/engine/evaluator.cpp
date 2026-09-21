// Degraded Mode Fabric - single-contract decision evaluation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <limits>

#include "dmf/engine.hpp"

namespace dmf {

namespace {

void push_unique(std::vector<ReasonCode>& reasons, ReasonCode reason) {
  if (reason == ReasonCode::None) return;
  if (reasons.size() >= kMaxReasonsPerDecision) return;
  if (std::find(reasons.begin(), reasons.end(), reason) != reasons.end()) return;
  reasons.push_back(reason);
}

Decision invalid_decision(const ContractEvaluationRequest& request) {
  Decision decision;
  decision.id = request.context.decision_id;
  decision.binding = request.context.binding;
  decision.decided_tick = request.context.now;
  decision.expires_tick = request.context.now;
  decision.provenance = request.context.provenance;
  decision.outcome = DecisionOutcome::Invalid;
  decision.primary_reason = ReasonCode::ContractInvalid;
  decision.reasons.push_back(ReasonCode::ContractInvalid);
  decision.max_authority = AuthorityLevel::None;
  decision.plan_status = PlanStatus::Invalid;
  decision.evidence_state = EvidenceState::Unknown;
  decision.escalation_required = true;
  if (request.contract != nullptr) {
    decision.service_class = request.contract->service_class;
    decision.protected_obligation = request.contract->protected_obligation();
    decision.priority = request.contract->priority;
    decision.original = request.contract->original;
  }
  return decision;
}

}  // namespace

Decision evaluate_contract(const ContractEvaluationRequest& request) {
  if (request.contract == nullptr || request.capability == nullptr || request.policy == nullptr) {
    return invalid_decision(request);
  }
  const ServiceContract& contract = *request.contract;
  const CapabilitySnapshot& capability = *request.capability;
  const Policy& policy = *request.policy;

  if (!contract.validate().ok()) return invalid_decision(request);
  if (!policy.validate().ok()) return invalid_decision(request);
  if (!request.context.decision_id.valid()) return invalid_decision(request);
  if (!request.context.binding.valid()) return invalid_decision(request);
  const AuthorityBinding& binding = request.context.binding;
  if (binding.contract != contract.id || binding.contract_generation != contract.generation ||
      binding.subject != contract.subject ||
      binding.subject_generation != contract.subject_generation ||
      binding.scope != contract.scope || binding.scope != capability.scope) {
    return invalid_decision(request);
  }

  Decision decision;
  decision.id = request.context.decision_id;
  decision.binding = binding;
  decision.service_class = contract.service_class;
  decision.protected_obligation = contract.protected_obligation();
  decision.priority = contract.priority;
  decision.original = contract.original;
  decision.decided_tick = request.context.now;
  decision.provenance = request.context.provenance;
  decision.evidence_state = capability.state;
  if (request.evidence != nullptr) decision.evidence = *request.evidence;

  for (const Guarantee& guarantee : contract.original.items()) {
    GuaranteeSupport entry;
    entry.kind = guarantee.kind;
    entry.support = supports(capability, guarantee);
    entry.reason = (entry.support == SupportTri::Supported)
                       ? ReasonCode::None
                       : shortfall_reason(capability, guarantee);
    decision.support.push_back(entry);
  }

  const AllocationEntry* allocation = request.allocation;
  AllocationEntry produced;
  PlanStatus plan_status = request.plan_status;
  if (allocation == nullptr) {
    AllocationRequest allocation_request;
    allocation_request.scope = contract.scope;
    allocation_request.contracts.push_back(&contract);
    allocation_request.capability = &capability;
    allocation_request.evidence = request.evidence;
    allocation_request.policy = &policy;
    allocation_request.context = request.context;
    auto plan = allocate_scope(allocation_request);
    if (!plan.ok()) return invalid_decision(request);
    plan_status = plan.value().status;
    const AllocationEntry* found = plan.value().find(contract.id);
    if (found == nullptr) return invalid_decision(request);
    produced = *found;
    allocation = &produced;
  }

  decision.plan_status = plan_status;
  decision.approved = allocation->approved;
  decision.delta = allocation->delta;
  decision.outcome = allocation->outcome;
  decision.primary_reason = allocation->reason;
  push_unique(decision.reasons, allocation->reason);

  // An evaluation is never authority by itself. It recommends service when the
  // obligations are supportable, records that the subject is inside the
  // degradation regime when it is not but could be degraded, and otherwise
  // reports only what was observed. Only a committed grant reaches Authorized.
  const bool authorising = is_authorising(decision.outcome);
  if (authorising) {
    decision.max_authority = AuthorityLevel::Recommended;
  } else if (decision.outcome == DecisionOutcome::Invalid) {
    decision.max_authority = AuthorityLevel::None;
  } else if (!is_rigid(contract) && capability.usable()) {
    decision.max_authority = AuthorityLevel::Eligible;
  } else {
    decision.max_authority = AuthorityLevel::Observed;
  }
  decision.escalation_required =
      decision.outcome == DecisionOutcome::Escalated ||
      decision.outcome == DecisionOutcome::Unsupported ||
      decision.outcome == DecisionOutcome::Indeterminate ||
      (decision.outcome == DecisionOutcome::Refused && decision.protected_obligation) ||
      plan_status == PlanStatus::ProvenInfeasible;

  for (const GuaranteeSupport& entry : decision.support) push_unique(decision.reasons, entry.reason);
  if (plan_status == PlanStatus::ProvenInfeasible) push_unique(decision.reasons, ReasonCode::ResourceExhausted);
  if (plan_status == PlanStatus::SearchLimitReached) push_unique(decision.reasons, ReasonCode::SearchLimitReached);
  if (capability.origin == OriginClass::Synthetic || capability.origin == OriginClass::Unspecified) {
    push_unique(decision.reasons, ReasonCode::HardwareNotExercised);
  }

  if (authorising) {
    Tick ttl = policy.default_ttl_ticks;
    if (policy.minimum_ttl_ticks > ttl) ttl = policy.minimum_ttl_ticks;
    const auto expires = checked_add(request.context.now.value, ttl.value);
    decision.expires_tick =
        expires.has_value() ? Tick{*expires} : Tick{std::numeric_limits<std::uint64_t>::max()};
  } else {
    decision.expires_tick = request.context.now;
  }

  if (!decision.validate().ok()) return invalid_decision(request);
  return decision;
}

bool full_service_supported(const ServiceContract& contract,
                            const CapabilitySnapshot& capability) noexcept {
  if (!capability.usable()) return false;
  if (!contract.validate().ok()) return false;
  for (const Guarantee& guarantee : contract.original.items()) {
    if (supports(capability, guarantee) != SupportTri::Supported) return false;
  }
  return true;
}

}  // namespace dmf
