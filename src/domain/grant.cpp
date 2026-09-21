// Degraded Mode Fabric - grant lifecycle and restoration rules.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/grant.hpp"

#include <cstdio>

namespace dmf {

bool is_valid(PreconditionKind value) noexcept {
  switch (value) {
    case PreconditionKind::EvidenceKnown:
    case PreconditionKind::CapabilitySupports:
    case PreconditionKind::NoActiveFence:
    case PreconditionKind::PolicyPermits:
    case PreconditionKind::ContractUnchanged:
    case PreconditionKind::MinimumDwell:
    case PreconditionKind::ScopeRecovered:
    case PreconditionKind::AuthorityUnchanged:
    case PreconditionKind::EscalationClear:
      return true;
  }
  return false;
}

std::string_view to_string(PreconditionKind value) noexcept {
  switch (value) {
    case PreconditionKind::EvidenceKnown: return "EVIDENCE_KNOWN";
    case PreconditionKind::CapabilitySupports: return "CAPABILITY_SUPPORTS";
    case PreconditionKind::NoActiveFence: return "NO_ACTIVE_FENCE";
    case PreconditionKind::PolicyPermits: return "POLICY_PERMITS";
    case PreconditionKind::ContractUnchanged: return "CONTRACT_UNCHANGED";
    case PreconditionKind::MinimumDwell: return "MINIMUM_DWELL";
    case PreconditionKind::ScopeRecovered: return "SCOPE_RECOVERED";
    case PreconditionKind::AuthorityUnchanged: return "AUTHORITY_UNCHANGED";
    case PreconditionKind::EscalationClear: return "ESCALATION_CLEAR";
  }
  return "UNRECOGNISED";
}

bool operator==(const RestorationPrecondition& a, const RestorationPrecondition& b) noexcept {
  return a.kind == b.kind && a.guarantee == b.guarantee && a.scope == b.scope &&
         a.required_value == b.required_value && a.dwell_ticks == b.dwell_ticks;
}

std::string RestorationPrecondition::render() const {
  std::string out(to_string(kind));
  out.push_back('(');
  out.append(to_string(guarantee));
  out.append(" scope=");
  out.append(scope.to_string());
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), " required=%llu dwell=%llu",
                static_cast<unsigned long long>(required_value),
                static_cast<unsigned long long>(dwell_ticks));
  out.append(buffer);
  out.push_back(')');
  return out;
}

Status Grant::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "grant identity is unset");
  if (!decision.valid()) return Status(ErrorCode::InvalidArgument, "grant decision is unset");
  if (!binding.valid()) return Status(ErrorCode::InvalidArgument, "grant binding is unset");
  if (!is_valid(service_class)) return Status(ErrorCode::Invalid, "grant service class is invalid");
  if (!is_valid(state)) return Status(ErrorCode::Invalid, "grant state is invalid");
  if (!is_valid(evidence_state)) return Status(ErrorCode::Invalid, "grant evidence state is invalid");
  if (!is_valid(provenance)) return Status(ErrorCode::Invalid, "grant provenance is invalid");
  if (!sequence.valid()) return Status(ErrorCode::InvalidArgument, "grant sequence is unset");
  if (expires_tick < issued_tick) {
    return Status(ErrorCode::Invalid, "grant expires before it was issued");
  }
  Status status = original.validate();
  if (!status.ok()) return status;
  status = degraded.validate();
  if (!status.ok()) return status;
  const WeakeningResult relation = classify_weakening(original, degraded);
  if (relation != WeakeningResult::WeakensOrEquals) {
    return Status(ErrorCode::Invalid, std::string("grant degraded set is not a weakening: ") +
                                          std::string(to_string(relation)));
  }
  if (protected_obligation && !delta.empty()) {
    return Status(ErrorCode::InvalidState, "protected obligation carries concessions");
  }
  return Status{};
}

