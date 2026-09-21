// Degraded Mode Fabric - property and differential suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic seeds, invariant checks after every operation, canonical-order
// independence, a slow independent reference solver for differential testing,
// and adversarial instances built specifically to break a greedy heuristic.
#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

ServiceClass class_for(std::uint32_t selector) {
  switch (selector % 4U) {
    case 0: return ServiceClass::Protected;
    case 1: return ServiceClass::Standard;
    case 2: return ServiceClass::BestEffort;
    default: return ServiceClass::Scavenger;
  }
}

std::vector<ServiceContract> random_contracts(Rng& rng, std::uint32_t count, std::uint64_t scope) {
  std::vector<ServiceContract> contracts;
  contracts.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const ServiceClass service_class = class_for(rng.u32(4));
    const std::uint64_t bandwidth = 500 + rng.below(20000);
    const std::uint64_t floor = (bandwidth / 4) + rng.below(bandwidth / 4 + 1);
    contracts.push_back(contract(i + 1, service_class, rng.u32(1000), bandwidth,
                                 500 + rng.below(4000), floor, 4, scope));
  }
  return contracts;
}

CapabilitySpec random_capability(Rng& rng, std::uint64_t scope) {
  CapabilitySpec spec;
  spec.bandwidth_kbps = 500 + rng.below(60000);
  spec.rtt_us = 100 + rng.below(5000);
  spec.path_diversity = 1 + rng.u32(8);
  spec.reachable_ppm = 900000 + rng.u32(100001);
  spec.node_coverage_ppm = 900000 + rng.u32(100001);
  spec.synchronous_durability = rng.coin();
  spec.scope = scope;
  return spec;
}

/// Checks every invariant the product claims, for one produced plan.
void assert_plan_invariants(const AllocationPlan& plan, const Policy& policy,
                            const std::vector<ServiceContract>& contracts,
                            const CapabilitySnapshot& snapshot) {
  std::uint64_t total = 0;
  for (const AllocationEntry& entry : plan.entries) {
    const auto found = std::find_if(contracts.begin(), contracts.end(),
                                    [&entry](const ServiceContract& candidate) {
                                      return candidate.id == entry.contract;
                                    });
    DMF_CHECK(found != contracts.end());
    if (found == contracts.end()) continue;
    if (!entry.admitted) {
      DMF_CHECK(entry.approved.empty());
      DMF_CHECK_EQ(entry.allocated_bandwidth_kbps, std::uint64_t{0});
      DMF_CHECK(!is_authorising(entry.outcome));
      continue;
    }
    // Property 1: degraded output never exceeds original authority.
    DMF_CHECK(never_exceeds(found->original, entry.approved));
    // Property 2: a rigid contract is either whole or refused.
    if (is_rigid(*found)) {
      DMF_CHECK(entry.delta.empty());
      DMF_CHECK(entry.approved == found->original);
    }
    // Property 3: the approved set is inside the policy envelope.
    auto envelope = policy.effective_envelope(*found);
    DMF_CHECK(envelope.ok());
    if (envelope.ok()) DMF_CHECK(envelope.value().permits(found->original, entry.approved));
    // Property 4: allocation is within demand, and above the floor unless zero.
    DMF_CHECK(entry.allocated_bandwidth_kbps <= found->bandwidth_demand());
    if (entry.allocated_bandwidth_kbps != 0 && found->bandwidth_demand() != 0) {
      DMF_CHECK(entry.allocated_bandwidth_kbps >= found->bandwidth_floor());
    }
    total += entry.allocated_bandwidth_kbps;
  }
  // Property 5: the plan never allocates more than the snapshot offers.
  DMF_CHECK(total <= snapshot.bandwidth_kbps);
  DMF_CHECK(total == plan.allocated_bandwidth_kbps);
  DMF_CHECK(plan.allocated_bandwidth_kbps <= plan.available_bandwidth_kbps);
  // Property 6: a claim of optimality is only made when the search finished.
  if (plan.status == PlanStatus::Optimal) DMF_CHECK(!plan.work.budget_exhausted);
  // Property 7: PROVEN_INFEASIBLE always carries a certificate.
  if (plan.status == PlanStatus::ProvenInfeasible) DMF_CHECK(plan.certificate.valid);
}

}  // namespace

