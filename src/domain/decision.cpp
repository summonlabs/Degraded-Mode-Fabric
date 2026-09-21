// Degraded Mode Fabric - decision rendering and validation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/decision.hpp"

#include <algorithm>
#include <cstdio>

namespace dmf {

bool operator==(const AllocationEntry& a, const AllocationEntry& b) noexcept {
  return a.contract == b.contract && a.contract_generation == b.contract_generation &&
         a.scope == b.scope && a.admitted == b.admitted && a.full == b.full &&
         a.approved == b.approved && a.allocated_bandwidth_kbps == b.allocated_bandwidth_kbps &&
         a.outcome == b.outcome && a.reason == b.reason;
}

std::string AllocationEntry::render() const {
  std::string out = "contract=";
  out.append(contract.to_string());
  out.push_back('/');
  out.append(contract_generation.to_string());
  out.append(" outcome=");
  out.append(to_string(outcome));
  if (admitted) out.append(full ? " full" : " degraded");
  out.append(" bw=");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(allocated_bandwidth_kbps));
  out.append(buffer);
  out.append(" approved=");
  out.append(approved.render());
  if (!delta.empty()) {
    out.append(" delta[");
    out.append(delta.render());
    out.push_back(']');
  }
  return out;
}

std::string InfeasibilityCertificate::render() const {
  if (!valid) return "no certificate";
  char buffer[192];
  std::snprintf(buffer, sizeof(buffer),
                "certificate{required=%u available=%u minimum_demand=%llu kbps capacity=%llu kbps}",
                required_protected, available_protected,
                static_cast<unsigned long long>(minimum_demand_kbps),
                static_cast<unsigned long long>(offered_capacity_kbps));
  return std::string(buffer);
}

const AllocationEntry* AllocationPlan::find(ContractId contract) const noexcept {
  for (const AllocationEntry& entry : entries) {
    if (entry.contract == contract) return &entry;
  }
  return nullptr;
}

std::uint64_t AllocationPlan::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(authority.digest());
  digest.update_u64(scope.value());
  digest.update_u64(capacity_generation.value());
  digest.update_u16(static_cast<std::uint16_t>(status));
  digest.update_u64(available_bandwidth_kbps);
  digest.update_u64(allocated_bandwidth_kbps);
  digest.update_u32(static_cast<std::uint32_t>(entries.size()));
  for (const AllocationEntry& entry : entries) {
    digest.update_u64(entry.contract.value());
    digest.update_u64(entry.contract_generation.value());
    digest.update_bool(entry.admitted);
    digest.update_bool(entry.full);
    digest.update_u64(entry.approved.digest());
    digest.update_u64(entry.allocated_bandwidth_kbps);
    digest.update_u16(static_cast<std::uint16_t>(entry.outcome));
    digest.update_u16(static_cast<std::uint16_t>(entry.reason));
  }
  digest.update_bool(certificate.valid);
  digest.update_u32(certificate.required_protected);
  digest.update_u32(certificate.available_protected);
  digest.update_u64(certificate.minimum_demand_kbps);
  digest.update_u64(certificate.offered_capacity_kbps);
  digest.update_bool(protected_shortfall);
  return digest.value();
}

std::string AllocationPlan::render() const {
  std::string out = "plan=";
  out.append(id.to_string());
  out.append(" scope=");
  out.append(scope.to_string());
  out.append(" status=");
  out.append(to_string(status));
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), " bandwidth=%llu/%llu nodes=%llu pruned=%llu budget=%llu%s",
                static_cast<unsigned long long>(allocated_bandwidth_kbps),
                static_cast<unsigned long long>(available_bandwidth_kbps),
                static_cast<unsigned long long>(work.nodes_visited),
                static_cast<unsigned long long>(work.nodes_pruned),
                static_cast<unsigned long long>(work.budget),
                work.budget_exhausted ? " (budget exhausted)" : "");
  out.append(buffer);
  if (protected_shortfall) out.append(" protected-shortfall");
  if (certificate.valid) {
    out.append(" ");
    out.append(certificate.render());
  }
  return out;
}

