// Degraded Mode Fabric - in-process closure self test.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proves the product-defining proposition without sockets: which degraded
// service remains supportable, which guarantees are withdrawn, when authority is
// fenced, and that a restart preserves durable lineage without resurrecting live
// evidence. Every fixture used here is SYNTHETIC.
#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "dmf/runtime.hpp"
#include "tool_support.hpp"

namespace {

int g_failures = 0;

/// Self-removing scratch directory. The self test never writes outside it.
class ScratchDir {
 public:
  explicit ScratchDir(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    std::error_code error;
    const std::filesystem::path base = std::filesystem::current_path(error);
    std::filesystem::path candidate =
        base / ("dmf-selftest-" + label + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(candidate, error);
    path_ = candidate.string();
  }
  ~ScratchDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_{};
};

void check(bool condition, const std::string& label) {
  std::cout << (condition ? "[ PASS ] " : "[ FAIL ] ") << label << std::endl;
  if (!condition) ++g_failures;
}

/// Reports a failed decision check together with the decision that produced it,
/// so a failure is diagnosable from the self test output alone.
void check_decision(bool condition, const std::string& label,
                    const dmf::Result<dmf::Decision>& result) {
  if (!condition) {
    if (result.ok()) {
      std::cout << result.value().explain() << std::endl;
    } else {
      std::cout << "  evaluation failed: " << result.status().render() << std::endl;
    }
  }
  check(condition, label);
}

dmf::EvidenceItem evidence(dmf::ScopeId scope, dmf::PublisherId publisher, dmf::BootIncarnation boot,
                           dmf::EvidenceKind kind, std::uint64_t value, dmf::Tick observed) {
  dmf::EvidenceItem item;
  item.publisher = publisher;
  item.publisher_boot = boot;
  item.generation = dmf::EvidenceGeneration::from_value(1);
  item.kind = kind;
  item.scope = scope;
  item.state = dmf::EvidenceState::Known;
  item.value = value;
  item.observed_tick = observed;
  item.origin = dmf::OriginClass::Synthetic;
  return item;
}

void publish_capability(dmf::Coordinator& coordinator, dmf::ScopeId scope, dmf::PublisherId publisher,
                        std::uint64_t bandwidth, std::uint64_t rtt, std::uint64_t coverage) {
  const dmf::BootIncarnation boot = coordinator.authority().boot;
  const dmf::Tick now = coordinator.now();
  const struct {
    dmf::EvidenceKind kind;
    std::uint64_t value;
  } items[] = {
      {dmf::EvidenceKind::FabricTopology, 1},
      {dmf::EvidenceKind::AggregateBandwidth, bandwidth},
      {dmf::EvidenceKind::RoundTripLatency, rtt},
      {dmf::EvidenceKind::PathDiversity, 4},
      {dmf::EvidenceKind::Reachability, 1000000},
      {dmf::EvidenceKind::NodeCoverage, coverage},
      {dmf::EvidenceKind::SynchronousDurability, 1},
  };
  for (const auto& entry : items) {
    const dmf::Status status =
        coordinator.publish_evidence(evidence(scope, publisher, boot, entry.kind, entry.value, now));
    if (!status.ok()) {
      std::cout << "[ FAIL ] publish " << dmf::to_string(entry.kind) << ": " << status.render()
                << std::endl;
      ++g_failures;
      return;
    }
  }
}

int run() {
  ScratchDir directory("closure");
  const dmf::ScopeId scope = dmf::ScopeId::from_value(1);
  const dmf::SubjectId subject = dmf::SubjectId::from_value(1);
  const dmf::ContractId protected_contract = dmf::ContractId::from_value(1);
  const dmf::ContractId flexible_contract = dmf::ContractId::from_value(2);

  {
    dmf::CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    config.origin = dmf::OriginClass::Synthetic;
    auto created = dmf::Coordinator::create(config);
    if (!created.ok()) {
      std::cout << "[ FAIL ] coordinator create: " << created.status().render() << std::endl;
      return 1;
    }
    std::unique_ptr<dmf::Coordinator> coordinator = std::move(created.value());

    // 1. No policy: nothing can be authorised.
    check(coordinator->register_contract(dmf::tool::make_contract(
              protected_contract, dmf::ContractGeneration::from_value(1), scope, subject,
              dmf::SubjectGeneration::from_value(1), dmf::ServiceClass::Protected, 100, 40000, 2000,
              0, 0))
              .ok(),
          "register protected contract");
    check(coordinator->register_contract(dmf::tool::make_contract(
              flexible_contract, dmf::ContractGeneration::from_value(1), scope, subject,
              dmf::SubjectGeneration::from_value(1), dmf::ServiceClass::Standard, 10, 4000, 2000,
              1000, 3))
              .ok(),
          "register flexible contract");
    publish_capability(*coordinator, scope, dmf::PublisherId::from_value(1), 100000, 500, 1000000);
    auto without_policy = coordinator->evaluate(protected_contract);
    check(!without_policy.ok() && without_policy.status().code() == dmf::ErrorCode::InvalidState,
          "evaluation without a policy is refused");

    // 2. Install the demonstration policy and evaluate at full capability.
    const dmf::Policy policy =
        dmf::tool::standard_policy(dmf::PolicyId::from_value(1), dmf::PolicyGeneration::from_value(1));
    check(coordinator->install_policy(policy).ok(), "install policy");
    auto full_protected = coordinator->evaluate(protected_contract);
    auto full_flexible = coordinator->evaluate(flexible_contract);
    check(full_protected.ok() && full_protected.value().outcome == dmf::DecisionOutcome::Full,
          "protected contract is fully supportable at full capability");
    check(full_flexible.ok() && full_flexible.value().outcome == dmf::DecisionOutcome::Full,
          "flexible contract is fully supportable at full capability");
    if (full_flexible.ok()) {
      check(dmf::weakens_or_equals(full_flexible.value().original, full_flexible.value().approved),
            "approved obligations never exceed the original");
    }

    // 3. Reduce bandwidth to exactly the protected demand: the flexible
    //    contract absorbs the whole shortfall and the protected one is untouched.
    coordinator->advance(60);
    publish_capability(*coordinator, scope, dmf::PublisherId::from_value(1), 42000, 2000, 1000000);
    auto degraded_flexible = coordinator->evaluate(flexible_contract);
    auto still_full_protected = coordinator->evaluate(protected_contract);
    check_decision(degraded_flexible.ok() &&
                       degraded_flexible.value().outcome == dmf::DecisionOutcome::Degraded,
                   "weaker traffic absorbs degradation first", degraded_flexible);
    check_decision(still_full_protected.ok() &&
                       still_full_protected.value().outcome == dmf::DecisionOutcome::Full,
                   "protected obligation is untouched while weaker traffic degrades",
                   still_full_protected);
    if (degraded_flexible.ok()) {
      const dmf::Decision& decision = degraded_flexible.value();
      check(dmf::weakens_or_equals(decision.original, decision.approved),
            "degraded contract is a weakening of the original");
      check(!decision.delta.empty(), "the concession is explicit and non-empty");
    }

    // 4. Starve the scope below the protected demand: the protected obligation
    //    must be refused outright, never quietly weakened.
    coordinator->advance(60);
    publish_capability(*coordinator, scope, dmf::PublisherId::from_value(1), 1500, 2000, 1000000);
    auto starved_protected = coordinator->evaluate(protected_contract);
    check_decision(starved_protected.ok() &&
                       starved_protected.value().outcome == dmf::DecisionOutcome::Refused,
                   "an unmet protected obligation is explicitly refused", starved_protected);
    if (starved_protected.ok()) {
      check(starved_protected.value().approved.empty() &&
                starved_protected.value().delta.empty(),
            "a refused protected contract carries no weakened contract");
      check(starved_protected.value().escalation_required,
            "a refused protected obligation asks for escalation");
    }

    // 5. Restore capability and walk the authority ladder.
    coordinator->advance(60);
    publish_capability(*coordinator, scope, dmf::PublisherId::from_value(1), 100000, 500, 1000000);
    auto grant_result = coordinator->acquire(flexible_contract, coordinator->now());
    check(grant_result.ok(), "acquire degraded authority");
    if (grant_result.ok()) {
      const dmf::Grant grant = grant_result.value();
      check(grant.state == dmf::GrantState::Issued &&
                dmf::is_authority(grant.authority_level()),
            "an issued grant is authorising authority");
      auto acknowledged = coordinator->acknowledge(grant.id, grant.last_attempt, coordinator->now());
      check(acknowledged.ok() && acknowledged.value().state == dmf::GrantState::Acknowledged,
            "acknowledgement advances the ladder without claiming effect");
      if (acknowledged.ok()) {
        const dmf::Grant after_ack = acknowledged.value();
        auto replay = coordinator->report_applied(grant.id, grant.last_attempt, coordinator->now());
        check(!replay.ok() && replay.status().code() == dmf::ErrorCode::ReplayDetected,
              "a replayed attempt identity is rejected");
        auto applied =
            coordinator->report_applied(after_ack.id, after_ack.last_attempt, coordinator->now());
        check(applied.ok() && applied.value().state == dmf::GrantState::Applied,
              "only a verified attempt reports the effect");
        auto restored = coordinator->request_restoration(after_ack.id, coordinator->now());
        check(restored.ok() &&
                  restored.value().outcome == dmf::RestorationOutcome::Proven,
              "restoration is proven when capability again supports the original");
        auto replayed = coordinator->report_applied(after_ack.id, after_ack.last_attempt,
                                                    coordinator->now());
        check(!replayed.ok(), "a grant closed by restoration accepts no further effect reports");
      }
    }

    // 6. Evidence that is not Known can never authorise full service.
    coordinator->advance(500);
    auto stale = coordinator->evaluate(flexible_contract);
    check(stale.ok() && !dmf::is_authorising(stale.value().outcome),
          "stale evidence never authorises service");
    if (stale.ok()) {
      check(stale.value().evidence_state == dmf::EvidenceState::Stale,
            "the decision names the stale evidence state explicitly");
    }

    // 7. Live authority exists at the moment of the restart.
    coordinator->advance(10);
    publish_capability(*coordinator, scope, dmf::PublisherId::from_value(1), 100000, 500, 1000000);
    auto live = coordinator->acquire(flexible_contract, coordinator->now());
    check(live.ok(), "acquire authority before restart");
    const dmf::GrantId live_id = live.ok() ? live.value().id : dmf::GrantId{};
    const dmf::AuthorityVector before = coordinator->authority();
    check(coordinator->stop().ok(), "clean shutdown");
    (void)before;
    (void)live_id;
  }

  // 8. Restart: durable lineage survives, live authority does not.
  {
    dmf::CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    config.origin = dmf::OriginClass::Synthetic;
    auto created = dmf::Coordinator::create(config);
    if (!created.ok()) {
      std::cout << "  reopen failed: " << created.status().render() << std::endl;
    }
    check(created.ok(), "restart opens the same durable store");
    if (!created.ok()) return 1;
    std::unique_ptr<dmf::Coordinator> coordinator = std::move(created.value());
    const dmf::DurableState& state = coordinator->state();
    check(state.contracts.size() == 2, "contract definitions survive the restart");
    check(state.policy.validate().ok(), "policy survives the restart");
    check(coordinator->recovery() == dmf::RecoveryOutcome::TornTailRecovered ||
              coordinator->recovery() == dmf::RecoveryOutcome::CleanReopen,
          "the store reports an explicit recovery outcome");
    bool any_live = false;
    for (const dmf::Grant& grant : state.grants) {
      if (grant.live()) any_live = true;
    }
    check(!any_live, "no pre-restart grant is live after the restart");
    check(!state.grants.empty(), "pre-restart grant lineage is preserved");
    check(!state.fences.empty(), "the fences that ended that authority are durable");
    auto capability = coordinator->capability(scope, coordinator->now());
    check(capability.ok() && capability.value().state == dmf::EvidenceState::Unknown,
          "live evidence is not resurrected by the restart");
    auto decision = coordinator->evaluate(flexible_contract);
    check(decision.ok() && !dmf::is_authorising(decision.value().outcome),
          "a restarted runtime refuses service until evidence is re-published");
    const dmf::StatusBody body = coordinator->describe(coordinator->now());
    check(body.accounting_closed, "accounting closure holds across the restart");
    check(coordinator->stop().ok(), "clean shutdown after restart");
  }

  std::cout << (g_failures == 0 ? "SELFTEST OK" : "SELFTEST FAILED") << " failures=" << g_failures
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main() { return run(); }