DMF_TEST(property, invariants_hold_after_every_operation) {
  const Policy policy = demonstration_policy();
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    Rng rng(seed);
    const std::uint32_t count = 1 + rng.u32(8);
    const std::vector<ServiceContract> contracts = random_contracts(rng, count, 1);
    const CapabilitySpec spec = random_capability(rng, 1);
    const CapabilitySnapshot snapshot = capability(spec);
    std::vector<const ServiceContract*> pointers;
    pointers.reserve(contracts.size());
    for (const ServiceContract& entry : contracts) pointers.push_back(&entry);
    auto plan = allocate_direct(pointers, snapshot, policy, seed);
    DMF_CHECK(plan.ok());
    if (!plan.ok()) continue;
    assert_plan_invariants(plan.value(), policy, contracts, snapshot);
    // Property 8: every entry is accounted for exactly once.
    DMF_CHECK_EQ(plan.value().entries.size(), contracts.size());
  }
}

DMF_TEST(property, the_plan_is_independent_of_input_order) {
  const Policy policy = demonstration_policy();
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed);
    const std::uint32_t count = 2 + rng.u32(6);
    const std::vector<ServiceContract> contracts = random_contracts(rng, count, 1);
    const CapabilitySnapshot snapshot = capability(random_capability(rng, 1));
    std::vector<const ServiceContract*> pointers;
    for (const ServiceContract& entry : contracts) pointers.push_back(&entry);

    auto straight = allocate_direct(pointers, snapshot, policy, seed);
    DMF_CHECK(straight.ok());
    std::vector<const ServiceContract*> shuffled = pointers;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
      std::swap(shuffled[i - 1], shuffled[rng.below(i)]);
    }
    auto reversed = allocate_direct(shuffled, snapshot, policy, seed);
    DMF_CHECK(reversed.ok());
    if (!straight.ok() || !reversed.ok()) continue;
    DMF_CHECK_EQ(straight.value().digest(), reversed.value().digest());
  }
}

DMF_TEST(property, differential_against_the_reference_solver) {
  const Policy policy = demonstration_policy();
  std::uint64_t compared = 0;
  for (std::uint64_t seed = 1; seed <= 3000; ++seed) {
    Rng rng(seed);
    const std::uint32_t count = 1 + rng.u32(7);
    const std::vector<ServiceContract> contracts = random_contracts(rng, count, 1);
    const CapabilitySnapshot snapshot = capability(random_capability(rng, 1));
    std::vector<const ServiceContract*> pointers;
    for (const ServiceContract& entry : contracts) pointers.push_back(&entry);

    auto plan = allocate_direct(pointers, snapshot, policy, seed);
    DMF_CHECK(plan.ok());
    if (!plan.ok()) continue;
    assert_plan_invariants(plan.value(), policy, contracts, snapshot);

    AllocationRequest request;
    request.scope = scope_id(1);
    request.contracts = pointers;
    request.capability = &snapshot;
    request.policy = &policy;
    auto reference = reference_solve_rigid(request, 1U << 20);
    if (!reference.ok()) continue;  // the instance is too large to enumerate

    std::uint64_t served = 0;
    std::uint64_t priority = 0;
    for (const AllocationEntry& entry : plan.value().entries) {
      if (!entry.admitted || !entry.full) continue;
      const auto found = std::find_if(contracts.begin(), contracts.end(),
                                      [&entry](const ServiceContract& candidate) {
                                        return candidate.id == entry.contract;
                                      });
      if (found == contracts.end() || !is_rigid(*found)) continue;
      ++served;
      priority += found->priority;
    }
    // Only an OPTIMAL plan may claim optimality; when it does, it must match the
    // exhaustive optimum exactly. Otherwise it must not beat it.
    if (plan.value().status == PlanStatus::Optimal) {
      DMF_CHECK_EQ(served, reference.value().served_count);
      DMF_CHECK_EQ(priority, reference.value().priority_sum);
      ++compared;
    } else {
      DMF_CHECK(served <= reference.value().served_count);
      DMF_CHECK(priority <= reference.value().priority_sum);
    }
  }
  DMF_CHECK(compared > 500);
}

