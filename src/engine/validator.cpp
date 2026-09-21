// Degraded Mode Fabric - independent plan validation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The validator re-derives every obligation from the plan and the request. It
// never consults allocator state, so a defect in the allocator cannot hide
// behind a matching validator.
#include <algorithm>
#include <cstdio>

#include "dmf/engine.hpp"

namespace dmf {

namespace {

void report(ValidationReport& report, ContractId contract, ReasonCode reason,
            std::string detail) {
  ValidationIssue issue;
  issue.contract = contract;
  issue.reason = reason;
  issue.detail = std::move(detail);
  report.issues.push_back(std::move(issue));
  ++report.violations;
}

}  // namespace

std::string ValidationReport::render() const {
  std::string out = valid ? "plan VALID" : "plan INVALID";
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), " entries=%llu violations=%llu",
                static_cast<unsigned long long>(entries_checked),
                static_cast<unsigned long long>(violations));
  out.append(buffer);
  for (const ValidationIssue& issue : issues) {
    out.append("\n    ");
    out.append(issue.contract.to_string());
    out.append(" ");
    out.append(to_string(issue.reason));
    out.append(": ");
    out.append(issue.detail);
  }
  return out;
}

ValidationReport validate_plan(const AllocationPlan& plan, const AllocationRequest& request) {
  ValidationReport result;
  if (request.policy == nullptr || request.capability == nullptr) {
    report(result, ContractId{}, ReasonCode::ContractInvalid,
           "validation request is incomplete");
    return result;
  }
  const Policy& policy = *request.policy;
  const CapabilitySnapshot& capability = *request.capability;

  if (plan.scope != request.scope) {
    report(result, ContractId{}, ReasonCode::ScopeUnknown, "plan scope does not match the request");
  }
  if (plan.capacity_generation != capability.generation) {
    report(result, ContractId{}, ReasonCode::AuthorityGenerationChanged,
           "plan is bound to a different capacity generation");
  }
  if (plan.available_bandwidth_kbps != capability.bandwidth_kbps) {
    report(result, ContractId{}, ReasonCode::FabricCapabilityShortfall,
           "plan records a different offered capacity than the snapshot");
  }
  if (plan.status == PlanStatus::ProvenInfeasible && !plan.certificate.valid) {
    report(result, ContractId{}, ReasonCode::ResourceExhausted,
           "plan claims PROVEN_INFEASIBLE without a certificate");
  }
  if (plan.status == PlanStatus::Optimal && plan.work.budget_exhausted) {
    report(result, ContractId{}, ReasonCode::SearchLimitReached,
           "plan claims OPTIMAL after exhausting its search budget");
  }

  std::vector<const ServiceContract*> expected;
  expected.reserve(request.contracts.size());
  for (const ServiceContract* contract : request.contracts) {
    if (contract == nullptr) {
      report(result, ContractId{}, ReasonCode::ContractInvalid, "request carries a null contract");
      continue;
    }
    expected.push_back(contract);
  }
  std::sort(expected.begin(), expected.end(),
            [](const ServiceContract* a, const ServiceContract* b) { return a->id < b->id; });

  if (plan.entries.size() != expected.size()) {
    report(result, ContractId{}, ReasonCode::ContractInvalid,
           "plan does not account for every contract exactly once");
  }

  std::uint64_t total_allocated = 0;
  std::uint64_t previous_id = 0;
  bool first = true;
  for (const AllocationEntry& entry : plan.entries) {
    ++result.entries_checked;
    if (!first && entry.contract.value() <= previous_id) {
      report(result, entry.contract, ReasonCode::ContractInvalid,
             "plan entries are not in canonical identity order");
    }
    previous_id = entry.contract.value();
    first = false;

    const auto found = std::find_if(expected.begin(), expected.end(),
                                    [&entry](const ServiceContract* candidate) {
                                      return candidate->id == entry.contract;
                                    });
    if (found == expected.end()) {
      report(result, entry.contract, ReasonCode::ContractInvalid,
             "plan entry names a contract that was not requested");
      continue;
    }
    const ServiceContract& contract = **found;
    if (entry.contract_generation != contract.generation) {
      report(result, entry.contract, ReasonCode::ContractGenerationChanged,
             "plan entry is bound to a stale contract generation");
    }
    if (entry.scope != contract.scope) {
      report(result, entry.contract, ReasonCode::ScopeUnknown,
             "plan entry scope does not match the contract scope");
    }

    if (!entry.admitted) {
      if (!entry.approved.empty()) {
        report(result, entry.contract, ReasonCode::ContractInvalid,
               "refused entry still carries an approved guarantee set");
      }
      if (entry.allocated_bandwidth_kbps != 0) {
        report(result, entry.contract, ReasonCode::ContractInvalid,
               "refused entry still holds bandwidth");
      }
      if (is_authorising(entry.outcome)) {
        report(result, entry.contract, ReasonCode::ContractInvalid,
               "refused entry reports an authorising outcome");
      }
      continue;
    }

    // Product-defining invariant: approved never exceeds the original.
    const WeakeningResult relation = classify_weakening(contract.original, entry.approved);
    if (relation != WeakeningResult::WeakensOrEquals) {
      report(result, entry.contract, ReasonCode::PolicyEnvelopeExceeded,
             std::string("approved obligations are not a weakening: ") +
                 std::string(to_string(relation)));
    }
    auto delta = compute_delta(contract.original, entry.approved, ReasonCode::ConcessionWithinEnvelope);
    if (delta.ok()) {
      if (delta.value().digest() != entry.delta.digest()) {
        report(result, entry.contract, ReasonCode::ContractInvalid,
               "recorded concession delta does not match the approved set");
      }
    } else {
      report(result, entry.contract, ReasonCode::ContractInvalid,
             "concession delta could not be derived");
    }

    auto envelope = policy.effective_envelope(contract);
    if (!envelope.ok()) {
      report(result, entry.contract, ReasonCode::PolicyNoRule,
             "no policy envelope exists for an admitted contract");
    } else if (!envelope.value().permits(contract.original, entry.approved)) {
      report(result, entry.contract, ReasonCode::PolicyEnvelopeExceeded,
             "approved obligations exceed the policy envelope");
    }

    if (is_rigid(contract) && !entry.delta.empty()) {
      report(result, entry.contract, ReasonCode::ProtectedObligationUnmet,
             "rigid contract was served with a concession");
    }
    if (entry.admitted && entry.full != entry.delta.empty()) {
      report(result, entry.contract, ReasonCode::ContractInvalid,
             "full marker disagrees with the concession delta");
    }

    const std::uint64_t demand = contract.bandwidth_demand();
    const std::uint64_t floor = contract.bandwidth_floor();
    if (entry.allocated_bandwidth_kbps > demand) {
      report(result, entry.contract, ReasonCode::ContractInvalid,
             "allocated bandwidth exceeds the contractual demand");
    }
    if (entry.allocated_bandwidth_kbps != 0 && entry.allocated_bandwidth_kbps < floor) {
      report(result, entry.contract, ReasonCode::ResourceExhausted,
             "allocated bandwidth is below the policy floor");
    }
    const auto sum = checked_add(total_allocated, entry.allocated_bandwidth_kbps);
    if (!sum.has_value()) {
      report(result, entry.contract, ReasonCode::ArithmeticOverflow,
             "allocated bandwidth accounting overflowed");
      total_allocated = std::numeric_limits<std::uint64_t>::max();
    } else {
      total_allocated = *sum;
    }
  }

  if (total_allocated > plan.available_bandwidth_kbps) {
    report(result, ContractId{}, ReasonCode::ResourceExhausted,
           "plan allocates more bandwidth than the snapshot offers");
  }
  if (plan.allocated_bandwidth_kbps != total_allocated) {
    report(result, ContractId{}, ReasonCode::ArithmeticOverflow,
           "plan bandwidth total does not match the sum of its entries");
  }
  result.valid = result.violations == 0;
  return result;
}

}  // namespace dmf
