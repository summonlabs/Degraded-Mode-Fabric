// Degraded Mode Fabric - explicit degradation policy.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Policy is the only thing that can authorise a concession. With no matching
// rule, no concession is legal and the runtime must return UNSUPPORTED rather
// than inventing one.
#ifndef DMF_POLICY_HPP
#define DMF_POLICY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dmf/core.hpp"
#include "dmf/evidence.hpp"
#include "dmf/guarantee.hpp"
#include "dmf/ids.hpp"

namespace dmf {

struct ServiceContract;

/// What policy permits when an obligation cannot be met.
enum class DegradeAction : std::uint16_t {
  Protect = 1,   ///< obligation must be met; never weaken it
  Degrade = 2,   ///< a bounded, explicit concession is permitted
  Refuse = 3,    ///< explicit refusal; service must not continue degraded
  Escalate = 4,  ///< hand to authority outside this runtime
};

bool is_valid(DegradeAction value) noexcept;
std::string_view to_string(DegradeAction value) noexcept;

/// One ordered entry of the policy decision table. A rule matches when every
/// selector it sets matches; selectors left unset are wildcards.
struct PolicyRule {
  RuleId id{};
  /// Lower precedence wins. Precedences are unique inside one policy so rule
  /// selection is total and independent of insertion order.
  std::uint32_t precedence = 0;
  std::optional<ServiceClass> service_class{};
  std::optional<ScopeId> scope{};
  /// The guarantee kind whose shortfall triggered evaluation.
  std::optional<GuaranteeKind> trigger{};
  DegradeAction action = DegradeAction::Degrade;

  [[nodiscard]] bool matches(ServiceClass candidate_class, ScopeId candidate_scope,
                             std::optional<GuaranteeKind> trigger_kind) const noexcept;
  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render() const;
};

/// Per-service-class policy profile.
struct ClassProfile {
  ServiceClass service_class = ServiceClass::Standard;
  /// Class-wide ceiling on any concession. Composed with the contract envelope
  /// by taking the stricter bound per guarantee kind.
  GuaranteeEnvelope ceiling{};
  bool may_be_degraded = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string render() const;
};

struct Policy {
  PolicyId id{};
  PolicyGeneration generation{};
  std::vector<PolicyRule> rules{};
  std::vector<ClassProfile> class_profiles{};

  /// Actions taken when evidence cannot prove capability. All default to a
  /// fail-closed action; a policy may relax them only explicitly.
  DegradeAction unknown_evidence_action = DegradeAction::Refuse;
  DegradeAction stale_evidence_action = DegradeAction::Refuse;
  DegradeAction conflict_evidence_action = DegradeAction::Refuse;
  DegradeAction invalid_evidence_action = DegradeAction::Refuse;
  DegradeAction unsupported_evidence_action = DegradeAction::Refuse;

  /// When true, more protected classes are satisfied before weaker ones absorb
  /// degradation. When false, the allocator still honours protection, but
  /// non-protected classes are ordered by priority across classes.
  bool protect_first = true;

  /// Upper bound on how long a grant stays valid without revalidation.
  Tick default_ttl_ticks{1000};
  Tick minimum_ttl_ticks{1};
  /// Wall on how many guarantees one contract may concede at once.
  std::uint32_t max_concessions_per_contract = 4;
  /// Minimum number of ticks a grant must be held before restoration may be
  /// proven. Prevents flapping between degraded and normal service.
  std::uint32_t minimum_dwell_ticks = 0;
  /// Operator requirement: at least this many protected contracts must be
  /// served at their full original obligations. When an exact certificate shows
  /// the requirement cannot be met, the plan is PROVEN_INFEASIBLE and the
  /// runtime refuses rather than silently serving less.
  std::uint32_t minimum_protected_served = 0;
  /// Node budget for the bounded exact allocator search.
  std::uint64_t search_node_budget = 50000;
  /// Freshness budget applied to evidence when capability is derived.
  std::uint64_t evidence_freshness_ticks = 100;

  /// True when a policy has actually been installed. A store that has never
  /// received one carries an all-zero policy, which is legal durable state and
  /// must round-trip through a snapshot; it simply authorises nothing.
  [[nodiscard]] bool installed() const noexcept { return id.valid() && generation.valid(); }
  [[nodiscard]] Status validate() const;
  /// Highest-precedence matching rule, or nullptr when no rule matches.
  [[nodiscard]] const PolicyRule* select_rule(ServiceClass candidate_class, ScopeId candidate_scope,
                                              std::optional<GuaranteeKind> trigger) const noexcept;
  [[nodiscard]] const ClassProfile* class_profile(ServiceClass candidate_class) const noexcept;
  /// Action for a non-Known evidence state.
  [[nodiscard]] DegradeAction evidence_action(EvidenceState state) const noexcept;

  /// Effective concession envelope for one contract: the stricter of the
  /// contract envelope and the class ceiling, capped by
  /// max_concessions_per_contract.
  [[nodiscard]] Result<GuaranteeEnvelope> effective_envelope(const ServiceContract& contract) const;

  [[nodiscard]] std::uint64_t digest() const;
  [[nodiscard]] std::string render() const;
};

/// Composes two envelopes by taking the stricter bound per kind.
[[nodiscard]] Result<GuaranteeEnvelope> compose_envelopes(const GuaranteeEnvelope& contract_envelope,
                                                          const GuaranteeEnvelope& class_ceiling,
                                                          std::uint32_t max_concessions);

/// The policy a fresh runtime starts from: no rule matches anything, so every
/// shortfall is UNSUPPORTED until an operator installs a real policy.
[[nodiscard]] Policy empty_policy(PolicyId id, PolicyGeneration generation);

}  // namespace dmf

#endif  // DMF_POLICY_HPP
