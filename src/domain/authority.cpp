// Degraded Mode Fabric - authority binding and lifecycle rules.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/authority.hpp"

#include <cstdio>

namespace dmf {

std::uint64_t AuthorityVector::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(coordinator_term.value());
  digest.update_u64(boot.value());
  digest.update_u64(fabric.value());
  digest.update_u64(capacity.value());
  digest.update_u64(policy.value());
  digest.update_u64(evidence.value());
  return digest.value();
}

std::string AuthorityVector::render() const {
  std::string out = "term=";
  out.append(coordinator_term.to_string());
  out.append(" boot=");
  out.append(boot.to_string());
  out.append(" fabric=");
  out.append(fabric.to_string());
  out.append(" capacity=");
  out.append(capacity.to_string());
  out.append(" policy=");
  out.append(policy.to_string());
  out.append(" evidence=");
  out.append(evidence.to_string());
  return out;
}

std::string to_string(AuthorityDelta value) {
  if (value == AuthorityDelta::None) return "NONE";
  std::string out;
  const auto append = [&out](std::string_view name) {
    if (!out.empty()) out.push_back('|');
    out.append(name);
  };
  if (has_flag(value, AuthorityDelta::CoordinatorTerm)) append("COORDINATOR_TERM");
  if (has_flag(value, AuthorityDelta::Boot)) append("BOOT");
  if (has_flag(value, AuthorityDelta::Fabric)) append("FABRIC");
  if (has_flag(value, AuthorityDelta::Capacity)) append("CAPACITY");
  if (has_flag(value, AuthorityDelta::Policy)) append("POLICY");
  if (has_flag(value, AuthorityDelta::Evidence)) append("EVIDENCE");
  if (has_flag(value, AuthorityDelta::Contract)) append("CONTRACT");
  if (has_flag(value, AuthorityDelta::ContractGeneration)) append("CONTRACT_GENERATION");
  if (has_flag(value, AuthorityDelta::Subject)) append("SUBJECT");
  if (has_flag(value, AuthorityDelta::SubjectGeneration)) append("SUBJECT_GENERATION");
  if (has_flag(value, AuthorityDelta::Scope)) append("SCOPE");
  return out;
}

AuthorityDelta AuthorityBinding::classify(const AuthorityBinding& other) const noexcept {
  AuthorityDelta delta = AuthorityDelta::None;
  if (vector.coordinator_term != other.vector.coordinator_term) {
    delta = delta | AuthorityDelta::CoordinatorTerm;
  }
  if (vector.boot != other.vector.boot) delta = delta | AuthorityDelta::Boot;
  if (vector.fabric != other.vector.fabric) delta = delta | AuthorityDelta::Fabric;
  if (vector.capacity != other.vector.capacity) delta = delta | AuthorityDelta::Capacity;
  if (vector.policy != other.vector.policy) delta = delta | AuthorityDelta::Policy;
  if (vector.evidence != other.vector.evidence) delta = delta | AuthorityDelta::Evidence;
  if (contract != other.contract) delta = delta | AuthorityDelta::Contract;
  if (contract_generation != other.contract_generation) {
    delta = delta | AuthorityDelta::ContractGeneration;
  }
  if (subject != other.subject) delta = delta | AuthorityDelta::Subject;
  if (subject_generation != other.subject_generation) {
    delta = delta | AuthorityDelta::SubjectGeneration;
  }
  if (scope != other.scope) delta = delta | AuthorityDelta::Scope;
  return delta;
}

std::uint64_t AuthorityBinding::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(vector.digest());
  digest.update_u64(contract.value());
  digest.update_u64(contract_generation.value());
  digest.update_u64(subject.value());
  digest.update_u64(subject_generation.value());
  digest.update_u64(scope.value());
  return digest.value();
}

std::string AuthorityBinding::render() const {
  std::string out = "contract=";
  out.append(contract.to_string());
  out.push_back('/');
  out.append(contract_generation.to_string());
  out.append(" subject=");
  out.append(subject.to_string());
  out.push_back('/');
  out.append(subject_generation.to_string());
  out.append(" scope=");
  out.append(scope.to_string());
  out.push_back(' ');
  out.append(vector.render());
  return out;
}

bool is_valid(AuthorityLevel value) noexcept {
  switch (value) {
    case AuthorityLevel::None:
    case AuthorityLevel::Observed:
    case AuthorityLevel::Eligible:
    case AuthorityLevel::Recommended:
    case AuthorityLevel::Authorized:
    case AuthorityLevel::Acknowledged:
    case AuthorityLevel::Applied:
      return true;
  }
  return false;
}

std::string_view to_string(AuthorityLevel value) noexcept {
  switch (value) {
    case AuthorityLevel::None: return "NONE";
    case AuthorityLevel::Observed: return "OBSERVED";
    case AuthorityLevel::Eligible: return "ELIGIBLE";
    case AuthorityLevel::Recommended: return "RECOMMENDED";
    case AuthorityLevel::Authorized: return "AUTHORIZED";
    case AuthorityLevel::Acknowledged: return "ACKNOWLEDGED";
    case AuthorityLevel::Applied: return "APPLIED";
  }
  return "UNRECOGNISED";
}

bool is_authority(AuthorityLevel value) noexcept {
  return static_cast<std::uint16_t>(value) >= static_cast<std::uint16_t>(AuthorityLevel::Authorized);
}

bool is_verified_effect(AuthorityLevel value) noexcept { return value == AuthorityLevel::Applied; }