DMF_TEST(property, a_capacity_increase_never_weakens_an_approved_set) {
  const Policy policy = demonstration_policy();
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed);
    const ServiceContract entry = random_contracts(rng, 1, 1).front();
    CapabilitySpec spec = random_capability(rng, 1);
    const CapabilitySnapshot poor = capability(spec);
    const Decision poor_decision = evaluate_direct(entry, poor, policy, 1);
    DMF_CHECK(never_exceeds(entry.original, poor_decision.approved));

    CapabilitySpec richer = spec;
    richer.bandwidth_kbps = spec.bandwidth_kbps * 2 + 1000;
    richer.rtt_us = spec.rtt_us / 2;
    const CapabilitySnapshot good = capability(richer);
    const Decision good_decision = evaluate_direct(entry, good, policy, 2);
    DMF_CHECK(never_exceeds(entry.original, good_decision.approved));

    for (const Guarantee& guarantee : poor_decision.approved.items()) {
      const Guarantee* richer_guarantee = good_decision.approved.find(guarantee.kind);
      if (richer_guarantee == nullptr) continue;
      if (guarantee.comparator == Comparator::AtLeast) {
        DMF_CHECK(richer_guarantee->value >= guarantee.value);
      } else {
        DMF_CHECK(richer_guarantee->value <= guarantee.value);
      }
    }
  }
}

DMF_TEST(property, adversarial_instances_break_a_naive_greedy) {
  // A naive first-come allocation would serve the flexible contract first and
  // starve the protected one. The allocator must do the opposite.
  const Policy policy = demonstration_policy();
  const ServiceContract protected_contract =
      contract(1, ServiceClass::Protected, 5, 60000, 2000, 0, 0);
  const ServiceContract flexible_contract =
      contract(2, ServiceClass::Standard, 900, 59000, 2000, 1000, 4);
  std::vector<const ServiceContract*> contracts{&protected_contract, &flexible_contract};

  CapabilitySpec spec;
  spec.bandwidth_kbps = 60000;
  spec.rtt_us = 2000;
  const CapabilitySnapshot snapshot = capability(spec);

  auto plan = allocate_direct(contracts, snapshot, policy, 1);
  DMF_CHECK(plan.ok());
  const AllocationEntry* rigid = plan.value().find(contract_id(1));
  const AllocationEntry* flexible = plan.value().find(contract_id(2));
  DMF_CHECK(rigid != nullptr);
  DMF_CHECK(flexible != nullptr);
  if (rigid != nullptr) {
    DMF_CHECK(rigid->admitted);
    DMF_CHECK(rigid->full);
  }
  if (flexible != nullptr) {
    DMF_CHECK(!flexible->admitted || flexible->allocated_bandwidth_kbps == 0);
  }
  DMF_CHECK_EQ(plan.value().status, PlanStatus::Optimal);

  std::vector<ServiceContract> many;
  for (std::uint64_t i = 10; i < 20; ++i) {
    many.push_back(contract(i, ServiceClass::Scavenger, 1, 1000, 2000, 900, 4));
  }
  many.push_back(contract(1, ServiceClass::Protected, 5, 60000, 2000, 0, 0));
  std::vector<const ServiceContract*> many_pointers;
  for (const ServiceContract& entry : many) many_pointers.push_back(&entry);
  auto second = allocate_direct(many_pointers, snapshot, policy, 2);
  DMF_CHECK(second.ok());
  const AllocationEntry* winner = second.value().find(contract_id(1));
  DMF_CHECK(winner != nullptr);
  if (winner != nullptr) DMF_CHECK(winner->admitted);
}

