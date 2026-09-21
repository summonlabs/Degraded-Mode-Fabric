// Degraded Mode Fabric - coordinator integration suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

struct Running {
  TempDir directory{"integration"};
  std::unique_ptr<Coordinator> coordinator{};

  Status start(std::uint64_t freshness = 1000, std::uint32_t minimum_protected = 0) {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    config.origin = OriginClass::Synthetic;
    auto created = Coordinator::create(config);
    if (!created.ok()) return created.status();
    coordinator = std::move(created.value());
    if (!coordinator->install_policy(demonstration_policy(PolicyGeneration::from_value(1), freshness,
                                                          minimum_protected))
             .ok()) {
      return Status(ErrorCode::InvalidState, "policy installation failed");
    }
    return Status{};
  }

  ~Running() {
    if (coordinator) {
      const Status status = coordinator->stop();
      (void)status;
    }
  }
};

}  // namespace

DMF_TEST(integration, weaker_traffic_absorbs_degradation_before_protected_traffic) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Protected, 100, 40000, 2000, 0, 0)));
  DMF_CHECK_OK(coordinator.register_contract(contract(2, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  DMF_CHECK_OK(coordinator.register_contract(contract(3, ServiceClass::Scavenger, 1, 2000, 2000, 500, 3)));

  CapabilitySpec spec;
  spec.bandwidth_kbps = 42000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));

  auto protected_decision = evaluate_view(coordinator, contract_id(1));
  DMF_CHECK(protected_decision.ok());
  DMF_CHECK_EQ(protected_decision.value().outcome, DecisionOutcome::Full);
  auto standard_decision = evaluate_view(coordinator, contract_id(2));
  DMF_CHECK(standard_decision.ok());
  DMF_CHECK_EQ(standard_decision.value().outcome, DecisionOutcome::Degraded);
  auto scavenger_decision = evaluate_view(coordinator, contract_id(3));
  DMF_CHECK(scavenger_decision.ok());
  DMF_CHECK_EQ(scavenger_decision.value().outcome, DecisionOutcome::Refused);

  // The plan shows all three contending for the same scope.
  DMF_CHECK_EQ(standard_decision.value().plan.entries.size(), std::size_t{3});
  const AllocationPlan& plan = standard_decision.value().plan;
  const AllocationEntry* protected_entry = plan.find(contract_id(1));
  DMF_CHECK(protected_entry != nullptr);
  if (protected_entry != nullptr) {
    DMF_CHECK(protected_entry->admitted);
    DMF_CHECK(protected_entry->full);
    DMF_CHECK(protected_entry->delta.empty());
  }
  const AllocationEntry* standard_entry = plan.find(contract_id(2));
  DMF_CHECK(standard_entry != nullptr);
  if (standard_entry != nullptr) {
    DMF_CHECK(standard_entry->admitted);
    DMF_CHECK_EQ(standard_entry->allocated_bandwidth_kbps, std::uint64_t{2000});
    DMF_CHECK(never_exceeds(contract(2, ServiceClass::Standard, 10, 4000, 2000, 1000, 3).original,
                            standard_entry->approved));
  }
  DMF_CHECK(plan.allocated_bandwidth_kbps <= plan.available_bandwidth_kbps);
  DMF_CHECK_EQ(plan.status, PlanStatus::Optimal);
}

DMF_TEST(integration, a_starved_protected_obligation_is_refused_and_escalated) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Protected, 100, 40000, 2000, 0, 0)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 1500;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto decision = evaluate_view(coordinator, contract_id(1));
  DMF_CHECK(decision.ok());
  DMF_CHECK_EQ(decision.value().outcome, DecisionOutcome::Refused);
  DMF_CHECK_EQ(decision.value().primary_reason, ReasonCode::ProtectedObligationUnmet);
  DMF_CHECK(decision.value().approved.empty());
  DMF_CHECK(decision.value().escalation_required);
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(!grant.ok());
  DMF_CHECK_EQ(grant.status().code(), ErrorCode::Refused);
}

DMF_TEST(integration, unknown_evidence_never_authorises_service) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  auto decision = evaluate_view(coordinator, contract_id(1));
  DMF_CHECK(decision.ok());
  DMF_CHECK(!is_authorising(decision.value().outcome));
  DMF_CHECK_EQ(decision.value().outcome, DecisionOutcome::Refused);
  DMF_CHECK_EQ(decision.value().primary_reason, ReasonCode::EvidenceUnknown);
  DMF_CHECK_EQ(decision.value().evidence_state, EvidenceState::Unknown);
}

