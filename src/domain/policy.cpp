// Degraded Mode Fabric - policy validation and envelope composition.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/policy.hpp"

#include <algorithm>
#include <cstdio>

#include "dmf/contract.hpp"

namespace dmf {

bool is_valid(DegradeAction value) noexcept {
  switch (value) {
    case DegradeAction::Protect:
    case DegradeAction::Degrade:
    case DegradeAction::Refuse:
    case DegradeAction::Escalate:
      return true;
  }
  return false;
}

std::string_view to_string(DegradeAction value) noexcept {
  switch (value) {
    case DegradeAction::Protect: return "PROTECT";
    case DegradeAction::Degrade: return "DEGRADE";
    case DegradeAction::Refuse: return "REFUSE";
    case DegradeAction::Escalate: return "ESCALATE";
  }
  return "UNRECOGNISED";
}

bool PolicyRule::matches(ServiceClass candidate_class, ScopeId candidate_scope,
                         std::optional<GuaranteeKind> trigger_kind) const noexcept {
  if (service_class.has_value() && *service_class != candidate_class) return false;
  if (scope.has_value() && *scope != candidate_scope) return false;
  if (trigger.has_value()) {
    if (!trigger_kind.has_value()) return false;
    if (*trigger != *trigger_kind) return false;
  }
  return true;
}

Status PolicyRule::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "policy rule identity is unset");
  if (service_class.has_value() && !is_valid(*service_class)) {
    return Status(ErrorCode::Invalid, "policy rule names an invalid service class");
  }
  if (scope.has_value() && !scope->valid()) {
    return Status(ErrorCode::Invalid, "policy rule names an unset scope");
  }
  if (trigger.has_value() && !is_valid(*trigger)) {
    return Status(ErrorCode::Invalid, "policy rule names an invalid guarantee kind");
  }
  if (!is_valid(action)) return Status(ErrorCode::Invalid, "policy rule has an invalid action");
  return Status{};
}

std::string PolicyRule::render() const {
  std::string out = "rule#";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%u", precedence);
  out.append(buffer);
  out.push_back('(');
  out.append(service_class.has_value() ? std::string(to_string(*service_class)) : std::string("*"));
  out.push_back(',');
  out.append(scope.has_value() ? scope->to_string() : std::string("*"));
  out.push_back(',');
  out.append(trigger.has_value() ? std::string(to_string(*trigger)) : std::string("*"));
  out.append(") -> ");
  out.append(to_string(action));
  return out;
}

Status ClassProfile::validate() const {
  if (!is_valid(service_class)) {
    return Status(ErrorCode::Invalid, "class profile names an invalid service class");
  }
  if (service_class == ServiceClass::Protected && may_be_degraded) {
    return Status(ErrorCode::Invalid, "protected service class cannot be degradable");
  }
  return ceiling.validate();
}

std::string ClassProfile::render() const {
  std::string out = "class=";
  out.append(to_string(service_class));
  out.append(may_be_degraded ? " degradable" : " frozen");
  out.append(" ceiling=");
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "bounds:%llu max:%u",
                static_cast<unsigned long long>(ceiling.size()), ceiling.max_concessions());
  out.append(buffer);
  return out;
}

