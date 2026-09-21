// Degraded Mode Fabric - service guarantees, concessions and weakening.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The product-defining invariant lives here: an approved (degraded) guarantee
// set must be a weakening of the original guarantee set. It may never add a
// guarantee, never strengthen a shared guarantee, and never silently drop one:
// every dropped guarantee is materialised as an explicit withdrawal.
#ifndef DMF_GUARANTEE_HPP
#define DMF_GUARANTEE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dmf/core.hpp"
#include "dmf/ids.hpp"
#include "dmf/reason.hpp"

namespace dmf {

/// Contractual service classes, ordered from most to least protected.
enum class ServiceClass : std::uint16_t {
  Protected = 1,
  Standard = 2,
  BestEffort = 3,
  Scavenger = 4,
};

bool is_valid(ServiceClass value) noexcept;
std::string_view to_string(ServiceClass value) noexcept;
/// 0 for the most protected class. Used as the deterministic precedence key
/// when deciding which traffic absorbs degradation.
std::uint8_t class_rank(ServiceClass value) noexcept;

/// Kinds of contractual guarantee the runtime reasons about.
enum class GuaranteeKind : std::uint16_t {
  Availability = 1,   ///< AtLeast, parts per million of the window
  Bandwidth = 2,      ///< AtLeast, kilobits per second
  LatencyP99 = 3,     ///< AtMost, microseconds
  PathDiversity = 4,  ///< AtLeast, count of independent paths
  Durability = 5,     ///< AtLeast, synchronous replication level 0..4
  Reachability = 6,   ///< AtLeast, parts per million of addressed endpoints
};

bool is_valid(GuaranteeKind value) noexcept;
std::string_view to_string(GuaranteeKind value) noexcept;
std::string_view guarantee_unit(GuaranteeKind value) noexcept;

enum class Comparator : std::uint16_t {
  AtLeast = 1,
  AtMost = 2,
};

bool is_valid(Comparator value) noexcept;
std::string_view to_string(Comparator value) noexcept;

/// The comparator is fixed by the guarantee kind; a mismatch is Invalid.
Comparator comparator_for(GuaranteeKind kind) noexcept;
/// Inclusive upper bound accepted for a value of this kind.
std::uint64_t max_value_for(GuaranteeKind kind) noexcept;

inline constexpr std::uint64_t kPpmScale = 1000000ULL;
inline constexpr std::uint64_t kMaxBandwidthKbps = 1000000000ULL;
inline constexpr std::uint64_t kMaxLatencyUs = 3600000000ULL;
inline constexpr std::uint64_t kMaxPathDiversity = 4096ULL;
inline constexpr std::uint64_t kMaxDurabilityLevel = 4ULL;

struct Guarantee {
  GuaranteeKind kind = GuaranteeKind::Availability;
  Comparator comparator = Comparator::AtLeast;
  std::uint64_t value = 0;
  /// A hard guarantee is a binding obligation. Weakening a hard guarantee to a
  /// soft one is a strengthening of the candidate and is rejected.
  bool hard = true;

  [[nodiscard]] bool valid() const noexcept;
  /// Deterministic total order key: kind, then comparator, then value.
  friend bool operator<(const Guarantee& a, const Guarantee& b) noexcept;
  friend bool operator==(const Guarantee& a, const Guarantee& b) noexcept;
  friend bool operator!=(const Guarantee& a, const Guarantee& b) noexcept { return !(a == b); }
  [[nodiscard]] std::string render() const;
  void digest_into(Digest64& digest) const noexcept;
  [[nodiscard]] std::uint64_t digest() const noexcept;
};

/// Canonical guarantee set: sorted by kind with exactly one entry per kind.
class GuaranteeSet {
 public:
  GuaranteeSet() = default;

  /// Inserts a guarantee. Rejects invalid guarantees, duplicate kinds and
  /// values outside the per-kind domain.
  [[nodiscard]] Status insert(const Guarantee& guarantee);
  [[nodiscard]] Status insert(GuaranteeKind kind, std::uint64_t value, bool hard = true);
  /// Inserts the guarantee, replacing any existing entry for the same kind.
  /// Used when an allocator resolves a contended guarantee after the rest of the
  /// set has already been fixed.
  [[nodiscard]] Status upsert(const Guarantee& guarantee);
  [[nodiscard]] Status upsert(GuaranteeKind kind, std::uint64_t value, bool hard = true);

  [[nodiscard]] const std::vector<Guarantee>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] const Guarantee* find(GuaranteeKind kind) const noexcept;
  [[nodiscard]] bool contains(GuaranteeKind kind) const noexcept { return find(kind) != nullptr; }
  [[nodiscard]] std::optional<std::uint64_t> value_of(GuaranteeKind kind) const noexcept;

  /// Sorts and rejects duplicates/invalid entries produced by decoding.
  [[nodiscard]] Status normalize();
  [[nodiscard]] Status validate() const;

  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;

  friend bool operator==(const GuaranteeSet& a, const GuaranteeSet& b) noexcept {
    return a.items_ == b.items_;
  }
  friend bool operator!=(const GuaranteeSet& a, const GuaranteeSet& b) noexcept { return !(a == b); }

 private:
  std::vector<Guarantee> items_{};
};

/// Classification of the relation between an original and a candidate set.
enum class WeakeningResult : std::uint16_t {
  WeakensOrEquals = 1,
  Strengthens = 2,
  AddsGuarantee = 3,
  Invalid = 4,
};

