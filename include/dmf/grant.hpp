// Degraded Mode Fabric - committed degraded authority, fences and restoration.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef DMF_GRANT_HPP
#define DMF_GRANT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/authority.hpp"
#include "dmf/contract.hpp"
#include "dmf/core.hpp"
#include "dmf/decision.hpp"
#include "dmf/evidence.hpp"
#include "dmf/ids.hpp"
#include "dmf/reason.hpp"

namespace dmf {

/// A condition that must hold before normal service may be restored.
enum class PreconditionKind : std::uint16_t {
  EvidenceKnown = 1,           ///< required evidence groups are Known and fresh
  CapabilitySupports = 2,      ///< capability proves the original obligation
  NoActiveFence = 3,           ///< no outstanding fence covers the subject
  PolicyPermits = 4,           ///< policy permits a return to full service
  ContractUnchanged = 5,       ///< contract identity and generation unchanged
  MinimumDwell = 6,            ///< grant has been held long enough
  ScopeRecovered = 7,          ///< scope topology is no longer partitioned
  AuthorityUnchanged = 8,      ///< every generation the grant was bound to is current
  EscalationClear = 9,         ///< no escalation is outstanding for the subject
};

bool is_valid(PreconditionKind value) noexcept;
std::string_view to_string(PreconditionKind value) noexcept;

struct RestorationPrecondition {
  PreconditionKind kind = PreconditionKind::EvidenceKnown;
  GuaranteeKind guarantee = GuaranteeKind::Availability;
  ScopeId scope{};
  std::uint64_t required_value = 0;
  std::uint64_t dwell_ticks = 0;

  friend bool operator==(const RestorationPrecondition& a, const RestorationPrecondition& b) noexcept;
  [[nodiscard]] std::string render() const;
};

/// Committed degraded authority for exactly one subject, scope and contract.
struct Grant {
  GrantId id{};
  DecisionId decision{};
  AuthorityBinding binding{};
  ServiceClass service_class = ServiceClass::Standard;
  bool protected_obligation = false;
  GuaranteeSet original{};
  GuaranteeSet degraded{};
  GuaranteeDelta delta{};
  ReasonCode reason = ReasonCode::None;
  EvidenceVector evidence{};
  EvidenceState evidence_state = EvidenceState::Unknown;
  std::vector<RestorationPrecondition> preconditions{};
  /// Monotonic per-authority attempt identity. Every acknowledgement and
  /// application report must carry the current value or it is rejected as a
  /// replay.
  SequenceNumber sequence{};
  AttemptId last_attempt{};
  Tick issued_tick{};
  Tick expires_tick{};
  Tick last_transition_tick{};
  GrantState state = GrantState::Issued;
  ProvenanceClass provenance = ProvenanceClass::Synthetic;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool live() const noexcept { return grant_state_live(state); }
  /// Current authority ladder position implied by the lifecycle state.
  [[nodiscard]] AuthorityLevel authority_level() const noexcept;
  [[nodiscard]] bool expired_at(Tick now) const noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
  [[nodiscard]] std::string explain() const;
};

enum class PreconditionResult : std::uint16_t {
  Satisfied = 1,
  NotSatisfied = 2,
  Indeterminate = 3,
  Unsupported = 4,
};

bool is_valid(PreconditionResult value) noexcept;
std::string_view to_string(PreconditionResult value) noexcept;

struct PreconditionEvaluation {
  RestorationPrecondition precondition{};
  PreconditionResult result = PreconditionResult::Indeterminate;
  ReasonCode reason = ReasonCode::None;

  [[nodiscard]] std::string render() const;
};

/// Result of asking whether a degraded grant may be restored to normal service.
/// Restoration is a transition that requires positive proof; UNKNOWN evidence
/// can never produce Proven.
struct RestorationEvaluation {
  RestorationOutcome outcome = RestorationOutcome::Invalid;
  GrantId grant{};
  DecisionId decision{};
  AuthorityBinding binding{};
  GuaranteeSet original{};
  GuaranteeSet degraded{};
  std::vector<PreconditionEvaluation> preconditions{};
  ReasonCode primary_reason = ReasonCode::None;
  EvidenceState evidence_state = EvidenceState::Unknown;
  ProvenanceClass provenance = ProvenanceClass::Synthetic;
  Tick evaluated_tick{};

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string explain() const;
};

}  // namespace dmf

#endif  // DMF_GRANT_HPP