Status Policy::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "policy identity is unset");
  if (!generation.valid()) return Status(ErrorCode::InvalidArgument, "policy generation is unset");
  if (rules.size() > kMaxCollectionItems) {
    return Status(ErrorCode::CapacityExceeded, "policy carries too many rules");
  }
  std::uint32_t previous = 0;
  bool first = true;
  std::vector<std::uint32_t> seen;
  seen.reserve(rules.size());
  for (const PolicyRule& rule : rules) {
    Status status = rule.validate();
    if (!status.ok()) return status;
    if (!first && rule.precedence == previous) {
      return Status(ErrorCode::AlreadyExists, "policy rule precedences are not unique");
    }
    first = false;
    previous = rule.precedence;
    seen.push_back(rule.precedence);
  }
  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
    return Status(ErrorCode::AlreadyExists, "policy rule precedences are not unique");
  }
  std::vector<std::uint16_t> classes;
  for (const ClassProfile& profile : class_profiles) {
    Status status = profile.validate();
    if (!status.ok()) return status;
    classes.push_back(static_cast<std::uint16_t>(profile.service_class));
  }
  std::sort(classes.begin(), classes.end());
  if (std::adjacent_find(classes.begin(), classes.end()) != classes.end()) {
    return Status(ErrorCode::AlreadyExists, "policy declares a class profile twice");
  }
  if (!default_ttl_ticks.value) {
    return Status(ErrorCode::Invalid, "policy default ttl must be at least one tick");
  }
  if (!minimum_ttl_ticks.value) {
    return Status(ErrorCode::Invalid, "policy minimum ttl must be at least one tick");
  }
  if (minimum_ttl_ticks > default_ttl_ticks) {
    return Status(ErrorCode::Invalid, "policy minimum ttl exceeds the default ttl");
  }
  if (max_concessions_per_contract > static_cast<std::uint32_t>(kMaxCollectionItems)) {
    return Status(ErrorCode::OutOfRange, "policy concession cap is out of range");
  }
  if (minimum_protected_served > static_cast<std::uint32_t>(kMaxCollectionItems)) {
    return Status(ErrorCode::OutOfRange, "policy minimum protected service is out of range");
  }
  if (search_node_budget == 0 || search_node_budget > 100000000ULL) {
    return Status(ErrorCode::OutOfRange, "policy search node budget is out of range");
  }
  if (evidence_freshness_ticks == 0) {
    return Status(ErrorCode::Invalid, "policy evidence freshness budget must be non-zero");
  }
  const DegradeAction evidence_actions[] = {unknown_evidence_action, stale_evidence_action,
                                            conflict_evidence_action, invalid_evidence_action,
                                            unsupported_evidence_action};
  for (const DegradeAction action : evidence_actions) {
    if (!is_valid(action)) return Status(ErrorCode::Invalid, "policy evidence action is invalid");
  }
  return Status{};
}

const PolicyRule* Policy::select_rule(ServiceClass candidate_class, ScopeId candidate_scope,
                                      std::optional<GuaranteeKind> trigger) const noexcept {
  const PolicyRule* best = nullptr;
  for (const PolicyRule& rule : rules) {
    if (!rule.matches(candidate_class, candidate_scope, trigger)) continue;
    if (best == nullptr || rule.precedence < best->precedence) best = &rule;
  }
  return best;
}

const ClassProfile* Policy::class_profile(ServiceClass candidate_class) const noexcept {
  for (const ClassProfile& profile : class_profiles) {
    if (profile.service_class == candidate_class) return &profile;
  }
  return nullptr;
}

DegradeAction Policy::evidence_action(EvidenceState state) const noexcept {
  switch (state) {
    case EvidenceState::Known: return DegradeAction::Protect;
    case EvidenceState::Unknown: return unknown_evidence_action;
    case EvidenceState::Stale: return stale_evidence_action;
    case EvidenceState::Conflict: return conflict_evidence_action;
    case EvidenceState::Invalid: return invalid_evidence_action;
    case EvidenceState::Unsupported: return unsupported_evidence_action;
  }
  return DegradeAction::Refuse;
}

Result<GuaranteeEnvelope> compose_envelopes(const GuaranteeEnvelope& contract_envelope,
                                            const GuaranteeEnvelope& class_ceiling,
                                            std::uint32_t max_concessions) {
  // upsert keeps the stricter bound per guarantee kind: for AtLeast kinds the
  // lower floor wins, for AtMost kinds the higher ceiling wins.
  GuaranteeEnvelope composed;
  for (const ConcessionBound& bound : contract_envelope.bounds()) {
    Status status = composed.upsert(bound);
    if (!status.ok()) return status;
  }
  for (const ConcessionBound& bound : class_ceiling.bounds()) {
    Status status = composed.upsert(bound);
    if (!status.ok()) return status;
  }
  Status status = composed.normalize();
  if (!status.ok()) return status;
  std::uint32_t cap = max_concessions;
  if (contract_envelope.max_concessions() < cap) cap = contract_envelope.max_concessions();
  if (class_ceiling.max_concessions() < cap) cap = class_ceiling.max_concessions();
  composed.set_max_concessions(cap);
  return composed;
}

