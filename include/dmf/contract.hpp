// Degraded Mode Fabric - service obligations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A contract is the runtime's only source of truth for what a service is owed.
// The runtime does not invent obligations, does not create capacity and does not
// route traffic; it decides which subset of these obligations remains legally
// supportable under observed fabric capability.
#ifndef DMF_CONTRACT_HPP
#define DMF_CONTRACT_HPP

#include <cstdint>
#include <string>

#include "dmf/core.hpp"
#include "dmf/guarantee.hpp"
#include "dmf/ids.hpp"

namespace dmf {

inline constexpr std::uint32_t kMaxServicePriority = 1000000U;

/// A registered service obligation set for one subject on one scope.
struct ServiceContract {
  ContractId id{};
  ContractGeneration generation{};
  ScopeId scope{};
  SubjectId subject{};
  SubjectGeneration subject_generation{};
  ServiceClass service_class = ServiceClass::Standard;
  /// Explicit protection marker. Protected service classes are always treated as
  /// non-degradable even when this flag is false; setting it makes the
  /// protection explicit and is required for non-protected classes that must
  /// never be weakened.
  bool non_degradable = false;
  /// Higher wins. Ties are always broken by identity so the order is total.
  std::uint32_t priority = 0;
  GuaranteeSet original{};
  GuaranteeEnvelope envelope{};
  Tick registered_tick{};
  bool active = true;

  [[nodiscard]] bool protected_obligation() const noexcept {
    return non_degradable || service_class == ServiceClass::Protected;
  }
  /// True when the envelope authorises no weakening at all.
  [[nodiscard]] bool frozen() const noexcept { return envelope.max_concessions() == 0; }
  /// Lowest bandwidth the contract may be served at before it must be refused.
  [[nodiscard]] std::uint64_t bandwidth_floor() const noexcept;
  /// Original bandwidth demand, or 0 when the contract carries no bandwidth
  /// guarantee. Only the bandwidth guarantee is treated as an additive,
  /// contended fabric resource.
  [[nodiscard]] std::uint64_t bandwidth_demand() const noexcept;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
};

/// Deterministic total order for allocation: most protected class first, then
/// higher priority, then lower identity. Independent of container order.
bool contract_allocation_precedes(const ServiceContract& a, const ServiceContract& b) noexcept;

}  // namespace dmf

#endif  // DMF_CONTRACT_HPP
