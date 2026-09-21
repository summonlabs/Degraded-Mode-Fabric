// Degraded Mode Fabric - authority vector, bindings and the authority ladder.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Authority-bearing state is only legal while every generation it was derived
// from is unchanged. Observation is not authority, eligibility is not
// authorisation, authorisation is not application, and acknowledgement is not
// verified effect.
#ifndef DMF_AUTHORITY_HPP
#define DMF_AUTHORITY_HPP

#include <cstdint>
#include <string>

#include "dmf/core.hpp"
#include "dmf/ids.hpp"

namespace dmf {

/// Identity of one running process incarnation. A durable store advances the
/// boot incarnation on every restart, so records written by an earlier
/// incarnation can never be mistaken for current authority.
struct Incarnation {
  ProcessId process{};
  BootIncarnation boot{};
  Tick boot_tick{};

  [[nodiscard]] bool valid() const noexcept { return process.valid() && boot.valid(); }
  friend bool operator==(const Incarnation& a, const Incarnation& b) noexcept {
    return a.process == b.process && a.boot == b.boot;
  }
  friend bool operator!=(const Incarnation& a, const Incarnation& b) noexcept { return !(a == b); }
};

/// Every generation that can invalidate previously issued degraded authority.
struct AuthorityVector {
  CoordinatorTerm coordinator_term{};
  BootIncarnation boot{};
  FabricGeneration fabric{};
  CapacityGeneration capacity{};
  PolicyGeneration policy{};
  EvidenceGeneration evidence{};

  [[nodiscard]] bool valid() const noexcept {
    return coordinator_term.valid() && boot.valid() && fabric.valid() && capacity.valid() &&
           policy.valid() && evidence.valid();
  }
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;

  friend bool operator==(const AuthorityVector& a, const AuthorityVector& b) noexcept {
    return a.coordinator_term == b.coordinator_term && a.boot == b.boot && a.fabric == b.fabric &&
           a.capacity == b.capacity && a.policy == b.policy && a.evidence == b.evidence;
  }
  friend bool operator!=(const AuthorityVector& a, const AuthorityVector& b) noexcept {
    return !(a == b);
  }
};

/// Which components of an authority vector differ between two bindings.
enum class AuthorityDelta : std::uint32_t {
  None = 0,
  CoordinatorTerm = 1U << 0,
  Boot = 1U << 1,
  Fabric = 1U << 2,
  Capacity = 1U << 3,
  Policy = 1U << 4,
  Evidence = 1U << 5,
  Contract = 1U << 6,
  ContractGeneration = 1U << 7,
  Subject = 1U << 8,
  SubjectGeneration = 1U << 9,
  Scope = 1U << 10,
};

constexpr AuthorityDelta operator|(AuthorityDelta a, AuthorityDelta b) noexcept {
  return static_cast<AuthorityDelta>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr AuthorityDelta operator&(AuthorityDelta a, AuthorityDelta b) noexcept {
  return static_cast<AuthorityDelta>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool has_flag(AuthorityDelta value, AuthorityDelta flag) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}
/// True when the delta invalidates authority (every flag except None).
constexpr bool invalidates_authority(AuthorityDelta value) noexcept {
  return static_cast<std::uint32_t>(value) != 0U;
}
std::string to_string(AuthorityDelta value);

/// Full binding of one decision or grant: the authority vector plus the exact
/// subject the authority applies to. Matching identity is not matching
/// generation, so both are bound and both are compared.
struct AuthorityBinding {
  AuthorityVector vector{};
  ContractId contract{};
  ContractGeneration contract_generation{};
  SubjectId subject{};
  SubjectGeneration subject_generation{};
  ScopeId scope{};

  [[nodiscard]] bool valid() const noexcept {
    return vector.valid() && contract.valid() && contract_generation.valid() && subject.valid() &&
           subject_generation.valid() && scope.valid();
  }
  [[nodiscard]] AuthorityDelta classify(const AuthorityBinding& other) const noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;

  friend bool operator==(const AuthorityBinding& a, const AuthorityBinding& b) noexcept {
    return a.vector == b.vector && a.contract == b.contract &&
           a.contract_generation == b.contract_generation && a.subject == b.subject &&
           a.subject_generation == b.subject_generation && a.scope == b.scope;
  }
  friend bool operator!=(const AuthorityBinding& a, const AuthorityBinding& b) noexcept {
    return !(a == b);
  }
};

/// How far up the authority ladder a piece of state has actually climbed.
enum class AuthorityLevel : std::uint16_t {
  None = 0,
  Observed = 1,       ///< we hold an observation; not authority
  Eligible = 2,       ///< the subject could be degraded; not authorisation
  Recommended = 3,    ///< policy recommends; not authorisation
  Authorized = 4,     ///< committed degraded authority exists
  Acknowledged = 5,   ///< the subject claims to have accepted it; not effect
  Applied = 6,        ///< effect verified against evidence
};

bool is_valid(AuthorityLevel value) noexcept;
std::string_view to_string(AuthorityLevel value) noexcept;
/// True only at Authorized and above.
bool is_authority(AuthorityLevel value) noexcept;
/// True only at Applied.
bool is_verified_effect(AuthorityLevel value) noexcept;

/// Lifecycle of a degraded authority grant. Issued, Acknowledged and Applied
/// are live states; Fenced, Expired and Revoked are terminal.
enum class GrantState : std::uint16_t {
  Issued = 1,
  Acknowledged = 2,
  Applied = 3,
  Fenced = 4,
  Expired = 5,
  Revoked = 6,
};

bool is_valid(GrantState value) noexcept;
std::string_view to_string(GrantState value) noexcept;
bool grant_state_terminal(GrantState value) noexcept;
bool grant_state_live(GrantState value) noexcept;
/// Legal transitions are strictly forward; anything else is invalid.
bool grant_transition_allowed(GrantState from, GrantState to) noexcept;

/// Why degraded authority stopped being valid.
enum class FenceReason : std::uint16_t {
  TopologyChange = 1,
  CapacityChange = 2,
  PolicyChange = 3,
  ContractChange = 4,
  CoordinatorTermAdvance = 5,
  BootAdvance = 6,
  Expiry = 7,
  Revocation = 8,
  SessionLoss = 9,
  Manual = 10,
  AuthorityUnprovable = 11,
  Restored = 12,
};

bool is_valid(FenceReason value) noexcept;
std::string_view to_string(FenceReason value) noexcept;
/// Maps an authority delta onto the fence reason that must be recorded.
FenceReason fence_reason_for(AuthorityDelta delta) noexcept;

/// Durable record that a grant is no longer valid.
struct FenceRecord {
  FenceId id{};
  GrantId grant{};
  FenceReason reason = FenceReason::Manual;
  AuthorityDelta delta = AuthorityDelta::None;
  AuthorityBinding prior{};
  AuthorityBinding current{};
  Tick fenced_tick{};

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
};

}  // namespace dmf

#endif  // DMF_AUTHORITY_HPP