bool is_valid(GrantState value) noexcept {
  switch (value) {
    case GrantState::Issued:
    case GrantState::Acknowledged:
    case GrantState::Applied:
    case GrantState::Fenced:
    case GrantState::Expired:
    case GrantState::Revoked:
      return true;
  }
  return false;
}

std::string_view to_string(GrantState value) noexcept {
  switch (value) {
    case GrantState::Issued: return "ISSUED";
    case GrantState::Acknowledged: return "ACKNOWLEDGED";
    case GrantState::Applied: return "APPLIED";
    case GrantState::Fenced: return "FENCED";
    case GrantState::Expired: return "EXPIRED";
    case GrantState::Revoked: return "REVOKED";
  }
  return "UNRECOGNISED";
}

bool grant_state_terminal(GrantState value) noexcept {
  return value == GrantState::Fenced || value == GrantState::Expired || value == GrantState::Revoked;
}

bool grant_state_live(GrantState value) noexcept { return !grant_state_terminal(value); }

bool grant_transition_allowed(GrantState from, GrantState to) noexcept {
  if (from == to) return false;
  if (grant_state_terminal(from)) return false;
  switch (from) {
    case GrantState::Issued:
      return to == GrantState::Acknowledged || to == GrantState::Applied || to == GrantState::Fenced ||
             to == GrantState::Expired || to == GrantState::Revoked;
    case GrantState::Acknowledged:
      return to == GrantState::Applied || to == GrantState::Fenced || to == GrantState::Expired ||
             to == GrantState::Revoked;
    case GrantState::Applied:
      return to == GrantState::Fenced || to == GrantState::Expired || to == GrantState::Revoked;
    case GrantState::Fenced:
    case GrantState::Expired:
    case GrantState::Revoked:
      return false;
  }
  return false;
}

bool is_valid(FenceReason value) noexcept {
  switch (value) {
    case FenceReason::TopologyChange:
    case FenceReason::CapacityChange:
    case FenceReason::PolicyChange:
    case FenceReason::ContractChange:
    case FenceReason::CoordinatorTermAdvance:
    case FenceReason::BootAdvance:
    case FenceReason::Expiry:
    case FenceReason::Revocation:
    case FenceReason::SessionLoss:
    case FenceReason::Manual:
    case FenceReason::AuthorityUnprovable:
    case FenceReason::Restored:
      return true;
  }
  return false;
}

std::string_view to_string(FenceReason value) noexcept {
  switch (value) {
    case FenceReason::TopologyChange: return "TOPOLOGY_CHANGE";
    case FenceReason::CapacityChange: return "CAPACITY_CHANGE";
    case FenceReason::PolicyChange: return "POLICY_CHANGE";
    case FenceReason::ContractChange: return "CONTRACT_CHANGE";
    case FenceReason::CoordinatorTermAdvance: return "COORDINATOR_TERM_ADVANCE";
    case FenceReason::BootAdvance: return "BOOT_ADVANCE";
    case FenceReason::Expiry: return "EXPIRY";
    case FenceReason::Revocation: return "REVOCATION";
    case FenceReason::SessionLoss: return "SESSION_LOSS";
    case FenceReason::Manual: return "MANUAL";
    case FenceReason::AuthorityUnprovable: return "AUTHORITY_UNPROVABLE";
    case FenceReason::Restored: return "RESTORED";
  }
  return "UNRECOGNISED";
}

FenceReason fence_reason_for(AuthorityDelta delta) noexcept {
  if (has_flag(delta, AuthorityDelta::Boot)) return FenceReason::BootAdvance;
  if (has_flag(delta, AuthorityDelta::CoordinatorTerm)) return FenceReason::CoordinatorTermAdvance;
  if (has_flag(delta, AuthorityDelta::Fabric)) return FenceReason::TopologyChange;
  if (has_flag(delta, AuthorityDelta::Capacity)) return FenceReason::CapacityChange;
  if (has_flag(delta, AuthorityDelta::Policy)) return FenceReason::PolicyChange;
  if (has_flag(delta, AuthorityDelta::Contract) ||
      has_flag(delta, AuthorityDelta::ContractGeneration) ||
      has_flag(delta, AuthorityDelta::Subject) ||
      has_flag(delta, AuthorityDelta::SubjectGeneration) ||
      has_flag(delta, AuthorityDelta::Scope)) {
    return FenceReason::ContractChange;
  }
  if (has_flag(delta, AuthorityDelta::Evidence)) return FenceReason::AuthorityUnprovable;
  return FenceReason::Manual;
}

Status FenceRecord::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "fence identity is unset");
  if (!grant.valid()) return Status(ErrorCode::InvalidArgument, "fence grant is unset");
  if (!is_valid(reason)) return Status(ErrorCode::Invalid, "fence reason is invalid");
  return Status{};
}

std::uint64_t FenceRecord::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(grant.value());
  digest.update_u16(static_cast<std::uint16_t>(reason));
  digest.update_u32(static_cast<std::uint32_t>(delta));
  digest.update_u64(prior.digest());
  digest.update_u64(current.digest());
  digest.update_u64(fenced_tick.value);
  return digest.value();
}

std::string FenceRecord::render() const {
  std::string out = "fence=";
  out.append(id.to_string());
  out.append(" grant=");
  out.append(grant.to_string());
  out.append(" reason=");
  out.append(to_string(reason));
  out.append(" delta=");
  out.append(to_string(delta));
  out.append(" at=");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(fenced_tick.value));
  out.append(buffer);
  return out;
}

}  // namespace dmf
