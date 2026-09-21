// Degraded Mode Fabric - scope allocation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <vector>

#include "dmf/decision.hpp"
#include "dmf/engine.hpp"

namespace dmf {

bool is_rigid(const ServiceContract& contract) noexcept {
  return contract.protected_obligation() || contract.frozen();
}

bool allocation_precedes(const ServiceContract& a, const ServiceContract& b,
                         bool protect_first) noexcept {
  if (protect_first) {
    const std::uint8_t rank_a = class_rank(a.service_class);
    const std::uint8_t rank_b = class_rank(b.service_class);
    if (rank_a != rank_b) return rank_a < rank_b;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.id < b.id;
  }
  if (a.priority != b.priority) return a.priority > b.priority;
  const std::uint8_t rank_a = class_rank(a.service_class);
  const std::uint8_t rank_b = class_rank(b.service_class);
  if (rank_a != rank_b) return rank_a < rank_b;
  return a.id < b.id;
}

namespace {

/// Value used by the exact solver. BIG dominates every priority so the solver
/// maximises the served count first and the priority sum second.
struct RigidItem {
  const ServiceContract* contract = nullptr;
  std::uint64_t weight = 0;
  std::uint64_t value = 0;
};

struct Assessment {
  const ServiceContract* contract = nullptr;
  bool rigid = false;
  bool admissible = false;  ///< structurally able to serve something
  bool non_bandwidth_full = true;
  GuaranteeSet approved{};
  DecisionOutcome outcome = DecisionOutcome::Refused;
  ReasonCode reason = ReasonCode::None;
  bool escalation = false;
  bool needs_bandwidth = false;
  std::uint64_t demand = 0;
  std::uint64_t floor = 0;
  bool frozen = false;
};

std::uint64_t capability_value_for(const CapabilitySnapshot& capability, GuaranteeKind kind) noexcept {
  switch (kind) {
    case GuaranteeKind::Availability: return capability.reachable_ppm;
    case GuaranteeKind::Bandwidth: return capability.bandwidth_kbps;
    case GuaranteeKind::LatencyP99: return capability.rtt_p99_us;
    case GuaranteeKind::PathDiversity: return capability.path_diversity;
    case GuaranteeKind::Durability:
      return capability.synchronous_durability ? kMaxDurabilityLevel : 0ULL;
    case GuaranteeKind::Reachability: return capability.node_coverage_ppm;
  }
  return 0;
}

ReasonCode evidence_reason(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Unknown: return ReasonCode::EvidenceUnknown;
    case EvidenceState::Stale: return ReasonCode::EvidenceStale;
    case EvidenceState::Conflict: return ReasonCode::EvidenceConflict;
    case EvidenceState::Invalid: return ReasonCode::EvidenceInvalid;
    case EvidenceState::Unsupported: return ReasonCode::EvidenceUnsupported;
    case EvidenceState::Known: return ReasonCode::None;
  }
  return ReasonCode::EvidenceUnknown;
}

void apply_evidence_action(const Policy& policy, EvidenceState state, Assessment& assessment) {
  assessment.reason = evidence_reason(state);
  switch (policy.evidence_action(state)) {
    case DegradeAction::Protect:
    case DegradeAction::Refuse:
      assessment.outcome = DecisionOutcome::Refused;
      assessment.admissible = false;
      break;
    case DegradeAction::Escalate:
      assessment.outcome = DecisionOutcome::Escalated;
      assessment.admissible = false;
      assessment.escalation = true;
      break;
    case DegradeAction::Degrade:
      // Degrading on unproven capability would invent a floor the runtime cannot
      // justify, so the honest answer is INDETERMINATE plus escalation.
      assessment.outcome = DecisionOutcome::Indeterminate;
      assessment.admissible = false;
      assessment.escalation = true;
      break;
  }
}

/// Resolves every non-bandwidth guarantee, choosing the strongest value the
/// snapshot supports inside the policy envelope.
Assessment assess(const ServiceContract& contract, const CapabilitySnapshot& capability,
                  const Policy& policy) {
  Assessment assessment;
  assessment.contract = &contract;
  assessment.rigid = is_rigid(contract);
  assessment.frozen = contract.frozen();
  assessment.demand = contract.bandwidth_demand();
  assessment.floor = contract.bandwidth_floor();
  assessment.needs_bandwidth = assessment.demand > 0;

  if (!capability.usable()) {
    apply_evidence_action(policy, capability.state, assessment);
    return assessment;
  }

  auto envelope = policy.effective_envelope(contract);
  if (!envelope.ok()) {
    assessment.outcome = DecisionOutcome::Unsupported;
    assessment.reason = envelope.status().code() == ErrorCode::NotFound
                            ? ReasonCode::NoRuleForServiceClass
                            : ReasonCode::PolicyNoRule;
    assessment.escalation = true;
    return assessment;
  }
  const GuaranteeEnvelope& limits = envelope.value();

  // Pass 1 classifies every guarantee. Nothing is weakened here: an obligation
  // that cannot be met either becomes a shortfall for a degradable contract or
  // is reported as unmet for a rigid one.
  std::vector<GuaranteeKind> shortfalls;
  for (const Guarantee& guarantee : contract.original.items()) {
    const SupportTri tri = supports(capability, guarantee);
    if (tri == SupportTri::Supported) {
      const Status status = assessment.approved.insert(guarantee);
      if (!status.ok()) {
        assessment.outcome = DecisionOutcome::Invalid;
        assessment.reason = ReasonCode::ContractInvalid;
        return assessment;
      }
      continue;
    }
    if (tri == SupportTri::Invalid || tri == SupportTri::NotModelled) {
      assessment.outcome = DecisionOutcome::Indeterminate;
      assessment.reason = tri == SupportTri::Invalid ? ReasonCode::EvidenceInvalid
                                                     : ReasonCode::EvidenceUnsupported;
      assessment.escalation = true;
      return assessment;
    }
    // Capability was usable, so an Indeterminate here means the model itself
    // cannot speak. Fail closed.
    if (tri == SupportTri::Indeterminate) {
      assessment.outcome = DecisionOutcome::Indeterminate;
      assessment.reason = ReasonCode::EvidenceUnknown;
      assessment.escalation = true;
      return assessment;
    }
    shortfalls.push_back(guarantee.kind);
  }

  if (shortfalls.empty()) {
    assessment.non_bandwidth_full = true;
    assessment.admissible = true;
    assessment.outcome = DecisionOutcome::Full;
    assessment.reason = ReasonCode::OriginalObligationsSupportable;
    return assessment;
  }

  // A rigid contract is never weakened, so its shortfall is answered directly:
  // no envelope arithmetic, no partial service, no silent concession.
  if (assessment.rigid) {
    assessment.admissible = false;
    assessment.approved = GuaranteeSet{};
    const PolicyRule* rule =
        policy.select_rule(contract.service_class, contract.scope, shortfalls.front());
    if (rule != nullptr && rule->action == DegradeAction::Escalate) {
      assessment.outcome = DecisionOutcome::Escalated;
      assessment.escalation = true;
      assessment.reason = ReasonCode::PolicyRequiresProtection;
    } else {
      assessment.outcome = DecisionOutcome::Refused;
      assessment.reason = ReasonCode::ProtectedObligationUnmet;
    }
    return assessment;
  }

  const PolicyRule* rule = policy.select_rule(contract.service_class, contract.scope, shortfalls.front());
  if (rule == nullptr) {
    assessment.approved = GuaranteeSet{};
    assessment.outcome = DecisionOutcome::Unsupported;
    assessment.reason = ReasonCode::PolicyNoRule;
    assessment.escalation = true;
    return assessment;
  }
  switch (rule->action) {
    case DegradeAction::Refuse:
      assessment.approved = GuaranteeSet{};
      assessment.outcome = DecisionOutcome::Refused;
      assessment.reason = shortfall_reason(capability, *contract.original.find(shortfalls.front()));
      return assessment;
    case DegradeAction::Escalate:
      assessment.approved = GuaranteeSet{};
      assessment.outcome = DecisionOutcome::Escalated;
      assessment.escalation = true;
      assessment.reason = ReasonCode::ProtectedObligationUnmet;
      return assessment;
    case DegradeAction::Protect:
      assessment.approved = GuaranteeSet{};
      assessment.outcome = DecisionOutcome::Refused;
      assessment.reason = ReasonCode::PolicyRequiresProtection;
      return assessment;
    case DegradeAction::Degrade:
      break;
  }

  // Pass 2 weakens each shortfall to the strongest value the envelope allows.
  assessment.non_bandwidth_full = true;
  for (const GuaranteeKind kind : shortfalls) {
    if (kind != GuaranteeKind::Bandwidth) assessment.non_bandwidth_full = false;
    const Guarantee* guarantee = contract.original.find(kind);
    if (guarantee == nullptr) {
      assessment.outcome = DecisionOutcome::Invalid;
      assessment.reason = ReasonCode::ContractInvalid;
      return assessment;
    }
    if (kind == GuaranteeKind::Bandwidth) {
      // The allocator decides the contended allocation. The envelope floor is
      // recorded here as a placeholder so the envelope check below evaluates the
      // whole set, and the allocator replaces it with the real allocation.
      const std::uint64_t placeholder = limits.floor_for(kind, guarantee->value);
      const Status status = assessment.approved.upsert(kind, placeholder, guarantee->hard);
      if (!status.ok()) {
        assessment.outcome = DecisionOutcome::Invalid;
        assessment.reason = ReasonCode::ContractInvalid;
        return assessment;
      }
      continue;
    }
    const std::uint64_t bound = limits.floor_for(kind, guarantee->value);
    const std::uint64_t observed = capability_value_for(capability, kind);
    std::uint64_t approved_value = 0;
    if (guarantee->comparator == Comparator::AtLeast) {
      if (observed < bound) {
        assessment.approved = GuaranteeSet{};
        assessment.outcome = DecisionOutcome::Unsupported;
        assessment.reason = ReasonCode::PolicyEnvelopeExceeded;
        assessment.escalation = true;
        return assessment;
      }
      approved_value = std::min(guarantee->value, observed);
    } else {
      if (observed > bound) {
        assessment.approved = GuaranteeSet{};
        assessment.outcome = DecisionOutcome::Unsupported;
        assessment.reason = ReasonCode::PolicyEnvelopeExceeded;
        assessment.escalation = true;
        return assessment;
      }
      approved_value = std::max(guarantee->value, observed);
    }
    const Status status = assessment.approved.upsert(kind, approved_value, guarantee->hard);
    if (!status.ok()) {
      assessment.outcome = DecisionOutcome::Invalid;
      assessment.reason = ReasonCode::ContractInvalid;
      return assessment;
    }
  }

  if (!limits.permits(contract.original, assessment.approved)) {
    assessment.approved = GuaranteeSet{};
    assessment.outcome = DecisionOutcome::Unsupported;
    assessment.reason = ReasonCode::PolicyEnvelopeExceeded;
    assessment.escalation = true;
    return assessment;
  }
  assessment.admissible = true;
  assessment.outcome = DecisionOutcome::Degraded;
  assessment.reason = shortfall_reason(capability, *contract.original.find(shortfalls.front()));
  return assessment;
}

/// Exact branch and bound over rigid contracts.
///
/// Items are ordered once by descending density so the fractional-knapsack
/// relaxation of any suffix is a valid upper bound. The incumbent is always a
/// feasible selection, so aborting on the node budget still yields a legal plan.
struct RigidSearch {
  std::vector<RigidItem> items{};
  std::uint64_t capacity = 0;
  std::uint64_t budget = 0;
  std::uint64_t nodes = 0;
  std::uint64_t pruned = 0;
  bool exhausted = false;
  std::uint64_t best_value = 0;
  std::uint64_t best_count = 0;
  std::uint64_t best_priority = 0;
  std::vector<std::uint32_t> best_selection{};
  std::vector<std::uint32_t> current{};