Result<GuaranteeEnvelope> Policy::effective_envelope(const ServiceContract& contract) const {
  const ClassProfile* profile = class_profile(contract.service_class);
  if (profile == nullptr) {
    return Status(ErrorCode::NotFound, "policy declares no profile for this service class");
  }
  if (contract.protected_obligation() || !profile->may_be_degraded) {
    GuaranteeEnvelope frozen;
    frozen.set_max_concessions(0);
    return frozen;
  }
  return compose_envelopes(contract.envelope, profile->ceiling, max_concessions_per_contract);
}

std::uint64_t Policy::digest() const {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(generation.value());
  digest.update_u32(static_cast<std::uint32_t>(rules.size()));
  std::vector<const PolicyRule*> ordered;
  ordered.reserve(rules.size());
  for (const PolicyRule& rule : rules) ordered.push_back(&rule);
  std::sort(ordered.begin(), ordered.end(), [](const PolicyRule* a, const PolicyRule* b) {
    if (a->precedence != b->precedence) return a->precedence < b->precedence;
    return a->id < b->id;
  });
  for (const PolicyRule* rule : ordered) {
    digest.update_u64(rule->id.value());
    digest.update_u32(rule->precedence);
    digest.update_u16(rule->service_class.has_value()
                          ? static_cast<std::uint16_t>(*rule->service_class)
                          : std::uint16_t{0});
    digest.update_u64(rule->scope.has_value() ? rule->scope->value() : std::uint64_t{0});
    digest.update_u16(rule->trigger.has_value() ? static_cast<std::uint16_t>(*rule->trigger)
                                                : std::uint16_t{0});
    digest.update_u16(static_cast<std::uint16_t>(rule->action));
  }
  digest.update_u32(static_cast<std::uint32_t>(class_profiles.size()));
  for (const ClassProfile& profile : class_profiles) {
    digest.update_u16(static_cast<std::uint16_t>(profile.service_class));
    digest.update_u64(profile.ceiling.digest());
    digest.update_bool(profile.may_be_degraded);
  }
  digest.update_u16(static_cast<std::uint16_t>(unknown_evidence_action));
  digest.update_u16(static_cast<std::uint16_t>(stale_evidence_action));
  digest.update_u16(static_cast<std::uint16_t>(conflict_evidence_action));
  digest.update_u16(static_cast<std::uint16_t>(invalid_evidence_action));
  digest.update_u16(static_cast<std::uint16_t>(unsupported_evidence_action));
  digest.update_bool(protect_first);
  digest.update_u64(default_ttl_ticks.value);
  digest.update_u64(minimum_ttl_ticks.value);
  digest.update_u32(max_concessions_per_contract);
  digest.update_u32(minimum_dwell_ticks);
  digest.update_u32(minimum_protected_served);
  digest.update_u64(search_node_budget);
  digest.update_u64(evidence_freshness_ticks);
  return digest.value();
}

std::string Policy::render() const {
  std::string out = "policy=";
  out.append(id.to_string());
  out.append(" gen=");
  out.append(generation.to_string());
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), " rules=%llu classes=%llu ttl=%llu dwell=%u budget=%llu",
                static_cast<unsigned long long>(rules.size()),
                static_cast<unsigned long long>(class_profiles.size()),
                static_cast<unsigned long long>(default_ttl_ticks.value), minimum_dwell_ticks,
                static_cast<unsigned long long>(search_node_budget));
  out.append(buffer);
  return out;
}

Policy empty_policy(PolicyId id, PolicyGeneration generation) {
  Policy policy;
  policy.id = id;
  policy.generation = generation;
  policy.rules.clear();
  policy.class_profiles.clear();
  policy.unknown_evidence_action = DegradeAction::Refuse;
  policy.stale_evidence_action = DegradeAction::Refuse;
  policy.conflict_evidence_action = DegradeAction::Refuse;
  policy.invalid_evidence_action = DegradeAction::Refuse;
  policy.unsupported_evidence_action = DegradeAction::Refuse;
  policy.protect_first = true;
  return policy;
}

}  // namespace dmf