AuthorityLevel Grant::authority_level() const noexcept {
  switch (state) {
    case GrantState::Issued: return AuthorityLevel::Authorized;
    case GrantState::Acknowledged: return AuthorityLevel::Acknowledged;
    case GrantState::Applied: return AuthorityLevel::Applied;
    case GrantState::Fenced:
    case GrantState::Expired:
    case GrantState::Revoked:
      return AuthorityLevel::None;
  }
  return AuthorityLevel::None;
}

bool Grant::expired_at(Tick now) const noexcept { return now > expires_tick; }

std::uint64_t Grant::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(decision.value());
  digest.update_u64(binding.digest());
  digest.update_u16(static_cast<std::uint16_t>(service_class));
  digest.update_bool(protected_obligation);
  digest.update_u64(original.digest());
  digest.update_u64(degraded.digest());
  digest.update_u64(delta.digest());
  digest.update_u16(static_cast<std::uint16_t>(reason));
  digest.update_u64(evidence.digest());
  digest.update_u16(static_cast<std::uint16_t>(evidence_state));
  digest.update_u64(sequence.value());
  digest.update_u64(last_attempt.value());
  digest.update_u64(issued_tick.value);
  digest.update_u64(expires_tick.value);
  digest.update_u64(last_transition_tick.value);
  digest.update_u16(static_cast<std::uint16_t>(state));
  digest.update_u16(static_cast<std::uint16_t>(provenance));
  digest.update_u32(static_cast<std::uint32_t>(preconditions.size()));
  for (const RestorationPrecondition& precondition : preconditions) {
    digest.update_u16(static_cast<std::uint16_t>(precondition.kind));
    digest.update_u16(static_cast<std::uint16_t>(precondition.guarantee));
    digest.update_u64(precondition.scope.value());
    digest.update_u64(precondition.required_value);
    digest.update_u64(precondition.dwell_ticks);
  }
  return digest.value();
}

std::string Grant::render() const {
  std::string out = "grant=";
  out.append(id.to_string());
  out.append(" subject=");
  out.append(binding.subject.to_string());
  out.append(" contract=");
  out.append(binding.contract.to_string());
  out.push_back('/');
  out.append(binding.contract_generation.to_string());
  out.append(" scope=");
  out.append(binding.scope.to_string());
  out.append(" state=");
  out.append(to_string(state));
  out.append(" degraded=");
  out.append(degraded.render());
  out.append(" seq=");
  out.append(sequence.to_string());
  return out;
}

std::string Grant::explain() const {
  std::string out;
  out.append("grant=");
  out.append(id.to_string());
  out.append(" decision=");
  out.append(decision.to_string());
  out.append(" state=");
  out.append(to_string(state));
  out.append(" authority=");
  out.append(to_string(authority_level()));
  out.append("\n  original=");
  out.append(original.render());
  out.append("\n  degraded=");
  out.append(degraded.render());
  out.append("\n  delta=");
  out.append(delta.render());
  out.append("\n  reason=");
  out.append(to_string(reason));
  out.append(" evidence=");
  out.append(to_string(evidence_state));
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "\n  issued=%llu expires=%llu sequence=%llu attempts=%llu",
                static_cast<unsigned long long>(issued_tick.value),
                static_cast<unsigned long long>(expires_tick.value),
                static_cast<unsigned long long>(sequence.value()),
                static_cast<unsigned long long>(last_attempt.value()));
  out.append(buffer);
  out.append("\n  bound=");
  out.append(binding.vector.render());
  if (!preconditions.empty()) {
    out.append("\n  restoration requires:");
    for (const RestorationPrecondition& precondition : preconditions) {
      out.append("\n    ");
      out.append(precondition.render());
    }
  }
  if (out.size() > kMaxExplanationBytes) out.resize(kMaxExplanationBytes);
  return out;
}

bool is_valid(PreconditionResult value) noexcept {
  switch (value) {
    case PreconditionResult::Satisfied:
    case PreconditionResult::NotSatisfied:
    case PreconditionResult::Indeterminate:
    case PreconditionResult::Unsupported:
      return true;
  }
  return false;
}

