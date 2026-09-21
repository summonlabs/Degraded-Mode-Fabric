// Degraded Mode Fabric - outcome and reason vocabulary.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally visible decision uses exactly one of these vocabularies.
// Nothing in this header maps UNKNOWN/STALE/CONFLICT/INVALID/UNSUPPORTED onto a
// success value: they are distinct, first-class outcomes.
#ifndef DMF_REASON_HPP
#define DMF_REASON_HPP

#include <cstdint>
#include <string_view>

#include "dmf/core.hpp"

namespace dmf {

/// Terminal outcomes of a single-contract degradation evaluation.
enum class DecisionOutcome : std::uint16_t {
  Full = 1,           ///< original obligations are supportable; no concession
  Degraded = 2,       ///< explicit, bounded, explained concession granted
  Refused = 3,        ///< obligation cannot be met and must not be weakened
  Escalated = 4,      ///< requires authority outside this runtime
  Unsupported = 5,    ///< no policy/evidence can authorise any action
  Indeterminate = 6,  ///< bounded analysis could not decide
  Invalid = 7,        ///< inputs were invalid; nothing was authorised
};

bool is_valid(DecisionOutcome value) noexcept;
std::string_view to_string(DecisionOutcome value) noexcept;
/// True only when the outcome authorises continued service. Anything else must
/// be surfaced, never silently treated as normal operation.
bool is_authorising(DecisionOutcome value) noexcept;

/// Tri-state support classification for one guarantee against observed
/// capability. Absence of proof is Indeterminate, never Supported.
enum class SupportTri : std::uint16_t {
  Supported = 1,
  NotSupported = 2,
  Indeterminate = 3,  ///< evidence is not Known: support cannot be proven
  NotModelled = 4,    ///< the capability model does not cover this guarantee
  Invalid = 5,
};

bool is_valid(SupportTri value) noexcept;
std::string_view to_string(SupportTri value) noexcept;

/// Result quality of an allocation plan. Feasible never implies optimal.
enum class PlanStatus : std::uint16_t {
  Optimal = 1,             ///< exact solver proved optimality for the instance
  Feasible = 2,            ///< a valid plan exists; optimality is not claimed
  ProvenInfeasible = 3,    ///< exact solver proved no plan satisfies hard rules
  SearchLimitReached = 4,  ///< bounded search exhausted its budget
  Indeterminate = 5,       ///< no plan found and no proof of infeasibility
  Invalid = 6,             ///< inputs invalid
};

bool is_valid(PlanStatus value) noexcept;
std::string_view to_string(PlanStatus value) noexcept;
/// True when the plan carries at least one admissible allocation set.
bool plan_has_solution(PlanStatus value) noexcept;

/// Outcome of a restoration evaluation. UNKNOWN evidence can never yield
/// Proven.
enum class RestorationOutcome : std::uint16_t {
  Proven = 1,
  NotProven = 2,
  Indeterminate = 3,
  Refused = 4,
  Invalid = 5,
};

bool is_valid(RestorationOutcome value) noexcept;
std::string_view to_string(RestorationOutcome value) noexcept;

/// Deterministic reason vocabulary. Every decision and grant carries exactly one
/// primary reason plus an ordered, bounded reason list.
enum class ReasonCode : std::uint16_t {
  None = 0,
  FabricCapabilityShortfall = 1,
  FabricPartitioned = 2,
  EvidenceUnknown = 3,
  EvidenceStale = 4,
  EvidenceConflict = 5,
  EvidenceInvalid = 6,
  EvidenceUnsupported = 7,
  EvidenceAbsent = 8,
  ProtectedObligationUnmet = 9,
  PolicyNoRule = 10,
  PolicyEnvelopeExceeded = 11,
  PolicyRequiresProtection = 12,
  ResourceExhausted = 13,
  SearchLimitReached = 14,
  AuthorityGenerationChanged = 15,
  AuthorityExpired = 16,
  AuthorityRevoked = 17,
  CoordinatorEpochAdvanced = 18,
  BootIncarnationAdvanced = 19,
  ContractInvalid = 20,
  ContractGenerationChanged = 21,
  ScopeUnknown = 22,
  QuotaExceeded = 23,
  NoRuleForServiceClass = 24,
  ConcessionWithinEnvelope = 25,
  OriginalObligationsSupportable = 26,
  DwellRequirementUnmet = 27,
  HardwareNotExercised = 28,
  ArithmeticOverflow = 29,
};

bool is_valid(ReasonCode value) noexcept;
std::string_view to_string(ReasonCode value) noexcept;

/// Provenance classification for a decision or grant: whether it rests on real
/// exercised inputs, synthetic fixtures, or inputs the host cannot provide.
enum class ProvenanceClass : std::uint16_t {
  Real = 1,
  Synthetic = 2,
  Unsupported = 3,
};

bool is_valid(ProvenanceClass value) noexcept;
std::string_view to_string(ProvenanceClass value) noexcept;

}  // namespace dmf

#endif  // DMF_REASON_HPP
