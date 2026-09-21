// Degraded Mode Fabric - strongly typed identities, generations and sequences.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Identity rules enforced by this header:
//   * an identity is never interchangeable with a raw integer or with an
//     identity of a different kind;
//   * value 0 is "unset" and never valid;
//   * matching identity is not matching generation: holders must compare both.
#ifndef DMF_IDS_HPP
#define DMF_IDS_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "dmf/core.hpp"

namespace dmf {

/// Tag types. Each one names a distinct identity domain.
struct ServiceTag;
struct ContractTag;
struct ScopeTag;
struct NodeTag;
struct PolicyTag;
struct RuleTag;
struct GrantTag;
struct DecisionTag;
struct FenceTag;
struct EvidenceTag;
struct PublisherTag;
struct SessionTag;
struct AttemptTag;
struct SubjectTag;
struct PlanTag;
struct PrincipalTag;
struct EnvelopeTag;
struct CoordinatorTermTag;
struct BootTag;
struct ProcessTag;
struct FabricGenerationTag;
struct CapacityGenerationTag;
struct PolicyGenerationTag;
struct ContractGenerationTag;
struct EvidenceGenerationTag;
struct SubjectGenerationTag;
struct SequenceTag;

/// Strongly typed unsigned identity. 0 means unset and is never valid.
template <class Tag>
class StrongUInt {
 public:
  using tag_type = Tag;

  constexpr StrongUInt() noexcept = default;
  constexpr explicit StrongUInt(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongUInt from_value(std::uint64_t value) noexcept {
    return StrongUInt(value);
  }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool unset() const noexcept { return value_ == 0; }

  /// Next value in this identity domain. Returns nullopt at the top of the
  /// domain rather than wrapping, so exhaustion is always observable.
  [[nodiscard]] constexpr std::optional<StrongUInt> next() const noexcept {
    const auto advanced = checked_add(value_, std::uint64_t{1});
    if (!advanced.has_value()) return std::nullopt;
    return StrongUInt(*advanced);
  }

  /// Ordering is defined so that identity domains sort deterministically and
  /// independently of insertion order.
  friend constexpr bool operator==(StrongUInt a, StrongUInt b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongUInt a, StrongUInt b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongUInt a, StrongUInt b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(StrongUInt a, StrongUInt b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(StrongUInt a, StrongUInt b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator>=(StrongUInt a, StrongUInt b) noexcept { return a.value_ >= b.value_; }

  [[nodiscard]] std::string to_string() const { return to_hex(value_); }

 private:
  std::uint64_t value_ = 0;
};

using ServiceId = StrongUInt<ServiceTag>;
using ContractId = StrongUInt<ContractTag>;
using ScopeId = StrongUInt<ScopeTag>;
using NodeId = StrongUInt<NodeTag>;
using PolicyId = StrongUInt<PolicyTag>;
using RuleId = StrongUInt<RuleTag>;
using GrantId = StrongUInt<GrantTag>;
using DecisionId = StrongUInt<DecisionTag>;
using FenceId = StrongUInt<FenceTag>;
using EvidenceId = StrongUInt<EvidenceTag>;
using PublisherId = StrongUInt<PublisherTag>;
using SessionId = StrongUInt<SessionTag>;
using AttemptId = StrongUInt<AttemptTag>;
using SubjectId = StrongUInt<SubjectTag>;
using PlanId = StrongUInt<PlanTag>;
using PrincipalId = StrongUInt<PrincipalTag>;
using EnvelopeId = StrongUInt<EnvelopeTag>;

/// Authority-bearing generations. Every dependency that can invalidate a
/// decision or grant carries one of these. Zero is "unset" and is never valid.
using CoordinatorTerm = StrongUInt<CoordinatorTermTag>;
using BootIncarnation = StrongUInt<BootTag>;
using ProcessId = StrongUInt<ProcessTag>;
using FabricGeneration = StrongUInt<FabricGenerationTag>;
using CapacityGeneration = StrongUInt<CapacityGenerationTag>;
using PolicyGeneration = StrongUInt<PolicyGenerationTag>;
using ContractGeneration = StrongUInt<ContractGenerationTag>;
using EvidenceGeneration = StrongUInt<EvidenceGenerationTag>;
using SubjectGeneration = StrongUInt<SubjectGenerationTag>;
using SequenceNumber = StrongUInt<SequenceTag>;

/// Monotonic allocator for one identity domain. Never wraps: exhaustion is
/// reported so the caller can refuse rather than reuse an identity.
template <class Tag>
class IdAllocator {
 public:
  explicit IdAllocator(std::uint64_t start = 1) noexcept : next_(start) {}

  [[nodiscard]] std::optional<StrongUInt<Tag>> allocate() noexcept {
    if (next_ == 0) return std::nullopt;
    const StrongUInt<Tag> issued(next_);
    const auto advanced = checked_add(next_, std::uint64_t{1});
    next_ = advanced.has_value() ? *advanced : 0;
    return issued;
  }

  /// Restores the allocator past a durable high-water mark. Never moves
  /// backwards, so an identity is never reused after restart.
  void observe(std::uint64_t high_water) noexcept {
    if (high_water == 0) return;
    const auto candidate = checked_add(high_water, std::uint64_t{1});
    const std::uint64_t observed = candidate.has_value() ? *candidate : 0;
    if (next_ == 0) return;
    if (observed == 0 || observed > next_) next_ = observed;
  }

  [[nodiscard]] std::uint64_t high_water() const noexcept {
    return next_ == 0 ? 0 : next_ - 1;
  }

 private:
  std::uint64_t next_ = 1;
};

/// Explicit provenance classification required wherever the runtime reports
/// evidence. Physical fabric hardware is never claimed under Real unless it was
/// actually exercised.
enum class OriginClass : std::uint16_t {
  Unspecified = 0,
  Real = 1,
  Synthetic = 2,
  Unsupported = 3,
};

bool is_valid(OriginClass value) noexcept;
std::string_view to_string(OriginClass value) noexcept;

/// Serialisation helpers shared by every durable and wire codec.
void write_id(ByteWriter& writer, std::uint64_t value) noexcept;
std::uint64_t read_id(ByteReader& reader) noexcept;

}  // namespace dmf

namespace std {
template <class Tag>
struct hash<dmf::StrongUInt<Tag>> {
  std::size_t operator()(const dmf::StrongUInt<Tag>& id) const noexcept {
    // splitmix64 finaliser: stable, platform independent and well distributed
    // for the sequential identity domains used by the runtime.
    std::uint64_t x = id.value() + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return static_cast<std::size_t>(x);
  }
};
}  // namespace std

#endif  // DMF_IDS_HPP
