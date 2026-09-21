// Degraded Mode Fabric - restoration evaluation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Restoration is a transition, not a state. It requires positive proof that the
// original obligation is again supportable under current evidence; UNKNOWN
// evidence can never yield PROVEN.
#include <algorithm>

#include "dmf/engine.hpp"

namespace dmf {

std::vector<RestorationPrecondition> make_preconditions(const ServiceContract& contract,
                                                        const Policy& policy) {
  std::vector<RestorationPrecondition> preconditions;
  const auto push = [&preconditions](RestorationPrecondition precondition) {
    if (preconditions.size() < kMaxPreconditions) preconditions.push_back(precondition);
  };

  RestorationPrecondition evidence;
  evidence.kind = PreconditionKind::EvidenceKnown;
  evidence.scope = contract.scope;
  push(evidence);

  RestorationPrecondition scope;
  scope.kind = PreconditionKind::ScopeRecovered;
  scope.scope = contract.scope;
  push(scope);

  for (const Guarantee& guarantee : contract.original.items()) {
    RestorationPrecondition support;
    support.kind = PreconditionKind::CapabilitySupports;
    support.guarantee = guarantee.kind;
    support.scope = contract.scope;
    support.required_value = guarantee.value;
    push(support);
  }

  RestorationPrecondition permit;
  permit.kind = PreconditionKind::PolicyPermits;
  permit.scope = contract.scope;
  push(permit);

  RestorationPrecondition unchanged;
  unchanged.kind = PreconditionKind::ContractUnchanged;
  unchanged.scope = contract.scope;
  push(unchanged);

  RestorationPrecondition fence;
  fence.kind = PreconditionKind::NoActiveFence;
  fence.scope = contract.scope;
  push(fence);

  RestorationPrecondition authority;
  authority.kind = PreconditionKind::AuthorityUnchanged;
  authority.scope = contract.scope;
  push(authority);

  RestorationPrecondition dwell;
  dwell.kind = PreconditionKind::MinimumDwell;
  dwell.scope = contract.scope;
  dwell.dwell_ticks = policy.minimum_dwell_ticks;
  push(dwell);

  return preconditions;
}

namespace {

PreconditionEvaluation evaluate_one(const RestorationPrecondition& precondition,
                                    const RestorationRequest& request, const Policy& policy,
                                    const ServiceContract& contract) {
  PreconditionEvaluation evaluation;
  evaluation.precondition = precondition;
  const CapabilitySnapshot* capability = request.capability;
  const Guarantee* guarantee = contract.original.find(precondition.guarantee);

  switch (precondition.kind) {
    case PreconditionKind::EvidenceKnown: {
      if (capability == nullptr) {
        evaluation.result = PreconditionResult::Indeterminate;
        evaluation.reason = ReasonCode::EvidenceUnknown;
        break;
      }
      if (capability->usable()) {
        evaluation.result = PreconditionResult::Satisfied;
        break;
      }
      if (capability->state == EvidenceState::Known) {
        evaluation.result = PreconditionResult::Satisfied;
        break;
      }
      evaluation.result = PreconditionResult::Indeterminate;
      switch (capability->state) {
        case EvidenceState::Stale: evaluation.reason = ReasonCode::EvidenceStale; break;
        case EvidenceState::Conflict: evaluation.reason = ReasonCode::EvidenceConflict; break;
        case EvidenceState::Invalid: evaluation.reason = ReasonCode::EvidenceInvalid; break;
        case EvidenceState::Unsupported: evaluation.reason = ReasonCode::EvidenceUnsupported; break;
        case EvidenceState::Unknown:
        case EvidenceState::Known:
          evaluation.reason = ReasonCode::EvidenceUnknown;
          break;
      }
      break;
    }
    case PreconditionKind::ScopeRecovered: {
      if (capability == nullptr) {
        evaluation.result = PreconditionResult::Indeterminate;
        evaluation.reason = ReasonCode::EvidenceUnknown;
        break;
      }
      if (capability->topology == FabricTopologyClass::Partitioned ||
          capability->topology == FabricTopologyClass::Isolated) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::FabricPartitioned;
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::CapabilitySupports: {
      if (capability == nullptr || guarantee == nullptr) {
        evaluation.result = PreconditionResult::Indeterminate;
        evaluation.reason = ReasonCode::EvidenceUnknown;
        break;
      }
      const Guarantee demand = *guarantee;
      const SupportTri tri = supports(*capability, demand);
      switch (tri) {
        case SupportTri::Supported:
          evaluation.result = PreconditionResult::Satisfied;
          break;
        case SupportTri::NotSupported:
          evaluation.result = PreconditionResult::NotSatisfied;
          evaluation.reason = shortfall_reason(*capability, demand);
          break;
        case SupportTri::Indeterminate:
          evaluation.result = PreconditionResult::Indeterminate;
          evaluation.reason = shortfall_reason(*capability, demand);
          break;
        case SupportTri::NotModelled:
          evaluation.result = PreconditionResult::Unsupported;
          evaluation.reason = ReasonCode::EvidenceUnsupported;
          break;
        case SupportTri::Invalid:
          evaluation.result = PreconditionResult::Unsupported;
          evaluation.reason = ReasonCode::EvidenceInvalid;
          break;
      }
      break;
    }
    case PreconditionKind::PolicyPermits: {
      const PolicyRule* rule = policy.select_rule(contract.service_class, contract.scope, std::nullopt);
      if (rule != nullptr && (rule->action == DegradeAction::Refuse ||
                              rule->action == DegradeAction::Escalate)) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::PolicyNoRule;
        break;
      }
      const ClassProfile* profile = policy.class_profile(contract.service_class);
      if (profile == nullptr) {
        evaluation.result = PreconditionResult::Unsupported;
        evaluation.reason = ReasonCode::NoRuleForServiceClass;
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::ContractUnchanged: {
      if (request.grant == nullptr) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::ContractInvalid;
        break;
      }
      const AuthorityBinding& binding = request.grant->binding;
      if (binding.contract != contract.id || binding.contract_generation != contract.generation) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::ContractGenerationChanged;
        break;
      }
      if (binding.subject != contract.subject ||
          binding.subject_generation != contract.subject_generation) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::AuthorityGenerationChanged;
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::NoActiveFence: {
      if (request.fence_active) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::AuthorityGenerationChanged;
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::AuthorityUnchanged: {
      if (request.grant == nullptr) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::AuthorityGenerationChanged;
        break;
      }
      AuthorityBinding current = request.grant->binding;
      current.vector = request.current_authority;
      const AuthorityDelta delta = request.grant->binding.classify(current);
      if (invalidates_authority(delta)) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = has_flag(delta, AuthorityDelta::Boot)
                                ? ReasonCode::BootIncarnationAdvanced
                                : (has_flag(delta, AuthorityDelta::CoordinatorTerm)
                                       ? ReasonCode::CoordinatorEpochAdvanced
                                       : ReasonCode::AuthorityGenerationChanged);
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::MinimumDwell: {
      if (request.grant == nullptr) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::ContractInvalid;
        break;
      }
      if (request.now < request.grant->issued_tick) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::DwellRequirementUnmet;
        break;
      }
      const auto held = checked_sub(request.now.value, request.grant->issued_tick.value);
      if (!held.has_value() || *held < precondition.dwell_ticks) {
        evaluation.result = PreconditionResult::NotSatisfied;
        evaluation.reason = ReasonCode::DwellRequirementUnmet;
        break;
      }
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
    case PreconditionKind::EscalationClear: {
      evaluation.result = PreconditionResult::Satisfied;
      break;
    }
  }
  return evaluation;
}

bool blocks_authority(PreconditionKind kind) noexcept {
  return kind == PreconditionKind::NoActiveFence || kind == PreconditionKind::PolicyPermits ||
         kind == PreconditionKind::AuthorityUnchanged ||
         kind == PreconditionKind::ContractUnchanged;
}

}  // namespace

RestorationEvaluation evaluate_restoration(const RestorationRequest& request) {
  RestorationEvaluation evaluation;
  evaluation.evaluated_tick = request.now;
  evaluation.provenance = request.provenance;
  if (request.grant != nullptr) {
    evaluation.grant = request.grant->id;
    evaluation.decision = request.grant->decision;
    evaluation.binding = request.grant->binding;
  }
  if (request.contract != nullptr) evaluation.original = request.contract->original;
  if (request.grant != nullptr) evaluation.degraded = request.grant->degraded;
  evaluation.evidence_state = request.capability != nullptr ? request.capability->state
                                                            : EvidenceState::Unknown;
  evaluation.outcome = RestorationOutcome::Invalid;
  evaluation.primary_reason = ReasonCode::ContractInvalid;

  if (request.grant == nullptr || request.contract == nullptr || request.policy == nullptr) {
    return evaluation;
  }
  if (!request.grant->validate().ok() || !request.contract->validate().ok() ||
      !request.policy->validate().ok()) {
    return evaluation;
  }

  if (!request.grant->live()) {
    evaluation.outcome = RestorationOutcome::Refused;
    switch (request.grant->state) {
      case GrantState::Fenced:
        evaluation.primary_reason = ReasonCode::AuthorityGenerationChanged;
        break;
      case GrantState::Expired:
        evaluation.primary_reason = ReasonCode::AuthorityExpired;
        break;
      case GrantState::Revoked:
        evaluation.primary_reason = ReasonCode::AuthorityRevoked;
        break;
      case GrantState::Issued:
      case GrantState::Acknowledged:
      case GrantState::Applied:
        break;
    }
    return evaluation;
  }

  const ServiceContract& contract = *request.contract;
  const Policy& policy = *request.policy;
  std::vector<RestorationPrecondition> preconditions = request.grant->preconditions;
  if (preconditions.empty()) preconditions = make_preconditions(contract, policy);

  bool any_unsupported = false;
  bool any_blocking = false;
  bool any_not_satisfied = false;
  bool any_indeterminate = false;
  evaluation.primary_reason = ReasonCode::None;

  for (const RestorationPrecondition& precondition : preconditions) {
    PreconditionEvaluation result = evaluate_one(precondition, request, policy, contract);
    if (result.result != PreconditionResult::Satisfied) {
      if (evaluation.primary_reason == ReasonCode::None) {
        evaluation.primary_reason = result.reason;
      }
      switch (result.result) {
        case PreconditionResult::Unsupported: any_unsupported = true; break;
        case PreconditionResult::Indeterminate: any_indeterminate = true; break;
        case PreconditionResult::NotSatisfied:
          any_not_satisfied = true;
          if (blocks_authority(precondition.kind)) any_blocking = true;
          break;
        case PreconditionResult::Satisfied: break;
      }
    }
    evaluation.preconditions.push_back(result);
  }

  if (any_unsupported || any_blocking) {
    evaluation.outcome = RestorationOutcome::Refused;
  } else if (any_not_satisfied) {
    evaluation.outcome = RestorationOutcome::NotProven;
  } else if (any_indeterminate) {
    evaluation.outcome = RestorationOutcome::Indeterminate;
  } else {
    evaluation.outcome = RestorationOutcome::Proven;
    evaluation.primary_reason = ReasonCode::OriginalObligationsSupportable;
  }

  if (!evaluation.validate().ok()) {
    RestorationEvaluation invalid;
    invalid.outcome = RestorationOutcome::Invalid;
    invalid.grant = evaluation.grant;
    invalid.decision = evaluation.decision;
    invalid.binding = evaluation.binding;
    invalid.evaluated_tick = request.now;
    invalid.provenance = request.provenance;
    return invalid;
  }
  return evaluation;
}

}  // namespace dmf
