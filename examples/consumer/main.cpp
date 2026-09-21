// Degraded Mode Fabric - downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Linked against the installed package only. It answers the product-defining
// question end to end and fails loudly if any invariant breaks, so a broken
// installation cannot pass this program.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <dmf/dmf.hpp>

namespace {

int g_failures = 0;

void check(bool condition, const char* label) {
  std::cout << (condition ? "[ PASS ] " : "[ FAIL ] ") << label << '\n';
  if (!condition) ++g_failures;
}

dmf::ServiceContract make_contract(std::uint64_t id, dmf::ServiceClass service_class,
                                   std::uint32_t priority, std::uint64_t bandwidth,
                                   std::uint64_t floor) {
  dmf::ServiceContract contract;
  contract.id = dmf::ContractId::from_value(id);
  contract.generation = dmf::ContractGeneration::from_value(1);
  contract.scope = dmf::ScopeId::from_value(1);
  contract.subject = dmf::SubjectId::from_value(id);
  contract.subject_generation = dmf::SubjectGeneration::from_value(1);
  contract.service_class = service_class;
  contract.non_degradable = service_class == dmf::ServiceClass::Protected;
  contract.priority = priority;
  (void)contract.original.insert(dmf::GuaranteeKind::Bandwidth, bandwidth);
  (void)contract.original.insert(dmf::GuaranteeKind::LatencyP99, 2000);
  (void)contract.original.insert(dmf::GuaranteeKind::PathDiversity, 1);
  (void)contract.original.insert(dmf::GuaranteeKind::Reachability, 900000);
  if (!contract.protected_obligation()) {
    (void)contract.envelope.insert(dmf::ConcessionBound{dmf::GuaranteeKind::Bandwidth, floor});
    (void)contract.envelope.insert(dmf::ConcessionBound{dmf::GuaranteeKind::LatencyP99, 100000});
    contract.envelope.set_max_concessions(3);
  }
  return contract;
}

dmf::Policy make_policy() {
  dmf::Policy policy;
  policy.id = dmf::PolicyId::from_value(1);
  policy.generation = dmf::PolicyGeneration::from_value(1);
  policy.protect_first = true;
  policy.default_ttl_ticks = dmf::Tick{50};
  policy.evidence_freshness_ticks = 1000;
  policy.search_node_budget = 20000;
  dmf::PolicyRule degrade;
  degrade.id = dmf::RuleId::from_value(1);
  degrade.precedence = 10;
  degrade.action = dmf::DegradeAction::Degrade;
  policy.rules.push_back(degrade);
  dmf::PolicyRule protect;
  protect.id = dmf::RuleId::from_value(2);
  protect.precedence = 1;
  protect.service_class = dmf::ServiceClass::Protected;
  protect.action = dmf::DegradeAction::Protect;
  policy.rules.push_back(protect);
  dmf::ClassProfile rigid;
  rigid.service_class = dmf::ServiceClass::Protected;
  rigid.may_be_degraded = false;
  rigid.ceiling.set_max_concessions(0);
  policy.class_profiles.push_back(rigid);
  for (const dmf::ServiceClass service_class :
       {dmf::ServiceClass::Standard, dmf::ServiceClass::BestEffort}) {
    dmf::ClassProfile profile;
    profile.service_class = service_class;
    profile.may_be_degraded = true;
    profile.ceiling.set_max_concessions(3);
    policy.class_profiles.push_back(profile);
  }
  return policy;
}

}  // namespace

int main() {
  std::cout << "dmf consumer against version " << dmf::kVersionString << '\n';
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "dmf-consumer-store";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  dmf::CoordinatorConfig config;
  config.store.root = root.string();
  config.allow_anonymous_sessions = true;
  config.origin = dmf::OriginClass::Synthetic;
  auto created = dmf::Coordinator::create(config);
  if (!created.ok()) {
    std::cout << "[ FAIL ] coordinator create: " << created.status().render() << '\n';
    return 1;
  }
  std::unique_ptr<dmf::Coordinator> coordinator = std::move(created.value());
  check(coordinator->install_policy(make_policy()).ok(), "install policy");
  check(coordinator->register_contract(make_contract(1, dmf::ServiceClass::Protected, 100, 40000, 0)).ok(),
        "register a protected obligation");
  check(coordinator->register_contract(make_contract(2, dmf::ServiceClass::Standard, 10, 4000, 1000)).ok(),
        "register a flexible obligation");

  const dmf::BootIncarnation boot = coordinator->authority().boot;
  const dmf::Tick now = coordinator->now();
  const struct {
    dmf::EvidenceKind kind;
    std::uint64_t value;
  } items[] = {
      {dmf::EvidenceKind::FabricTopology, 1},
      {dmf::EvidenceKind::AggregateBandwidth, 42000},
      {dmf::EvidenceKind::RoundTripLatency, 2000},
      {dmf::EvidenceKind::PathDiversity, 4},
      {dmf::EvidenceKind::Reachability, 1000000},
      {dmf::EvidenceKind::NodeCoverage, 1000000},
      {dmf::EvidenceKind::SynchronousDurability, 1},
  };
  for (const auto& entry : items) {
    dmf::EvidenceItem item;
    item.publisher = dmf::PublisherId::from_value(1);
    item.publisher_boot = boot;
    item.generation = dmf::EvidenceGeneration::from_value(1);
    item.kind = entry.kind;
    item.scope = dmf::ScopeId::from_value(1);
    item.state = dmf::EvidenceState::Known;
    item.value = entry.value;
    item.observed_tick = now;
    item.origin = dmf::OriginClass::Synthetic;
    check(coordinator->publish_evidence(item).ok(), "publish synthetic evidence");
  }

  auto protected_view = coordinator->evaluate_with_plan(dmf::ContractId::from_value(1));
  auto flexible_view = coordinator->evaluate_with_plan(dmf::ContractId::from_value(2));
  check(protected_view.ok(), "evaluate the protected obligation");
  check(flexible_view.ok(), "evaluate the flexible obligation");
  if (protected_view.ok() && flexible_view.ok()) {
    check(protected_view.value().outcome == dmf::DecisionOutcome::Full,
          "the protected obligation is fully supportable");
    check(flexible_view.value().outcome == dmf::DecisionOutcome::Degraded,
          "the flexible obligation absorbs the shortfall");
    check(dmf::weakens_or_equals(flexible_view.value().original, flexible_view.value().approved),
          "degraded output never exceeds original authority");
    check(flexible_view.value().plan.allocated_bandwidth_kbps <=
              flexible_view.value().plan.available_bandwidth_kbps,
          "the plan never allocates more than the fabric offers");
    std::cout << flexible_view.value().explain() << '\n';
  }

  auto grant = coordinator->acquire(dmf::ContractId::from_value(2), coordinator->now());
  check(grant.ok(), "acquire degraded authority");
  if (grant.ok()) {
    check(dmf::is_authority(grant.value().authority_level()), "an issued grant is authority");
    check(!dmf::is_verified_effect(grant.value().authority_level()),
          "an issued grant is not a verified effect");
    auto acknowledged = coordinator->acknowledge(grant.value().id, grant.value().last_attempt,
                                                 coordinator->now());
    check(acknowledged.ok(), "acknowledge the grant");
  }

  const dmf::StatusBody body = coordinator->describe(coordinator->now());
  check(body.accounting_closed, "accounting closure holds");
  check(coordinator->stop().ok(), "clean shutdown");
  std::filesystem::remove_all(root, error);
  std::cout << (g_failures == 0 ? "CONSUMER OK" : "CONSUMER FAILED") << '\n';
  return g_failures == 0 ? 0 : 1;
}