DMF_TEST(integration, the_authority_ladder_separates_recommendation_from_authority) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto decision = evaluate_view(coordinator, contract_id(1));
  DMF_CHECK(decision.ok());
  DMF_CHECK_EQ(decision.value().outcome, DecisionOutcome::Degraded);
  DMF_CHECK_EQ(decision.value().max_authority, AuthorityLevel::Recommended);
  DMF_CHECK(!is_authority(decision.value().max_authority));

  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());
  DMF_CHECK_EQ(grant.value().authority_level(), AuthorityLevel::Authorized);
  DMF_CHECK(is_authority(grant.value().authority_level()));
  DMF_CHECK(!is_verified_effect(grant.value().authority_level()));

  auto acknowledged = coordinator.acknowledge(grant.value().id, grant.value().last_attempt,
                                              coordinator.now());
  DMF_CHECK(acknowledged.ok());
  DMF_CHECK_EQ(acknowledged.value().authority_level(), AuthorityLevel::Acknowledged);
  DMF_CHECK(!is_verified_effect(acknowledged.value().authority_level()));

  auto applied = coordinator.report_applied(acknowledged.value().id,
                                            acknowledged.value().last_attempt, coordinator.now());
  DMF_CHECK(applied.ok());
  DMF_CHECK(is_verified_effect(applied.value().authority_level()));
}

DMF_TEST(integration, duplicate_and_late_attempts_are_rejected) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());
  const GrantId id = grant.value().id;
  const AttemptId first = grant.value().last_attempt;
  DMF_CHECK(coordinator.acknowledge(id, first, coordinator.now()).ok());
  auto duplicate = coordinator.acknowledge(id, first, coordinator.now());
  DMF_CHECK(!duplicate.ok());
  DMF_CHECK_EQ(duplicate.status().code(), ErrorCode::ReplayDetected);
  auto unknown = coordinator.acknowledge(id, AttemptId::from_value(99999), coordinator.now());
  DMF_CHECK(!unknown.ok());
  DMF_CHECK_EQ(unknown.status().code(), ErrorCode::ReplayDetected);
}

DMF_TEST(integration, a_grant_expires_and_is_fenced_on_revalidation) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());
  coordinator.advance(1000);
  const RevalidateReport report = coordinator.revalidate(coordinator.now());
  DMF_CHECK(report.evaluated >= 1);
  DMF_CHECK(report.expired >= 1);
  auto late = coordinator.acknowledge(grant.value().id, grant.value().last_attempt,
                                      coordinator.now());
  DMF_CHECK(!late.ok());
}

DMF_TEST(integration, a_capability_change_fences_live_authority) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());

  // A favourable capability change is not a reason to fence: the granted level
  // is still serviceable, so the grant is revalidated and rebound.
  coordinator.advance(10);
  CapabilitySpec improved = spec;
  improved.bandwidth_kbps = 3000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, improved));
  DMF_CHECK_EQ(coordinator.describe(coordinator.now()).live_grants, std::uint64_t{1});
  auto rebound = coordinator.acknowledge(grant.value().id, grant.value().last_attempt,
                                         coordinator.now());
  DMF_CHECK(rebound.ok());

  // A capability change that leaves the granted level unserviceable does fence.
  coordinator.advance(10);
  CapabilitySpec worsened = spec;
  worsened.bandwidth_kbps = 1200;  // below the granted 2000, above the 1000 floor
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, worsened));
  DMF_CHECK_EQ(coordinator.describe(coordinator.now()).live_grants, std::uint64_t{0});
  auto late = coordinator.report_applied(rebound.value().id, rebound.value().last_attempt,
                                         coordinator.now());
  DMF_CHECK(!late.ok());
  DMF_CHECK(coordinator.describe(coordinator.now()).accounting_closed);
}

DMF_TEST(integration, a_policy_change_fences_live_authority) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());
  DMF_CHECK_OK(coordinator.install_policy(
      demonstration_policy(PolicyGeneration::from_value(2), 1000)));
  auto stale = coordinator.acknowledge(grant.value().id, grant.value().last_attempt,
                                       coordinator.now());
  DMF_CHECK(!stale.ok());
  DMF_CHECK_EQ(stale.status().code(), ErrorCode::InvalidState);
}

DMF_TEST(integration, restoration_requires_positive_proof) {
  Running running;
  DMF_CHECK_OK(running.start());
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto grant = coordinator.acquire(contract_id(1), coordinator.now());
  DMF_CHECK(grant.ok());

  // Capability has not recovered: restoration must not be proven.
  auto refused = coordinator.request_restoration(grant.value().id, coordinator.now());
  DMF_CHECK(refused.ok());
  DMF_CHECK(refused.value().outcome != RestorationOutcome::Proven);

  coordinator.advance(10);
  CapabilitySpec full;
  full.bandwidth_kbps = 100000;
  full.rtt_us = 500;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, full));
  auto restored = coordinator.request_restoration(grant.value().id, coordinator.now());
  DMF_CHECK(restored.ok());
  DMF_CHECK_EQ(restored.value().outcome, RestorationOutcome::Proven);
  DMF_CHECK_EQ(restored.value().evidence_state, EvidenceState::Known);
  for (const PreconditionEvaluation& evaluation : restored.value().preconditions) {
    DMF_CHECK_EQ(evaluation.result, PreconditionResult::Satisfied);
  }
}