  [[nodiscard]] static std::uint64_t saturating(std::optional<std::uint64_t> value) noexcept {
    return value.has_value() ? *value : std::numeric_limits<std::uint64_t>::max();
  }

  void consider(std::uint64_t count, std::uint64_t priority, std::uint64_t value) {
    const bool better_count = count > best_count;
    const bool better_priority = count == best_count && priority > best_priority;
    const bool better_tie =
        count == best_count && priority == best_priority && current < best_selection;
    if (!better_count && !better_priority && !better_tie) return;
    best_count = count;
    best_priority = priority;
    best_value = value;
    best_selection = current;
  }

  /// Fractional (LP) upper bound on the value obtainable from items[index..).
  [[nodiscard]] std::uint64_t bound(std::uint64_t index, std::uint64_t remaining) const noexcept {
    std::uint64_t total = 0;
    for (std::uint64_t i = index; i < items.size(); ++i) {
      const RigidItem& item = items[static_cast<std::size_t>(i)];
      if (item.weight <= remaining) {
        remaining -= item.weight;
        const auto sum = checked_add(total, item.value);
        total = sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
      } else {
        if (item.weight == 0) continue;
        const auto scaled = checked_mul(item.value, remaining);
        if (!scaled.has_value()) return std::numeric_limits<std::uint64_t>::max();
        const auto share = *scaled / item.weight;
        const auto sum = checked_add(total, share);
        total = sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
        break;
      }
    }
    return total;
  }