DMF_TEST(property, a_bounded_search_never_claims_optimality) {
  Policy policy = demonstration_policy();
  policy.search_node_budget = 1;  // deliberately starved
  std::vector<ServiceContract> contracts;
  for (std::uint64_t i = 1; i <= 12; ++i) {
    contracts.push_back(contract(i, ServiceClass::Protected, static_cast<std::uint32_t>(100 - i),
                                 1000 + i * 7, 2000, 0, 0));
  }
  std::vector<const ServiceContract*> pointers;
  for (const ServiceContract& entry : contracts) pointers.push_back(&entry);
  CapabilitySpec spec;
  spec.bandwidth_kbps = 5000;
  spec.rtt_us = 2000;
  const CapabilitySnapshot snapshot = capability(spec);
  auto plan = allocate_direct(pointers, snapshot, policy, 1);
  DMF_CHECK(plan.ok());
  DMF_CHECK(plan.value().status != PlanStatus::Optimal);
  DMF_CHECK(plan.value().work.budget_exhausted);
  assert_plan_invariants(plan.value(), policy, contracts, snapshot);
}

DMF_TEST(property, a_proven_infeasible_certificate_is_arithmetically_sound) {
  Policy policy = demonstration_policy();
  policy.minimum_protected_served = 3;
  std::vector<ServiceContract> contracts;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    contracts.push_back(contract(i, ServiceClass::Protected, 10, 10000, 2000, 0, 0));
  }
  std::vector<const ServiceContract*> pointers;
  for (const ServiceContract& entry : contracts) pointers.push_back(&entry);
  CapabilitySpec spec;
  spec.bandwidth_kbps = 25000;
  spec.rtt_us = 2000;
  const CapabilitySnapshot snapshot = capability(spec);
  auto plan = allocate_direct(pointers, snapshot, policy, 1);
  DMF_CHECK(plan.ok());
  DMF_CHECK_EQ(plan.value().status, PlanStatus::ProvenInfeasible);
  DMF_CHECK(plan.value().certificate.valid);
  DMF_CHECK_EQ(plan.value().certificate.required_protected, std::uint32_t{3});
  DMF_CHECK_EQ(plan.value().certificate.minimum_demand_kbps, std::uint64_t{30000});
  DMF_CHECK_EQ(plan.value().certificate.offered_capacity_kbps, std::uint64_t{25000});

  policy.minimum_protected_served = 2;
  auto feasible = allocate_direct(pointers, snapshot, policy, 2);
  DMF_CHECK(feasible.ok());
  DMF_CHECK(feasible.value().status != PlanStatus::ProvenInfeasible);
}

DMF_TEST(property, the_allocator_work_counter_matches_the_plan_status) {
  const Policy policy = demonstration_policy();
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    Rng rng(seed);
    const std::uint32_t count = 1 + rng.u32(6);
    const std::vector<ServiceContract> contracts = random_contracts(rng, count, 1);
    const CapabilitySnapshot snapshot = capability(random_capability(rng, 1));
    std::vector<const ServiceContract*> pointers;
    for (const ServiceContract& entry : contracts) pointers.push_back(&entry);
    auto plan = allocate_direct(pointers, snapshot, policy, seed);
    DMF_CHECK(plan.ok());
    DMF_CHECK_EQ(plan.value().work.budget, policy.search_node_budget);
    DMF_CHECK_EQ(plan.value().work.closure(), plan.value().work.nodes_visited);
    DMF_CHECK(plan.value().work.nodes_pruned <= plan.value().work.nodes_visited);
    if (!plan.value().work.budget_exhausted) {
      DMF_CHECK_EQ(plan.value().status, PlanStatus::Optimal);
    }
  }
}