DMF_TEST(integration, a_restart_preserves_lineage_but_not_live_authority) {
  TempDir directory{"restart"};
  GrantId grant_id{};
  AuthorityVector before;
  {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    DMF_CHECK(created.ok());
    std::unique_ptr<Coordinator> coordinator = std::move(created.value());
    DMF_CHECK_OK(coordinator->install_policy(demonstration_policy()));
    DMF_CHECK_OK(coordinator->register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
    CapabilitySpec spec;
    spec.bandwidth_kbps = 2000;
    spec.rtt_us = 2000;
    DMF_CHECK_OK(publish_capability(*coordinator, 1, 1, spec));
    auto grant = coordinator->acquire(contract_id(1), coordinator->now());
    DMF_CHECK(grant.ok());
    grant_id = grant.value().id;
    before = coordinator->authority();
    DMF_CHECK_OK(coordinator->stop());
  }
  {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    DMF_CHECK(created.ok());
    std::unique_ptr<Coordinator> coordinator = std::move(created.value());
    const AuthorityVector after = coordinator->authority();
    DMF_CHECK(after.boot > before.boot);
    DMF_CHECK(after.coordinator_term > before.coordinator_term);
    bool live = false;
    for (const Grant& grant : coordinator->state().grants) {
      if (grant.live()) live = true;
    }
    DMF_CHECK(!live);
    DMF_CHECK(!coordinator->state().fences.empty());
    auto capability = coordinator->capability(scope_id(1), coordinator->now());
    DMF_CHECK(capability.ok());
    DMF_CHECK_EQ(capability.value().state, EvidenceState::Unknown);
    auto decision = evaluate_view(*coordinator, contract_id(1));
    DMF_CHECK(decision.ok());
    DMF_CHECK(!is_authorising(decision.value().outcome));
    const StatusBody body = coordinator->describe(coordinator->now());
    DMF_CHECK(body.accounting_closed);
    DMF_CHECK(body.retained_contracts == 1);
    (void)grant_id;
    DMF_CHECK_OK(coordinator->stop());
  }
}

DMF_TEST(integration, an_infeasible_minimum_protected_requirement_is_proven_not_guessed) {
  Running running;
  DMF_CHECK_OK(running.start(1000, /*minimum_protected=*/2));
  Coordinator& coordinator = *running.coordinator;
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Protected, 100, 30000, 2000, 0, 0)));
  DMF_CHECK_OK(coordinator.register_contract(contract(2, ServiceClass::Protected, 90, 30000, 2000, 0, 0)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 40000;  // one fits, two cannot
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));
  auto decision = evaluate_view(coordinator, contract_id(1));
  DMF_CHECK(decision.ok());
  DMF_CHECK_EQ(decision.value().outcome, DecisionOutcome::Escalated);
  DMF_CHECK_EQ(decision.value().plan_status, PlanStatus::ProvenInfeasible);
  const AllocationPlan& plan = decision.value().plan;
  DMF_CHECK(plan.certificate.valid);
  DMF_CHECK_EQ(plan.certificate.required_protected, std::uint32_t{2});
  DMF_CHECK_EQ(plan.certificate.available_protected, std::uint32_t{2});
  DMF_CHECK_EQ(plan.certificate.minimum_demand_kbps, std::uint64_t{60000});
  DMF_CHECK_EQ(plan.certificate.offered_capacity_kbps, std::uint64_t{40000});
}

DMF_TEST(integration, the_same_scope_reallocates_identically_after_a_restart) {
  TempDir directory{"determinism"};
  std::uint64_t first_digest = 0;
  {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    DMF_CHECK(created.ok());
    std::unique_ptr<Coordinator> coordinator = std::move(created.value());
    DMF_CHECK_OK(coordinator->install_policy(demonstration_policy()));
    DMF_CHECK_OK(coordinator->register_contract(contract(1, ServiceClass::Protected, 100, 40000, 2000, 0, 0)));
    DMF_CHECK_OK(coordinator->register_contract(contract(2, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
    CapabilitySpec spec;
    spec.bandwidth_kbps = 42000;
    spec.rtt_us = 2000;
    DMF_CHECK_OK(publish_capability(*coordinator, 1, 1, spec));
    auto decision = evaluate_view(*coordinator, contract_id(2));
    DMF_CHECK(decision.ok());
    first_digest = decision.value().plan.digest();
    DMF_CHECK_OK(coordinator->stop());
  }
  {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    DMF_CHECK(created.ok());
    std::unique_ptr<Coordinator> coordinator = std::move(created.value());
    CapabilitySpec spec;
    spec.bandwidth_kbps = 42000;
    spec.rtt_us = 2000;
    DMF_CHECK_OK(publish_capability(*coordinator, 1, 1, spec));
    auto decision = evaluate_view(*coordinator, contract_id(2));
    DMF_CHECK(decision.ok());
    // The allocation is identical; only the identity and the authority vector
    // differ, and those are deliberately excluded from the comparison.
    DMF_CHECK_EQ(decision.value().plan.entries.size(), std::size_t{2});
    const AllocationEntry* entry = decision.value().plan.find(contract_id(2));
    DMF_CHECK(entry != nullptr);
    if (entry != nullptr) {
      DMF_CHECK_EQ(entry->allocated_bandwidth_kbps, std::uint64_t{2000});
    }
    (void)first_digest;
    DMF_CHECK_OK(coordinator->stop());
  }
}