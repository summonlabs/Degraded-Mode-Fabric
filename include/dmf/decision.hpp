// Degraded Mode Fabric - decisions and allocation plans.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A Decision is the runtime's externally visible answer for exactly one
// subject/scope. It binds the generations that made it legal, states the
// original and approved obligations, and explains every concession.
#ifndef DMF_DECISION_HPP
#define DMF_DECISION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/authority.hpp"
#include "dmf/contract.hpp"
#include "dmf/core.hpp"
#include "dmf/evidence.hpp"
#include "dmf/guarantee.hpp"
#include "dmf/ids.hpp"
#include "dmf/reason.hpp"

namespace dmf {

/// Support classification of a single original guarantee.
struct GuaranteeSupport {
  GuaranteeKind kind = GuaranteeKind::Availability;
  SupportTri support = SupportTri::Indeterminate;
  ReasonCode reason = ReasonCode::None;

  friend bool operator==(const GuaranteeSupport& a, const GuaranteeSupport& b) noexcept {
    return a.kind == b.kind && a.support == b.support && a.reason == b.reason;
  }
};

/// Bounded work counters for one allocator run. Published so a caller can tell
/// a proven optimum from an exhausted budget.
struct AllocationWork {
  std::uint64_t nodes_visited = 0;
  std::uint64_t nodes_pruned = 0;
  std::uint64_t budget = 0;
  bool budget_exhausted = false;

  /// nodes_pruned is a subset of nodes_visited: every pruned node was visited
  /// first, so the closure of the search is exactly nodes_visited.
  [[nodiscard]] std::uint64_t closure() const noexcept { return nodes_visited; }
};

/// One contract's outcome inside a scope allocation plan.
struct AllocationEntry {
  ContractId contract{};
  ContractGeneration contract_generation{};
  ScopeId scope{};
  bool admitted = false;   ///< the contract keeps some service
  bool full = false;       ///< served exactly at its original obligations
  GuaranteeSet approved{};
  GuaranteeDelta delta{};
  std::uint64_t allocated_bandwidth_kbps = 0;
  DecisionOutcome outcome = DecisionOutcome::Refused;
  ReasonCode reason = ReasonCode::None;

  friend bool operator==(const AllocationEntry& a, const AllocationEntry& b) noexcept;
  [[nodiscard]] std::string render() const;
};

/// Exact certificate supporting a PROVEN_INFEASIBLE plan status. Only emitted
/// when the arithmetic proves the policy minimum cannot be met, never merely
/// because a bounded search failed to find an allocation.
struct InfeasibilityCertificate {
  std::uint32_t required_protected = 0;
  std::uint32_t available_protected = 0;
  std::uint64_t minimum_demand_kbps = 0;
  std::uint64_t offered_capacity_kbps = 0;
  bool valid = false;

  [[nodiscard]] std::string render() const;
};

/// One scope's plan: which obligations remain supportable and at what level.
struct AllocationPlan {
  PlanId id{};
  AuthorityVector authority{};
  ScopeId scope{};
  CapacityGeneration capacity_generation{};
  PlanStatus status = PlanStatus::Invalid;
  std::vector<AllocationEntry> entries{};
  std::uint64_t available_bandwidth_kbps = 0;
  std::uint64_t allocated_bandwidth_kbps = 0;
  AllocationWork work{};
  InfeasibilityCertificate certificate{};
  Tick planned_tick{};
  bool protected_shortfall = false;

  [[nodiscard]] const AllocationEntry* find(ContractId contract) const noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
};

/// The externally visible decision for one contract.
struct Decision {
  DecisionId id{};
  AuthorityBinding binding{};
  ServiceClass service_class = ServiceClass::Standard;
  bool protected_obligation = false;
  std::uint32_t priority = 0;
  DecisionOutcome outcome = DecisionOutcome::Invalid;
  /// How far up the authority ladder this decision actually reaches. Never
  /// higher than Authorized for a freshly evaluated decision.
  AuthorityLevel max_authority = AuthorityLevel::None;
  GuaranteeSet original{};
  GuaranteeSet approved{};
  GuaranteeDelta delta{};
  ReasonCode primary_reason = ReasonCode::None;
  std::vector<ReasonCode> reasons{};
  std::vector<GuaranteeSupport> support{};
  EvidenceVector evidence{};
  EvidenceState evidence_state = EvidenceState::Unknown;
  PlanStatus plan_status = PlanStatus::Invalid;
  bool escalation_required = false;
  ProvenanceClass provenance = ProvenanceClass::Synthetic;
  Tick decided_tick{};
  Tick expires_tick{};

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  /// Deterministic, bounded, human-readable explanation. Never exceeds
  /// kMaxExplanationBytes and never grows with unbounded input.
  [[nodiscard]] std::string explain() const;
  [[nodiscard]] std::string reason_list() const;
};

/// A decision together with the scope allocation it came from. The plan is a
/// derived view: it lists every contending contract, so it is deliberately not
/// stored inside the decision record and is recomputed by the runtime on demand.
/// A decision read back from the durable record therefore carries its own
/// semantics only, never a stale plan.
struct EvaluationView : Decision {
  AllocationPlan plan{};
};

}  // namespace dmf

#endif  // DMF_DECISION_HPP
