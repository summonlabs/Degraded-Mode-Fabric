// Degraded Mode Fabric - degradation evaluation and allocation engine.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The engine answers one question per scope: given observed capability and the
// current policy, which obligations remain legally supportable, at what level,
// and which must be refused or escalated.
//
// Supported problem class for the allocator
// -----------------------------------------
// One scope, one capability snapshot, N contracts. Bandwidth is the only
// additive, contended fabric resource; every other guarantee is a support
// predicate evaluated against the snapshot. A contract is *rigid* when it is a
// protected obligation or carries a frozen envelope: it is either served at its
// full original obligations or explicitly refused. Flexible contracts absorb
// degradation.
//
// Objective, in strict lexicographic order:
//   1. maximise the number of rigid contracts served in full;
//   2. maximise the sum of their priorities;
//   3. serve flexible contracts in the documented deterministic order,
//      first-fit against remaining capacity, refusing any contract that cannot
//      reach its policy floor.
//
// The exact solver proves (1) and (2) optimal for the instance when it completes
// inside the policy node budget; otherwise the plan is reported as
// SEARCH_LIMIT_REACHED and optimality is never claimed. PROVEN_INFEASIBLE is
// emitted only with an arithmetic certificate, never because a bounded search
// gave up.
#ifndef DMF_ENGINE_HPP
#define DMF_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/authority.hpp"
#include "dmf/contract.hpp"
#include "dmf/decision.hpp"
#include "dmf/evidence.hpp"
#include "dmf/grant.hpp"
#include "dmf/policy.hpp"

namespace dmf {

/// Identities and temporal position supplied by the embedding runtime.
struct EvaluationContext {
  AuthorityBinding binding{};
  Tick now{};
  DecisionId decision_id{};
  PlanId plan_id{};
  ProvenanceClass provenance = ProvenanceClass::Synthetic;
};

struct ContractEvaluationRequest {
  const ServiceContract* contract = nullptr;
  const CapabilitySnapshot* capability = nullptr;
  const EvidenceVector* evidence = nullptr;
  const Policy* policy = nullptr;
  EvaluationContext context{};
  /// Allocation produced by the scope allocator. When null the evaluator runs a
  /// single-contract allocation itself.
  const AllocationEntry* allocation = nullptr;
  PlanStatus plan_status = PlanStatus::Invalid;
};

/// Evaluates exactly one contract. The returned Decision is the runtime's
/// externally visible answer for that subject and binds the generations that
/// made it legal.
Decision evaluate_contract(const ContractEvaluationRequest& request);

struct AllocationRequest {
  ScopeId scope{};
  std::vector<const ServiceContract*> contracts{};
  const CapabilitySnapshot* capability = nullptr;
  const EvidenceVector* evidence = nullptr;
  const Policy* policy = nullptr;
  EvaluationContext context{};
};

/// Allocates degraded service for one scope. Deterministic: the result depends
/// only on the values supplied, never on container or discovery order.
Result<AllocationPlan> allocate_scope(const AllocationRequest& request);

struct ValidationIssue {
  ContractId contract{};
  ReasonCode reason = ReasonCode::None;
  std::string detail{};
};

/// Independent re-derivation of a plan's obligations. Never consults the
/// allocator's intermediate state.
struct ValidationReport {
  bool valid = false;
  std::uint64_t entries_checked = 0;
  std::uint64_t violations = 0;
  std::vector<ValidationIssue> issues{};

  [[nodiscard]] std::string render() const;
};

ValidationReport validate_plan(const AllocationPlan& plan, const AllocationRequest& request);

struct ReferenceSolution {
  std::uint64_t served_count = 0;
  std::uint64_t priority_sum = 0;
  std::vector<ContractId> served{};
  bool complete = false;
  std::uint64_t subsets_examined = 0;
};

/// Slow, independent, exhaustive optimum over every rigid subset. Used to
/// differential-test the production allocator on small instances; it is never
/// used on a live path.
Result<ReferenceSolution> reference_solve_rigid(const AllocationRequest& request,
                                                std::uint64_t max_subsets = 1U << 20);

struct RestorationRequest {
  const Grant* grant = nullptr;
  const ServiceContract* contract = nullptr;
  const CapabilitySnapshot* capability = nullptr;
  const EvidenceVector* evidence = nullptr;
  const Policy* policy = nullptr;
  AuthorityVector current_authority{};
  bool fence_active = false;
  Tick now{};
  ProvenanceClass provenance = ProvenanceClass::Synthetic;
};

/// Decides whether a degraded grant may return to normal service. Positive
/// proof is required; UNKNOWN evidence can never yield Proven.
RestorationEvaluation evaluate_restoration(const RestorationRequest& request);

/// Preconditions derived from a contract and policy, recorded in the grant so
/// the restoration decision is checkable after a restart.
std::vector<RestorationPrecondition> make_preconditions(const ServiceContract& contract,
                                                        const Policy& policy);

/// Deterministic, total ordering used to decide which contract absorbs
/// degradation first.
bool allocation_precedes(const ServiceContract& a, const ServiceContract& b, bool protect_first) noexcept;

/// True when the contract cannot be weakened at all: a protected obligation or
/// a frozen envelope.
bool is_rigid(const ServiceContract& contract) noexcept;

/// True when the snapshot proves every original guarantee of the contract is
/// supportable. Indeterminate evidence yields false: absence of proof is not
/// proof of support.
bool full_service_supported(const ServiceContract& contract,
                            const CapabilitySnapshot& capability) noexcept;

}  // namespace dmf

#endif  // DMF_ENGINE_HPP
