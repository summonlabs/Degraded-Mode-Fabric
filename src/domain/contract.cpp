// Degraded Mode Fabric - service obligation validation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/contract.hpp"

#include <cstdio>

namespace dmf {

std::uint64_t ServiceContract::bandwidth_floor() const noexcept {
  const auto demand = original.value_of(GuaranteeKind::Bandwidth);
  if (!demand.has_value()) return 0;
  return envelope.floor_for(GuaranteeKind::Bandwidth, *demand);
}

std::uint64_t ServiceContract::bandwidth_demand() const noexcept {
  const auto demand = original.value_of(GuaranteeKind::Bandwidth);
  return demand.has_value() ? *demand : 0;
}

Status ServiceContract::validate() const {
  if (!id.valid()) return Status(ErrorCode::InvalidArgument, "contract identity is unset");
  if (!generation.valid()) {
    return Status(ErrorCode::InvalidArgument, "contract generation is unset");
  }
  if (!scope.valid()) return Status(ErrorCode::InvalidArgument, "contract scope is unset");
  if (!subject.valid()) return Status(ErrorCode::InvalidArgument, "contract subject is unset");
  if (!subject_generation.valid()) {
    return Status(ErrorCode::InvalidArgument, "contract subject generation is unset");
  }
  if (!is_valid(service_class)) {
    return Status(ErrorCode::InvalidArgument, "contract service class is invalid");
  }
  if (priority > kMaxServicePriority) {
    return Status(ErrorCode::InvalidArgument, "contract priority is out of range");
  }
  if (original.empty()) {
    return Status(ErrorCode::InvalidArgument, "contract declares no guarantees");
  }
  Status status = original.validate();
  if (!status.ok()) return status;
  status = validate_envelope_against(envelope, original);
  if (!status.ok()) return status;
  if (protected_obligation() && !frozen()) {
    return Status(ErrorCode::Invalid,
                  "non-degradable contract carries a concession envelope");
  }
  if (protected_obligation() && service_class != ServiceClass::Protected && !non_degradable) {
    return Status(ErrorCode::Invalid, "protected obligation marker is inconsistent");
  }
  return Status{};
}

std::uint64_t ServiceContract::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(id.value());
  digest.update_u64(generation.value());
  digest.update_u64(scope.value());
  digest.update_u64(subject.value());
  digest.update_u64(subject_generation.value());
  digest.update_u16(static_cast<std::uint16_t>(service_class));
  digest.update_bool(non_degradable);
  digest.update_u32(priority);
  digest.update_u64(original.digest());
  digest.update_u64(envelope.digest());
  digest.update_bool(active);
  return digest.value();
}

std::string ServiceContract::render() const {
  std::string out = "contract=";
  out.append(id.to_string());
  out.append(" gen=");
  out.append(generation.to_string());
  out.append(" scope=");
  out.append(scope.to_string());
  out.append(" class=");
  out.append(to_string(service_class));
  if (protected_obligation()) out.append(" protected");
  out.append(" priority=");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%u", priority);
  out.append(buffer);
  out.append(" obligations=");
  out.append(original.render());
  if (!active) out.append(" inactive");
  return out;
}

bool contract_allocation_precedes(const ServiceContract& a, const ServiceContract& b) noexcept {
  const std::uint8_t rank_a = class_rank(a.service_class);
  const std::uint8_t rank_b = class_rank(b.service_class);
  if (rank_a != rank_b) return rank_a < rank_b;
  if (a.priority != b.priority) return a.priority > b.priority;
  return a.id < b.id;
}

}  // namespace dmf