  void run(std::uint64_t index, std::uint64_t remaining, std::uint64_t count,
           std::uint64_t priority, std::uint64_t value) {
    if (exhausted) return;
    if (nodes >= budget) {
      exhausted = true;
      return;
    }
    ++nodes;
    if (index == items.size()) {
      consider(count, priority, value);
      return;
    }
    if (count + (items.size() - index) < best_count) {
      ++pruned;
      return;
    }
    // Prune only on a strict bound deficit: a tie can still win on the canonical
    // tie-break, so equality must be explored.
    if (saturating(checked_add(value, bound(index, remaining))) < best_value) {
      ++pruned;
      return;
    }
    const RigidItem& item = items[static_cast<std::size_t>(index)];
    if (item.weight <= remaining) {
      current.push_back(static_cast<std::uint32_t>(index));
      const auto next_remaining = checked_sub(remaining, item.weight);
      const auto next_count = checked_add(count, std::uint64_t{1});
      const auto next_priority = checked_add(priority,
                                             static_cast<std::uint64_t>(item.contract->priority));
      const auto next_value = checked_add(value, item.value);
      if (next_remaining.has_value() && next_count.has_value() && next_priority.has_value() &&
          next_value.has_value()) {
        run(index + 1, *next_remaining, *next_count, *next_priority, *next_value);
      } else {
        exhausted = true;
      }
      current.pop_back();
    } else {
      ++pruned;
    }
    run(index + 1, remaining, count, priority, value);
  }
};

}  // namespace