std::string_view to_string(PreconditionResult value) noexcept {
  switch (value) {
    case PreconditionResult::Satisfied: return "SATISFIED";
    case PreconditionResult::NotSatisfied: return "NOT_SATISFIED";
    case PreconditionResult::Indeterminate: return "INDETERMINATE";
    case PreconditionResult::Unsupported: return "UNSUPPORTED";
  }
  return "UNRECOGNISED";
}

std::string PreconditionEvaluation::render() const {
  std::string out(to_string(result));
  out.push_back(' ');
  out.append(precondition.render());
  out.append(" reason=");
  out.append(to_string(reason));
  return out;
}

Status RestorationEvaluation::validate() const {
  if (!is_valid(outcome)) return Status(ErrorCode::Invalid, "restoration outcome is invalid");
  if (!grant.valid()) return Status(ErrorCode::InvalidArgument, "restoration grant is unset");
  if (!binding.valid()) return Status(ErrorCode::InvalidArgument, "restoration binding is unset");
  if (!is_valid(evidence_state)) {
    return Status(ErrorCode::Invalid, "restoration evidence state is invalid");
  }
  Status status = original.validate();
  if (!status.ok()) return status;
  status = degraded.validate();
  if (!status.ok()) return status;
  if (!weakens_or_equals(original, degraded)) {
    return Status(ErrorCode::InvalidState, "restoration degraded set is not a weakening");
  }
  for (const PreconditionEvaluation& evaluation : preconditions) {
    if (!is_valid(evaluation.result)) {
      return Status(ErrorCode::Invalid, "precondition evaluation result is invalid");
    }
  }
  if (outcome == RestorationOutcome::Proven) {
    if (!evidence_is_authoritative(evidence_state)) {
      return Status(ErrorCode::InvalidState,
                    "restoration proven on non-authoritative evidence");
    }
    for (const PreconditionEvaluation& evaluation : preconditions) {
      if (evaluation.result != PreconditionResult::Satisfied) {
        return Status(ErrorCode::InvalidState, "restoration proven with an unsatisfied precondition");
      }
    }
  }
  return Status{};
}

std::uint64_t RestorationEvaluation::digest() const noexcept {
  Digest64 digest;
  digest.update_u16(static_cast<std::uint16_t>(outcome));
  digest.update_u64(grant.value());
  digest.update_u64(decision.value());
  digest.update_u64(binding.digest());
  digest.update_u64(original.digest());
  digest.update_u64(degraded.digest());
  digest.update_u32(static_cast<std::uint32_t>(preconditions.size()));
  for (const PreconditionEvaluation& evaluation : preconditions) {
    digest.update_u16(static_cast<std::uint16_t>(evaluation.precondition.kind));
    digest.update_u16(static_cast<std::uint16_t>(evaluation.precondition.guarantee));
    digest.update_u64(evaluation.precondition.scope.value());
    digest.update_u64(evaluation.precondition.required_value);
    digest.update_u64(evaluation.precondition.dwell_ticks);
    digest.update_u16(static_cast<std::uint16_t>(evaluation.result));
    digest.update_u16(static_cast<std::uint16_t>(evaluation.reason));
  }
  digest.update_u16(static_cast<std::uint16_t>(primary_reason));
  digest.update_u16(static_cast<std::uint16_t>(evidence_state));
  digest.update_u64(evaluated_tick.value);
  return digest.value();
}

std::string RestorationEvaluation::explain() const {
  std::string out;
  out.append("restoration=");
  out.append(to_string(outcome));
  out.append(" grant=");
  out.append(grant.to_string());
  out.append(" decision=");
  out.append(decision.to_string());
  out.append(" evidence=");
  out.append(to_string(evidence_state));
  out.append(" reason=");
  out.append(to_string(primary_reason));
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), " at=%llu",
                static_cast<unsigned long long>(evaluated_tick.value));
  out.append(buffer);
  for (const PreconditionEvaluation& evaluation : preconditions) {
    out.append("\n    ");
    out.append(evaluation.render());
  }
  if (out.size() > kMaxExplanationBytes) out.resize(kMaxExplanationBytes);
  return out;
}

}  // namespace dmf
