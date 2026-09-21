// Degraded Mode Fabric - slow independent reference solver.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Exhaustive enumeration of every rigid subset. This exists purely so the
// production allocator can be differential-tested against ground truth on small
// instances; it is never used on a live path and refuses to answer when the
// instance is too large to enumerate.
#include <algorithm>
#include <cmath>

#include "dmf/engine.hpp"

namespace dmf {

Result<ReferenceSolution> reference_solve_rigid(const AllocationRequest& request,
                                                std::uint64_t max_subsets) {
  ReferenceSolution solution;
  if (request.capability == nullptr || request.policy == nullptr) {
    return Status(ErrorCode::InvalidArgument, "reference solver needs capability and policy");
  }
  const CapabilitySnapshot& capability = *request.capability;

  std::vector<const ServiceContract*> candidates;
  for (const ServiceContract* contract : request.contracts) {
    if (contract == nullptr) continue;
    if (contract->scope != request.scope || !contract->active) continue;
    if (!is_rigid(*contract)) continue;
    if (!full_service_supported(*contract, capability)) continue;
    candidates.push_back(contract);
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const ServiceContract* a, const ServiceContract* b) { return a->id < b->id; });

  const std::size_t count = candidates.size();
  if (count >= 63) {
    return Status(ErrorCode::SearchLimitReached, "reference solver cannot enumerate this instance");
  }
  const std::uint64_t subsets = std::uint64_t{1} << count;
  if (subsets > max_subsets) {
    return Status(ErrorCode::SearchLimitReached, "reference solver subset budget exceeded");
  }

  std::uint64_t best_count = 0;
  std::uint64_t best_priority = 0;
  std::vector<ContractId> best_ids;
  for (std::uint64_t mask = 0; mask < subsets; ++mask) {
    ++solution.subsets_examined;
    std::uint64_t weight = 0;
    std::uint64_t served = 0;
    std::uint64_t priority = 0;
    std::vector<ContractId> ids;
    bool overflow = false;
    for (std::size_t bit = 0; bit < count; ++bit) {
      if ((mask & (std::uint64_t{1} << bit)) == 0) continue;
      const ServiceContract& contract = *candidates[bit];
      const auto next_weight = checked_add(weight, contract.bandwidth_demand());
      if (!next_weight.has_value()) {
        overflow = true;
        break;
      }
      weight = *next_weight;
      ++served;
      const auto next_priority = checked_add(priority, static_cast<std::uint64_t>(contract.priority));
      if (!next_priority.has_value()) {
        overflow = true;
        break;
      }
      priority = *next_priority;
      ids.push_back(contract.id);
    }
    if (overflow) continue;
    if (weight > capability.bandwidth_kbps) continue;
    const bool better = served > best_count ||
                        (served == best_count && priority > best_priority) ||
                        (served == best_count && priority == best_priority && ids < best_ids);
    if (better) {
      best_count = served;
      best_priority = priority;
      best_ids = ids;
    }
  }
  solution.served_count = best_count;
  solution.priority_sum = best_priority;
  solution.served = best_ids;
  solution.complete = true;
  return solution;
}

}  // namespace dmf