Result<AllocationPlan> allocate_scope(const AllocationRequest& request) {
  AllocationPlan plan;
  plan.id = request.context.plan_id;
  plan.authority = request.context.binding.vector;
  plan.scope = request.scope;
  plan.planned_tick = request.context.now;

  if (!request.scope.valid() || request.capability == nullptr || request.policy == nullptr) {
    plan.status = PlanStatus::Invalid;
    return Status(ErrorCode::InvalidArgument, "allocation request is incomplete");
  }
  if (request.capability->scope != request.scope) {
    plan.status = PlanStatus::Invalid;
    return Status(ErrorCode::InvalidArgument, "capability snapshot is for a different scope");
  }
  Status policy_status = request.policy->validate();
  if (!policy_status.ok()) {
    plan.status = PlanStatus::Invalid;
    return policy_status;
  }
  plan.capacity_generation = request.capability->generation;
  plan.available_bandwidth_kbps = request.capability->bandwidth_kbps;
  plan.work.budget = request.policy->search_node_budget;

  // Canonical input order: sorts by identity so the plan never depends on the
  // caller's container order.
  std::vector<const ServiceContract*> contracts = request.contracts;
  std::sort(contracts.begin(), contracts.end(),
            [](const ServiceContract* a, const ServiceContract* b) { return a->id < b->id; });
  for (std::size_t i = 1; i < contracts.size(); ++i) {
    if (contracts[i - 1]->id == contracts[i]->id) {
      plan.status = PlanStatus::Invalid;
      return Status(ErrorCode::AlreadyExists, "allocation request repeats a contract identity");
    }
  }

  std::vector<Assessment> assessments;
  assessments.reserve(contracts.size());
  for (const ServiceContract* contract : contracts) {
    if (contract->scope != request.scope || !contract->active) {
      AllocationEntry entry;
      entry.contract = contract->id;
      entry.contract_generation = contract->generation;
      entry.scope = request.scope;
      entry.admitted = false;
      entry.outcome = DecisionOutcome::Refused;
      entry.reason = contract->scope != request.scope ? ReasonCode::ScopeUnknown : ReasonCode::None;
      plan.entries.push_back(entry);
      Assessment inactive;
      inactive.contract = contract;
      inactive.admissible = false;
      inactive.outcome = entry.outcome;
      inactive.reason = entry.reason;
      assessments.push_back(inactive);
      continue;
    }
    Status status = contract->validate();
    if (!status.ok()) {
      plan.status = PlanStatus::Invalid;
      return status;
    }
    assessments.push_back(assess(*contract, *request.capability, *request.policy));
  }

  std::vector<std::size_t> rigid_indices;
  std::vector<std::size_t> flexible_indices;
  for (std::size_t i = 0; i < assessments.size(); ++i) {
    if (!assessments[i].admissible) continue;
    if (assessments[i].rigid) {
      rigid_indices.push_back(i);
    } else {
      flexible_indices.push_back(i);
    }
  }

  // ---- infeasibility certificate -----------------------------------------
  const std::uint32_t required = request.policy->minimum_protected_served;
  if (required > 0) {
    std::vector<std::uint64_t> demands;
    demands.reserve(rigid_indices.size());
    for (const std::size_t index : rigid_indices) {
      demands.push_back(assessments[index].demand);
    }
    std::sort(demands.begin(), demands.end());
    InfeasibilityCertificate certificate;
    certificate.required_protected = required;
    certificate.available_protected = static_cast<std::uint32_t>(demands.size());
    certificate.offered_capacity_kbps = request.capability->bandwidth_kbps;
    bool impossible = demands.size() < static_cast<std::size_t>(required);
    std::uint64_t minimum_demand = 0;
    if (!impossible) {
      for (std::uint32_t i = 0; i < required; ++i) {
        const auto sum = checked_add(minimum_demand, demands[i]);
        if (!sum.has_value()) {
          impossible = true;
          break;
        }
        minimum_demand = *sum;
      }
      if (!impossible && minimum_demand > request.capability->bandwidth_kbps) impossible = true;
    }
    if (impossible) {
      certificate.minimum_demand_kbps = minimum_demand;
      certificate.valid = true;
      plan.certificate = certificate;
      plan.status = PlanStatus::ProvenInfeasible;
      plan.protected_shortfall = true;
      for (const ServiceContract* contract : contracts) {
        AllocationEntry entry;
        entry.contract = contract->id;
        entry.contract_generation = contract->generation;
        entry.scope = request.scope;
        entry.admitted = false;
        entry.full = false;
        entry.outcome = DecisionOutcome::Escalated;
        entry.reason = ReasonCode::ResourceExhausted;
        entry.approved = GuaranteeSet{};
        plan.entries.push_back(entry);
      }
      std::sort(plan.entries.begin(), plan.entries.end(),
                [](const AllocationEntry& a, const AllocationEntry& b) { return a.contract < b.contract; });
      return plan;
    }
  }

  // ---- exact rigid selection ---------------------------------------------
  std::uint64_t priority_total = 0;
  for (const std::size_t index : rigid_indices) {
    const auto sum = checked_add(priority_total,
                                 static_cast<std::uint64_t>(assessments[index].contract->priority));
    if (!sum.has_value()) {
      plan.status = PlanStatus::Invalid;
      return Status(ErrorCode::Overflow, "priority sum overflowed");
    }
    priority_total = *sum;
  }
  const auto big_base = checked_add(priority_total, std::uint64_t{1});
  if (!big_base.has_value()) {
    plan.status = PlanStatus::Invalid;
    return Status(ErrorCode::Overflow, "priority scale overflowed");
  }
  const std::uint64_t big = *big_base;

  RigidSearch search;
  search.capacity = request.capability->bandwidth_kbps;
  search.budget = request.policy->search_node_budget;
  search.items.reserve(rigid_indices.size());
  for (const std::size_t index : rigid_indices) {
    RigidItem item;
    item.contract = assessments[index].contract;
    item.weight = assessments[index].demand;
    const auto value = checked_add(big, static_cast<std::uint64_t>(item.contract->priority));
    if (!value.has_value()) {
      plan.status = PlanStatus::Invalid;
      return Status(ErrorCode::Overflow, "rigid item value overflowed");
    }
    item.value = *value;
    search.items.push_back(item);
  }
  std::sort(search.items.begin(), search.items.end(), [](const RigidItem& a, const RigidItem& b) {
    // Descending density v/w, exact integer comparison, deterministic tie-break.
    const auto lhs = checked_mul(a.value, b.weight);
    const auto rhs = checked_mul(b.value, a.weight);
    if (lhs.has_value() && rhs.has_value() && *lhs != *rhs) return *lhs > *rhs;
    if (a.weight != b.weight) return a.weight < b.weight;
    if (a.value != b.value) return a.value > b.value;
    return a.contract->id < b.contract->id;
  });

  search.run(0, search.capacity, 0, 0, 0);
  plan.work.nodes_visited = search.nodes;
  plan.work.nodes_pruned = search.pruned;
  plan.work.budget_exhausted = search.exhausted;

  std::vector<bool> served(assessments.size(), false);
  std::uint64_t remaining = request.capability->bandwidth_kbps;
  for (const std::uint32_t index : search.best_selection) {
    const RigidItem& item = search.items[index];
    for (std::size_t i = 0; i < assessments.size(); ++i) {
      if (assessments[i].contract == item.contract) {
        served[i] = true;
        const auto rest = checked_sub(remaining, item.weight);
        if (!rest.has_value()) {
          plan.status = PlanStatus::Invalid;
          return Status(ErrorCode::Underflow, "rigid allocation exceeded capacity");
        }
        remaining = *rest;
        break;
      }
    }
  }

  // ---- flexible allocation ------------------------------------------------
  plan.status = search.exhausted ? PlanStatus::SearchLimitReached : PlanStatus::Optimal;
  if (rigid_indices.empty()) plan.status = PlanStatus::Optimal;

  std::vector<std::size_t> flexible = flexible_indices;
  std::sort(flexible.begin(), flexible.end(), [&assessments, &request](std::size_t a, std::size_t b) {
    return allocation_precedes(*assessments[a].contract, *assessments[b].contract,
                               request.policy->protect_first);
  });

  std::vector<GuaranteeSet> approved_sets(assessments.size());
  std::vector<std::uint64_t> allocations(assessments.size(), 0);
  std::vector<bool> admitted(assessments.size(), false);
  std::vector<DecisionOutcome> outcomes(assessments.size());
  std::vector<ReasonCode> reasons(assessments.size());
  for (std::size_t i = 0; i < assessments.size(); ++i) {
    outcomes[i] = assessments[i].outcome;
    reasons[i] = assessments[i].reason;
    approved_sets[i] = assessments[i].approved;
  }

  for (const std::size_t index : rigid_indices) {
    Assessment& assessment = assessments[index];
    if (!served[index]) {
      assessment.admissible = false;
      if (assessment.outcome == DecisionOutcome::Full ||
          assessment.outcome == DecisionOutcome::Degraded) {
        assessment.outcome = DecisionOutcome::Refused;
        assessment.reason = assessment.demand > remaining ? ReasonCode::ResourceExhausted
                                                          : ReasonCode::ProtectedObligationUnmet;
      }
      outcomes[index] = assessment.outcome;
      reasons[index] = assessment.reason;
      continue;
    }
    admitted[index] = true;
    allocations[index] = assessment.demand;
    outcomes[index] = DecisionOutcome::Full;
    reasons[index] = ReasonCode::OriginalObligationsSupportable;
  }
  // A rigid contract that is not admitted is an unmet protected obligation and
  // must be visible as such in the plan.
  plan.protected_shortfall = false;
  for (const std::size_t index : rigid_indices) {
    if (!admitted[index]) plan.protected_shortfall = true;
  }

  for (const std::size_t index : flexible) {
    Assessment& assessment = assessments[index];
    assessment.admissible = true;
    std::uint64_t allocated = 0;
    if (assessment.needs_bandwidth) {
      allocated = std::min(assessment.demand, remaining);
      if (allocated < assessment.floor) {
        assessment.admissible = false;
        assessment.outcome = DecisionOutcome::Refused;
        assessment.reason = ReasonCode::ResourceExhausted;
        outcomes[index] = assessment.outcome;
        reasons[index] = assessment.reason;
        continue;
      }
    }
    GuaranteeSet approved = assessment.approved;
    if (assessment.needs_bandwidth) {
      Status status = approved.upsert(GuaranteeKind::Bandwidth, allocated);
      if (!status.ok()) {
        plan.status = PlanStatus::Invalid;
        return status;
      }
    }
    auto envelope = request.policy->effective_envelope(*assessment.contract);
    if (!envelope.ok() || !envelope.value().permits(assessment.contract->original, approved)) {
      assessment.admissible = false;
      assessment.outcome = DecisionOutcome::Refused;
      assessment.reason = ReasonCode::PolicyEnvelopeExceeded;
      outcomes[index] = assessment.outcome;
      reasons[index] = assessment.reason;
      continue;
    }
    const auto rest = checked_sub(remaining, allocated);
    if (!rest.has_value()) {
      plan.status = PlanStatus::Invalid;
      return Status(ErrorCode::Underflow, "flexible allocation exceeded capacity");
    }
    remaining = *rest;
    admitted[index] = true;
    allocations[index] = allocated;
    approved_sets[index] = approved;
    outcomes[index] = (approved == assessment.contract->original) ? DecisionOutcome::Full
                                                                  : DecisionOutcome::Degraded;
    reasons[index] = (outcomes[index] == DecisionOutcome::Full)
                         ? ReasonCode::OriginalObligationsSupportable
                         : ReasonCode::ConcessionWithinEnvelope;
  }

  std::uint64_t total_allocated = 0;
  for (std::size_t i = 0; i < assessments.size(); ++i) {
    AllocationEntry entry;
    entry.contract = assessments[i].contract->id;
    entry.contract_generation = assessments[i].contract->generation;
    entry.scope = request.scope;
    entry.admitted = admitted[i];
    entry.allocated_bandwidth_kbps = allocations[i];
    if (admitted[i]) {
      entry.approved = approved_sets[i];
      auto delta = compute_delta(assessments[i].contract->original, entry.approved,
                                 ReasonCode::ConcessionWithinEnvelope);
      if (!delta.ok()) {
        plan.status = PlanStatus::Invalid;
        return delta.status();
      }
      entry.delta = delta.value();
      entry.full = entry.delta.empty();
      entry.outcome = entry.full ? DecisionOutcome::Full : DecisionOutcome::Degraded;
      if (entry.full) {
        entry.reason = ReasonCode::OriginalObligationsSupportable;
      } else if (reasons[i] == ReasonCode::OriginalObligationsSupportable ||
                 reasons[i] == ReasonCode::None) {
        entry.reason = ReasonCode::FabricCapabilityShortfall;
      } else {
        entry.reason = reasons[i];
      }
    } else {
      entry.full = false;
      entry.outcome = outcomes[i];
      entry.reason = reasons[i];
      if (entry.outcome == DecisionOutcome::Full ||
          entry.outcome == DecisionOutcome::Degraded) {
        entry.outcome = (entry.outcome == DecisionOutcome::Degraded &&
                         assessments[i].contract->protected_obligation())
                            ? DecisionOutcome::Refused
                            : entry.outcome;
      }
    }
    const auto sum = checked_add(total_allocated, allocations[i]);
    if (!sum.has_value()) {
      plan.status = PlanStatus::Invalid;
      return Status(ErrorCode::Overflow, "allocated bandwidth overflowed");
    }
    total_allocated = *sum;
    plan.entries.push_back(entry);
  }
  plan.allocated_bandwidth_kbps = total_allocated;
  if (plan.allocated_bandwidth_kbps > plan.available_bandwidth_kbps) {
    plan.status = PlanStatus::Invalid;
    return Status(ErrorCode::Overflow, "plan allocates more bandwidth than is available");
  }
  std::sort(plan.entries.begin(), plan.entries.end(),
            [](const AllocationEntry& a, const AllocationEntry& b) { return a.contract < b.contract; });
  return plan;
}

}  // namespace dmf