bool is_valid(WeakeningResult value) noexcept;
std::string_view to_string(WeakeningResult value) noexcept;

/// The central invariant check. Returns WeakensOrEquals only when p candidate
/// grants no more than p original for every shared guarantee kind and adds no
/// guarantee kind of its own.
WeakeningResult classify_weakening(const GuaranteeSet& original,
                                   const GuaranteeSet& candidate) noexcept;

[[nodiscard]] bool weakens_or_equals(const GuaranteeSet& original,
                                     const GuaranteeSet& candidate) noexcept;

/// One bounded weakening of a shared guarantee.
struct Concession {
  GuaranteeKind kind = GuaranteeKind::Availability;
  std::uint64_t original_value = 0;
  std::uint64_t approved_value = 0;

  friend bool operator==(const Concession& a, const Concession& b) noexcept {
    return a.kind == b.kind && a.original_value == b.original_value &&
           a.approved_value == b.approved_value;
  }
  [[nodiscard]] std::string render() const;
};

/// One guarantee present in the original contract that the approved contract no
/// longer carries. Withdrawals are always explicit.
struct GuaranteeWithdrawal {
  GuaranteeKind kind = GuaranteeKind::Availability;
  std::uint64_t original_value = 0;
  ReasonCode reason = ReasonCode::None;

  friend bool operator==(const GuaranteeWithdrawal& a, const GuaranteeWithdrawal& b) noexcept {
    return a.kind == b.kind && a.original_value == b.original_value && a.reason == b.reason;
  }
  [[nodiscard]] std::string render() const;
};

/// Complete, deterministic account of how an approved set differs from the
/// original. Empty means the sets are identical.
struct GuaranteeDelta {
  std::vector<Concession> concessions{};
  std::vector<GuaranteeWithdrawal> withdrawals{};

  [[nodiscard]] bool empty() const noexcept {
    return concessions.empty() && withdrawals.empty();
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return concessions.size() + withdrawals.size();
  }
  [[nodiscard]] std::uint64_t digest() const noexcept;
  [[nodiscard]] std::string render() const;
};

/// Computes the delta from p original to p approved. Returns Invalid status
/// when the relation is not a weakening, so a caller can never record a delta
/// for an illegal transition.
[[nodiscard]] Result<GuaranteeDelta> compute_delta(const GuaranteeSet& original,
                                                   const GuaranteeSet& approved,
                                                   ReasonCode withdrawal_reason);

/// Per-kind limit on how far a guarantee may be weakened. For AtLeast kinds
/// this is an inclusive floor; for AtMost kinds an inclusive ceiling.
struct ConcessionBound {
  GuaranteeKind kind = GuaranteeKind::Availability;
  std::uint64_t weakest_allowed_value = 0;

  friend bool operator<(const ConcessionBound& a, const ConcessionBound& b) noexcept {
    return static_cast<std::uint16_t>(a.kind) < static_cast<std::uint16_t>(b.kind);
  }
  friend bool operator==(const ConcessionBound& a, const ConcessionBound& b) noexcept {
    return a.kind == b.kind && a.weakest_allowed_value == b.weakest_allowed_value;
  }
};

/// The policy-authorised concession window for one contract.
class GuaranteeEnvelope {
 public:
  GuaranteeEnvelope() = default;

  [[nodiscard]] Status insert(const ConcessionBound& bound);
  /// Inserts or tightens the bound for a guarantee kind. When a bound already
  /// exists the stricter of the two is kept.
  [[nodiscard]] Status upsert(const ConcessionBound& bound);
  [[nodiscard]] Status normalize();
  [[nodiscard]] Status validate() const;

  [[nodiscard]] const std::vector<ConcessionBound>& bounds() const noexcept { return bounds_; }
  [[nodiscard]] std::size_t size() const noexcept { return bounds_.size(); }
  [[nodiscard]] bool empty() const noexcept { return bounds_.empty(); }
  [[nodiscard]] const ConcessionBound* find(GuaranteeKind kind) const noexcept;

  /// Maximum number of guarantees that may be conceded at once. 0 means the
  /// envelope authorises no concession at all.
  [[nodiscard]] std::uint32_t max_concessions() const noexcept { return max_concessions_; }
  void set_max_concessions(std::uint32_t value) noexcept { max_concessions_ = value; }

  /// The weakest value this envelope permits for p kind, given the original
  /// value. Returns the original value when the envelope carries no bound.
  [[nodiscard]] std::uint64_t floor_for(GuaranteeKind kind, std::uint64_t original_value) const noexcept;

  /// True when p approved is inside the envelope for every shared kind and the
  /// number of concessions does not exceed max_concessions().
  [[nodiscard]] bool permits(const GuaranteeSet& original, const GuaranteeSet& approved) const noexcept;

  [[nodiscard]] std::uint64_t digest() const noexcept;

 private:
  std::vector<ConcessionBound> bounds_{};
  std::uint32_t max_concessions_ = 0;
};

/// Validates that p envelope is consistent with p original: every bound must
/// name a guarantee the original carries, and every bound must itself be a
/// weakening of the original value.
[[nodiscard]] Status validate_envelope_against(const GuaranteeEnvelope& envelope,
                                               const GuaranteeSet& original) noexcept;

}  // namespace dmf

#endif  // DMF_GUARANTEE_HPP