Status Decision::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "decision identity is unset");
  if (!binding.valid()) return Status(ErrorCode::InvalidArgument, "decision binding is unset");
  if (!is_valid(outcome)) return Status(ErrorCode::Invalid, "decision outcome is invalid");
  if (!is_valid(plan_status)) return Status(ErrorCode::Invalid, "decision plan status is invalid");
  if (!is_valid(evidence_state)) return Status(ErrorCode::Invalid, "decision evidence state is invalid");
  Status status = original.validate();
  if (!status.ok()) return status;
  status = approved.validate();
  if (!status.ok()) return status;
  const WeakeningResult relation = classify_weakening(original, approved);
  if (relation != WeakeningResult::WeakensOrEquals) {
    return Status(ErrorCode::Invalid,
                  std::string("decision approved set is not a weakening: ") +
                      std::string(to_string(relation)));
  }
  if (is_authorising(outcome)) {
    if (static_cast<std::uint16_t>(max_authority) <
        static_cast<std::uint16_t>(AuthorityLevel::Recommended)) {
      return Status(ErrorCode::InvalidState, "an authorising decision carries no recommendation");
    }
    if (!evidence_is_authoritative(evidence_state) && outcome == DecisionOutcome::Full) {
      return Status(ErrorCode::InvalidState,
                    "full-service decision rests on non-authoritative evidence");
    }
  }
  if (!is_authorising(outcome) && !delta.empty()) {
    return Status(ErrorCode::InvalidState, "non-authorising decision carries concessions");
  }
  return Status{};
}

std::uint64_t Decision::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(binding.digest());
  digest.update_u16(static_cast<std::uint16_t>(service_class));
  digest.update_bool(protected_obligation);
  digest.update_u32(priority);
  digest.update_u16(static_cast<std::uint16_t>(outcome));
  digest.update_u16(static_cast<std::uint16_t>(max_authority));
  digest.update_u64(original.digest());
  digest.update_u64(approved.digest());
  digest.update_u64(delta.digest());
  digest.update_u16(static_cast<std::uint16_t>(primary_reason));
  digest.update_u32(static_cast<std::uint32_t>(reasons.size()));
  for (const ReasonCode reason : reasons) {
    digest.update_u16(static_cast<std::uint16_t>(reason));
  }
  digest.update_u64(evidence.digest());
  digest.update_u16(static_cast<std::uint16_t>(evidence_state));
  digest.update_u16(static_cast<std::uint16_t>(plan_status));
  digest.update_bool(escalation_required);
  digest.update_u16(static_cast<std::uint16_t>(provenance));
  digest.update_u64(decided_tick.value);
  digest.update_u64(expires_tick.value);
  return digest.value();
}

std::string Decision::reason_list() const {
  std::string out;
  for (const ReasonCode reason : reasons) {
    if (reason == ReasonCode::None) continue;
    if (!out.empty()) out.push_back(',');
    out.append(to_string(reason));
  }
  return out;
}

std::string Decision::explain() const {
  std::string out;
  out.reserve(512);
  out.append("decision=");
  out.append(id.to_string());
  out.append(" outcome=");
  out.append(to_string(outcome));
  out.append(" authority=");
  out.append(to_string(max_authority));
  out.append(" subject=");
  out.append(binding.subject.to_string());
  out.push_back('/');
  out.append(binding.subject_generation.to_string());
  out.append(" contract=");
  out.append(binding.contract.to_string());
  out.push_back('/');
  out.append(binding.contract_generation.to_string());
  out.append(" scope=");
  out.append(binding.scope.to_string());
  out.append(" class=");
  out.append(to_string(service_class));
  if (protected_obligation) out.append(" protected");
  out.append("\n  original=");
  out.append(original.render());
  out.append("\n  approved=");
  out.append(approved.render());
  out.append("\n  delta=");
  out.append(delta.render());
  out.append("\n  reason=");
  out.append(to_string(primary_reason));
  const std::string reason_text = reason_list();
  if (!reason_text.empty()) {
    out.append(" (");
    out.append(reason_text);
    out.push_back(')');
  }
  out.append("\n  evidence=");
  out.append(to_string(evidence_state));
  out.append(" provenance=");
  out.append(to_string(provenance));
  out.append(" plan=");
  out.append(to_string(plan_status));
  if (escalation_required) out.append(" escalation=required");
  out.append("\n  bound=");
  out.append(binding.vector.render());
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "\n  at=%llu expires=%llu",
                static_cast<unsigned long long>(decided_tick.value),
                static_cast<unsigned long long>(expires_tick.value));
  out.append(buffer);
  if (out.size() > kMaxExplanationBytes) out.resize(kMaxExplanationBytes);
  return out;
}

}  // namespace dmf
