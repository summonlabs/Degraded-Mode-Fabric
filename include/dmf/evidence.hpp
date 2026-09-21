// Degraded Mode Fabric - evidence and observed fabric capability.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Evidence is observation, never authority. Absence of evidence is UNKNOWN, not
// healthy. Contradictory evidence is CONFLICT, not a majority vote.
#ifndef DMF_EVIDENCE_HPP
#define DMF_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dmf/core.hpp"
#include "dmf/guarantee.hpp"
#include "dmf/ids.hpp"
#include "dmf/reason.hpp"

namespace dmf {

/// What a publisher may report about a scope. Every kind other than
/// FabricTopology is a scalar in the unit named by its matching guarantee kind.
enum class EvidenceKind : std::uint16_t {
  FabricTopology = 1,        ///< FabricTopologyClass
  AggregateBandwidth = 2,    ///< kbps
  RoundTripLatency = 3,      ///< us
  PathDiversity = 4,         ///< count
  Reachability = 5,          ///< ppm of the service window
  NodeCoverage = 6,          ///< ppm of addressed endpoints
  SynchronousDurability = 7, ///< 0 or 1
};

bool is_valid(EvidenceKind value) noexcept;
std::string_view to_string(EvidenceKind value) noexcept;
std::uint64_t max_value_for(EvidenceKind kind) noexcept;

enum class FabricTopologyClass : std::uint16_t {
  FullMesh = 1,
  PartialMesh = 2,
  Partitioned = 3,
  Isolated = 4,
};

bool is_valid(FabricTopologyClass value) noexcept;
std::string_view to_string(FabricTopologyClass value) noexcept;

/// Explicit evidence health state. Never collapsed into a boolean.
enum class EvidenceState : std::uint16_t {
  Known = 1,
  Unknown = 2,
  Stale = 3,
  Conflict = 4,
  Invalid = 5,
  Unsupported = 6,
};

bool is_valid(EvidenceState value) noexcept;
std::string_view to_string(EvidenceState value) noexcept;
/// Ordering used by the aggregation lattice: higher is worse. Known is 0.
std::uint8_t evidence_severity(EvidenceState value) noexcept;
/// True only for Known.
bool evidence_is_authoritative(EvidenceState value) noexcept;

struct EvidenceItem {
  EvidenceId id{};
  PublisherId publisher{};
  /// Boot incarnation of the publishing process. Evidence published by a
  /// previous incarnation of the same publisher is never current.
  BootIncarnation publisher_boot{};
  EvidenceGeneration generation{};
  EvidenceKind kind = EvidenceKind::FabricTopology;
  ScopeId scope{};
  EvidenceState state = EvidenceState::Unknown;
  std::uint64_t value = 0;
  Tick observed_tick{};
  OriginClass origin = OriginClass::Unspecified;

  /// Structural and domain validation. The identity is deliberately not
  /// required: a publisher reports an observation and the coordinator mints the
  /// identity from its own monotone space, so an observation can never choose
  /// the identity it is stored under.
  [[nodiscard]] Status validate() const;
  /// True when the item is valid and carries an identity, which is the shape a
  /// retained observation must have.
  [[nodiscard]] bool identified() const noexcept;
  friend bool operator==(const EvidenceItem& a, const EvidenceItem& b) noexcept;
  [[nodiscard]] std::string render() const;
};

/// Bounded evidence vector with canonical ordering and conflict detection.
class EvidenceVector {
 public:
  EvidenceVector() = default;
  explicit EvidenceVector(std::size_t limit) noexcept : limit_(limit) {}

  [[nodiscard]] Status push(const EvidenceItem& item);
  [[nodiscard]] Status normalize();
  [[nodiscard]] Status validate() const;

  [[nodiscard]] const std::vector<EvidenceItem>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

  /// Aggregated health for one (scope, kind) group after superseding and
  /// conflict detection.
  [[nodiscard]] EvidenceState state_for(ScopeId scope, EvidenceKind kind) const noexcept;
  /// Value for one (scope, kind) group. Present only when the group is Known.
  [[nodiscard]] std::optional<std::uint64_t> value_for(ScopeId scope, EvidenceKind kind) const noexcept;
  /// Newest observation tick contributing to one group.
  [[nodiscard]] Tick newest_tick_for(ScopeId scope, EvidenceKind kind) const noexcept;
  /// Aggregate over every group in the vector. Empty means Unknown.
  [[nodiscard]] EvidenceState aggregate_state() const noexcept;
  /// Worst origin class present, used for REAL/SYNTHETIC labelling.
  [[nodiscard]] OriginClass origin_class() const noexcept;

  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;

 private:
  std::vector<EvidenceItem> items_{};
  std::size_t limit_ = kMaxCollectionItems;
};

/// Freshness window applied when evidence is converted into capability.
struct FreshnessWindow {
  Tick now{};
  std::uint64_t budget_ticks = 0;
  /// When false the window is ignored, which is only legitimate for replay of a
  /// recorded decision at its original tick.
  bool enforced = true;

  [[nodiscard]] bool fresh(Tick observed) const noexcept;
};

/// Observed capability of one scope. Only became available because an adjacent
/// fabric observer published evidence; the runtime does not measure the fabric.
struct CapabilitySnapshot {
  ScopeId scope{};
  CapacityGeneration generation{};
  EvidenceState state = EvidenceState::Unknown;
  FabricTopologyClass topology = FabricTopologyClass::FullMesh;
  std::uint64_t bandwidth_kbps = 0;
  std::uint64_t rtt_p99_us = 0;
  std::uint32_t path_diversity = 0;
  std::uint32_t reachable_ppm = 0;
  std::uint32_t node_coverage_ppm = 0;
  bool synchronous_durability = false;
  Tick observed_tick{};
  OriginClass origin = OriginClass::Unspecified;

  /// True only when every required evidence group is Known and fresh.
  [[nodiscard]] bool usable() const noexcept { return state == EvidenceState::Known; }
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
};

/// Builds a capability snapshot from evidence. Fails closed: a missing,
/// unknown, stale, conflicting, invalid or unsupported evidence group degrades
/// the whole snapshot to that state, and no optimistic value is substituted.
[[nodiscard]] Result<CapabilitySnapshot> derive_capability(const EvidenceVector& evidence,
                                                           ScopeId scope,
                                                           CapacityGeneration generation,
                                                           const FreshnessWindow& freshness) noexcept;

/// Support classification of one guarantee against observed capability.
[[nodiscard]] SupportTri supports(const CapabilitySnapshot& capability,
                                  const Guarantee& guarantee) noexcept;

/// Reason code describing why a guarantee is not supported.
[[nodiscard]] ReasonCode shortfall_reason(const CapabilitySnapshot& capability,
                                          const Guarantee& guarantee) noexcept;

}  // namespace dmf

#endif  // DMF_EVIDENCE_HPP
