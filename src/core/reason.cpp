// Degraded Mode Fabric - outcome and reason rendering.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/reason.hpp"

namespace dmf {

bool is_valid(DecisionOutcome value) noexcept {
  switch (value) {
    case DecisionOutcome::Full:
    case DecisionOutcome::Degraded:
    case DecisionOutcome::Refused:
    case DecisionOutcome::Escalated:
    case DecisionOutcome::Unsupported:
    case DecisionOutcome::Indeterminate:
    case DecisionOutcome::Invalid:
      return true;
  }
  return false;
}

std::string_view to_string(DecisionOutcome value) noexcept {
  switch (value) {
    case DecisionOutcome::Full: return "FULL";
    case DecisionOutcome::Degraded: return "DEGRADED";
    case DecisionOutcome::Refused: return "REFUSED";
    case DecisionOutcome::Escalated: return "ESCALATED";
    case DecisionOutcome::Unsupported: return "UNSUPPORTED";
    case DecisionOutcome::Indeterminate: return "INDETERMINATE";
    case DecisionOutcome::Invalid: return "INVALID";
  }
  return "UNRECOGNISED";
}

bool is_authorising(DecisionOutcome value) noexcept {
  return value == DecisionOutcome::Full || value == DecisionOutcome::Degraded;
}

bool is_valid(SupportTri value) noexcept {
  switch (value) {
    case SupportTri::Supported:
    case SupportTri::NotSupported:
    case SupportTri::Indeterminate:
    case SupportTri::NotModelled:
    case SupportTri::Invalid:
      return true;
  }
  return false;
}

std::string_view to_string(SupportTri value) noexcept {
  switch (value) {
    case SupportTri::Supported: return "SUPPORTED";
    case SupportTri::NotSupported: return "NOT_SUPPORTED";
    case SupportTri::Indeterminate: return "INDETERMINATE";
    case SupportTri::NotModelled: return "NOT_MODELLED";
    case SupportTri::Invalid: return "INVALID";
  }
  return "UNRECOGNISED";
}

bool is_valid(PlanStatus value) noexcept {
  switch (value) {
    case PlanStatus::Optimal:
    case PlanStatus::Feasible:
    case PlanStatus::ProvenInfeasible:
    case PlanStatus::SearchLimitReached:
    case PlanStatus::Indeterminate:
    case PlanStatus::Invalid:
      return true;
  }
  return false;
}

std::string_view to_string(PlanStatus value) noexcept {
  switch (value) {
    case PlanStatus::Optimal: return "OPTIMAL";
    case PlanStatus::Feasible: return "FEASIBLE";
    case PlanStatus::ProvenInfeasible: return "PROVEN_INFEASIBLE";
    case PlanStatus::SearchLimitReached: return "SEARCH_LIMIT_REACHED";
    case PlanStatus::Indeterminate: return "INDETERMINATE";
    case PlanStatus::Invalid: return "INVALID";
  }
  return "UNRECOGNISED";
}

bool plan_has_solution(PlanStatus value) noexcept {
  return value == PlanStatus::Optimal || value == PlanStatus::Feasible ||
         value == PlanStatus::SearchLimitReached;
}

bool is_valid(RestorationOutcome value) noexcept {
  switch (value) {
    case RestorationOutcome::Proven:
    case RestorationOutcome::NotProven:
    case RestorationOutcome::Indeterminate:
    case RestorationOutcome::Refused:
    case RestorationOutcome::Invalid:
      return true;
  }
  return false;
}

std::string_view to_string(RestorationOutcome value) noexcept {
  switch (value) {
    case RestorationOutcome::Proven: return "PROVEN";
    case RestorationOutcome::NotProven: return "NOT_PROVEN";
    case RestorationOutcome::Indeterminate: return "INDETERMINATE";
    case RestorationOutcome::Refused: return "REFUSED";
    case RestorationOutcome::Invalid: return "INVALID";
  }
  return "UNRECOGNISED";
}

bool is_valid(ReasonCode value) noexcept { return static_cast<std::uint16_t>(value) <= 29U; }

std::string_view to_string(ReasonCode value) noexcept {
  switch (value) {
    case ReasonCode::None: return "NONE";
    case ReasonCode::FabricCapabilityShortfall: return "FABRIC_CAPABILITY_SHORTFALL";
    case ReasonCode::FabricPartitioned: return "FABRIC_PARTITIONED";
    case ReasonCode::EvidenceUnknown: return "EVIDENCE_UNKNOWN";
    case ReasonCode::EvidenceStale: return "EVIDENCE_STALE";
    case ReasonCode::EvidenceConflict: return "EVIDENCE_CONFLICT";
    case ReasonCode::EvidenceInvalid: return "EVIDENCE_INVALID";
    case ReasonCode::EvidenceUnsupported: return "EVIDENCE_UNSUPPORTED";
    case ReasonCode::EvidenceAbsent: return "EVIDENCE_ABSENT";
    case ReasonCode::ProtectedObligationUnmet: return "PROTECTED_OBLIGATION_UNMET";
    case ReasonCode::PolicyNoRule: return "POLICY_NO_RULE";
    case ReasonCode::PolicyEnvelopeExceeded: return "POLICY_ENVELOPE_EXCEEDED";
    case ReasonCode::PolicyRequiresProtection: return "POLICY_REQUIRES_PROTECTION";
    case ReasonCode::ResourceExhausted: return "RESOURCE_EXHAUSTED";
    case ReasonCode::SearchLimitReached: return "SEARCH_LIMIT_REACHED";
    case ReasonCode::AuthorityGenerationChanged: return "AUTHORITY_GENERATION_CHANGED";
    case ReasonCode::AuthorityExpired: return "AUTHORITY_EXPIRED";
    case ReasonCode::AuthorityRevoked: return "AUTHORITY_REVOKED";
    case ReasonCode::CoordinatorEpochAdvanced: return "COORDINATOR_EPOCH_ADVANCED";
    case ReasonCode::BootIncarnationAdvanced: return "BOOT_INCARNATION_ADVANCED";
    case ReasonCode::ContractInvalid: return "CONTRACT_INVALID";
    case ReasonCode::ContractGenerationChanged: return "CONTRACT_GENERATION_CHANGED";
    case ReasonCode::ScopeUnknown: return "SCOPE_UNKNOWN";
    case ReasonCode::QuotaExceeded: return "QUOTA_EXCEEDED";
    case ReasonCode::NoRuleForServiceClass: return "NO_RULE_FOR_SERVICE_CLASS";
    case ReasonCode::ConcessionWithinEnvelope: return "CONCESSION_WITHIN_ENVELOPE";
    case ReasonCode::OriginalObligationsSupportable: return "ORIGINAL_OBLIGATIONS_SUPPORTABLE";
    case ReasonCode::DwellRequirementUnmet: return "DWELL_REQUIREMENT_UNMET";
    case ReasonCode::HardwareNotExercised: return "HARDWARE_NOT_EXERCISED";
    case ReasonCode::ArithmeticOverflow: return "ARITHMETIC_OVERFLOW";
  }
  return "UNRECOGNISED";
}

bool is_valid(ProvenanceClass value) noexcept {
  switch (value) {
    case ProvenanceClass::Real:
    case ProvenanceClass::Synthetic:
    case ProvenanceClass::Unsupported:
      return true;
  }
  return false;
}

std::string_view to_string(ProvenanceClass value) noexcept {
  switch (value) {
    case ProvenanceClass::Real: return "REAL";
    case ProvenanceClass::Synthetic: return "SYNTHETIC";
    case ProvenanceClass::Unsupported: return "UNSUPPORTED";
  }
  return "UNRECOGNISED";
}

}  // namespace dmf
