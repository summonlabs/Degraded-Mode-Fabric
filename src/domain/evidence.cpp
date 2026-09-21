// Degraded Mode Fabric - evidence aggregation and capability derivation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/evidence.hpp"

#include <algorithm>
#include <cstdio>

namespace dmf {

bool is_valid(EvidenceKind value) noexcept {
  switch (value) {
    case EvidenceKind::FabricTopology:
    case EvidenceKind::AggregateBandwidth:
    case EvidenceKind::RoundTripLatency:
    case EvidenceKind::PathDiversity:
    case EvidenceKind::Reachability:
    case EvidenceKind::NodeCoverage:
    case EvidenceKind::SynchronousDurability:
      return true;
  }
  return false;
}

std::string_view to_string(EvidenceKind value) noexcept {
  switch (value) {
    case EvidenceKind::FabricTopology: return "FABRIC_TOPOLOGY";
    case EvidenceKind::AggregateBandwidth: return "AGGREGATE_BANDWIDTH";
    case EvidenceKind::RoundTripLatency: return "ROUND_TRIP_LATENCY";
    case EvidenceKind::PathDiversity: return "PATH_DIVERSITY";
    case EvidenceKind::Reachability: return "REACHABILITY";
    case EvidenceKind::NodeCoverage: return "NODE_COVERAGE";
    case EvidenceKind::SynchronousDurability: return "SYNCHRONOUS_DURABILITY";
  }
  return "UNRECOGNISED";
}

std::uint64_t max_value_for(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::FabricTopology: return 4;
    case EvidenceKind::AggregateBandwidth: return kMaxBandwidthKbps;
    case EvidenceKind::RoundTripLatency: return kMaxLatencyUs;
    case EvidenceKind::PathDiversity: return kMaxPathDiversity;
    case EvidenceKind::Reachability: return kPpmScale;
    case EvidenceKind::NodeCoverage: return kPpmScale;
    case EvidenceKind::SynchronousDurability: return 1;
  }
  return 0;
}

bool is_valid(FabricTopologyClass value) noexcept {
  switch (value) {
    case FabricTopologyClass::FullMesh:
    case FabricTopologyClass::PartialMesh:
    case FabricTopologyClass::Partitioned:
    case FabricTopologyClass::Isolated:
      return true;
  }
  return false;
}

std::string_view to_string(FabricTopologyClass value) noexcept {
  switch (value) {
    case FabricTopologyClass::FullMesh: return "FULL_MESH";
    case FabricTopologyClass::PartialMesh: return "PARTIAL_MESH";
    case FabricTopologyClass::Partitioned: return "PARTITIONED";
    case FabricTopologyClass::Isolated: return "ISOLATED";
  }
  return "UNRECOGNISED";
}

bool is_valid(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Known:
    case EvidenceState::Unknown:
    case EvidenceState::Stale:
    case EvidenceState::Conflict:
    case EvidenceState::Invalid:
    case EvidenceState::Unsupported:
      return true;
  }
  return false;
}

std::string_view to_string(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Known: return "KNOWN";
    case EvidenceState::Unknown: return "UNKNOWN";
    case EvidenceState::Stale: return "STALE";
    case EvidenceState::Conflict: return "CONFLICT";
    case EvidenceState::Invalid: return "INVALID";
    case EvidenceState::Unsupported: return "UNSUPPORTED";
  }
  return "UNRECOGNISED";
}

std::uint8_t evidence_severity(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Known: return 0;
    case EvidenceState::Stale: return 1;
    case EvidenceState::Unknown: return 2;
    case EvidenceState::Unsupported: return 3;
    case EvidenceState::Conflict: return 4;
    case EvidenceState::Invalid: return 5;
  }
  return 6;
}

bool evidence_is_authoritative(EvidenceState value) noexcept { return value == EvidenceState::Known; }

bool FreshnessWindow::fresh(Tick observed) const noexcept {
  if (!enforced) return true;
  if (observed > now) return false;  // evidence from the future is not trustworthy
  const auto age = checked_sub(now.value, observed.value);
  if (!age.has_value()) return false;
  return *age <= budget_ticks;
}

bool EvidenceItem::identified() const noexcept {
  return id.valid() && validate().ok();
}

Status EvidenceItem::validate() const {
  if (!publisher.valid()) return Status(ErrorCode::InvalidArgument, "evidence publisher is unset");
  if (!publisher_boot.valid()) {
    return Status(ErrorCode::InvalidArgument, "evidence publisher boot is unset");
  }
  if (!generation.valid()) return Status(ErrorCode::InvalidArgument, "evidence generation is unset");
  if (!scope.valid()) return Status(ErrorCode::InvalidArgument, "evidence scope is unset");
  if (!is_valid(kind)) return Status(ErrorCode::InvalidArgument, "evidence kind is invalid");
  if (!is_valid(state)) return Status(ErrorCode::InvalidArgument, "evidence state is invalid");
  if (!is_valid(origin)) return Status(ErrorCode::InvalidArgument, "evidence origin is invalid");
  if (value > max_value_for(kind)) {
    return Status(ErrorCode::OutOfRange, "evidence value is outside its per-kind domain");
  }
  return Status{};
}

bool operator==(const EvidenceItem& a, const EvidenceItem& b) noexcept {
  return a.id == b.id && a.publisher == b.publisher && a.publisher_boot == b.publisher_boot &&
         a.generation == b.generation && a.kind == b.kind && a.scope == b.scope &&
         a.state == b.state && a.value == b.value && a.observed_tick == b.observed_tick &&
         a.origin == b.origin;
}

std::string EvidenceItem::render() const {
  std::string out;
  out.append(to_string(kind));
  out.append("@");
  out.append(scope.to_string());
  out.append("=");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  out.append(buffer);
  out.append(" ");
  out.append(to_string(state));
  out.append(" pub=");
  out.append(publisher.to_string());
  out.append(" gen=");
  out.append(generation.to_string());
  out.append(" origin=");
  out.append(to_string(origin));
  return out;
}

Status EvidenceVector::push(const EvidenceItem& item) {
  if (!item.identified()) {
    return Status(ErrorCode::InvalidArgument, "retained evidence must carry an identity");
  }
  Status status = item.validate();
  if (!status.ok()) return status;
  if (items_.size() >= limit_) {
    return Status(ErrorCode::CapacityExceeded, "evidence vector is full");
  }
  items_.push_back(item);
  return Status{};
}

namespace {

bool evidence_item_less(const EvidenceItem& a, const EvidenceItem& b) noexcept {
  if (a.scope != b.scope) return a.scope < b.scope;
  if (a.kind != b.kind) {
    return static_cast<std::uint16_t>(a.kind) < static_cast<std::uint16_t>(b.kind);
  }
  if (a.publisher != b.publisher) return a.publisher < b.publisher;
  if (a.generation != b.generation) return b.generation < a.generation;  // newest first
  if (a.publisher_boot != b.publisher_boot) return b.publisher_boot < a.publisher_boot;
  return a.id < b.id;
}

}  // namespace

Status EvidenceVector::normalize() {
  std::sort(items_.begin(), items_.end(), evidence_item_less);
  // Supersede: keep only the newest generation per (scope, kind, publisher).
  std::vector<EvidenceItem> kept;
  kept.reserve(items_.size());
  for (const EvidenceItem& item : items_) {
    Status status = item.validate();
    if (!status.ok()) return status;
    if (!item.identified()) {
      return Status(ErrorCode::InvalidArgument, "retained evidence must carry an identity");
    }
    if (!kept.empty()) {
      const EvidenceItem& previous = kept.back();
      if (previous.scope == item.scope && previous.kind == item.kind &&
          previous.publisher == item.publisher) {
        continue;  // older generation of the same publisher
      }
    }
    kept.push_back(item);
  }
  items_.swap(kept);
  return Status{};
}

Status EvidenceVector::validate() const {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (!items_[i].identified()) {
      return Status(ErrorCode::InvalidArgument, "retained evidence must carry an identity");
    }
    Status status = items_[i].validate();
    if (!status.ok()) return status;
    if (i > 0 && !evidence_item_less(items_[i - 1], items_[i])) {
      return Status(ErrorCode::Invalid, "evidence vector is not canonically ordered");
    }
  }
  if (items_.size() > limit_) {
    return Status(ErrorCode::CapacityExceeded, "evidence vector exceeds its bound");
  }
  return Status{};
}

EvidenceState EvidenceVector::state_for(ScopeId scope, EvidenceKind kind) const noexcept {
  EvidenceState worst = EvidenceState::Known;
  bool present = false;
  std::optional<std::uint64_t> agreed;
  for (const EvidenceItem& item : items_) {
    if (item.scope != scope || item.kind != kind) continue;
    present = true;
    if (item.state == EvidenceState::Known) {
      if (!agreed.has_value()) {
        agreed = item.value;
      } else if (*agreed != item.value) {
        worst = EvidenceState::Conflict;
        continue;
      }
    }
    if (evidence_severity(item.state) > evidence_severity(worst)) {
      worst = item.state;
    }
  }
  if (!present) return EvidenceState::Unknown;
  return worst;
}

std::optional<std::uint64_t> EvidenceVector::value_for(ScopeId scope, EvidenceKind kind) const noexcept {
  if (state_for(scope, kind) != EvidenceState::Known) return std::nullopt;
  std::optional<std::uint64_t> agreed;
  for (const EvidenceItem& item : items_) {
    if (item.scope != scope || item.kind != kind) continue;
    if (item.state != EvidenceState::Known) continue;
    if (!agreed.has_value()) agreed = item.value;
  }
  return agreed;
}

Tick EvidenceVector::newest_tick_for(ScopeId scope, EvidenceKind kind) const noexcept {
  Tick newest{0};
  for (const EvidenceItem& item : items_) {
    if (item.scope != scope || item.kind != kind) continue;
    if (item.observed_tick > newest) newest = item.observed_tick;
  }
  return newest;
}

EvidenceState EvidenceVector::aggregate_state() const noexcept {
  if (items_.empty()) return EvidenceState::Unknown;
  EvidenceState worst = EvidenceState::Known;
  for (const EvidenceItem& item : items_) {
    const EvidenceState group = state_for(item.scope, item.kind);
    if (evidence_severity(group) > evidence_severity(worst)) worst = group;
  }
  return worst;
}

OriginClass EvidenceVector::origin_class() const noexcept {
  if (items_.empty()) return OriginClass::Unspecified;
  OriginClass worst = OriginClass::Real;
  bool saw_synthetic = false;
  bool saw_unsupported = false;
  bool saw_unspecified = false;
  for (const EvidenceItem& item : items_) {
    switch (item.origin) {
      case OriginClass::Unsupported: saw_unsupported = true; break;
      case OriginClass::Synthetic: saw_synthetic = true; break;
      case OriginClass::Unspecified: saw_unspecified = true; break;
      case OriginClass::Real: break;
    }
  }
  if (saw_unsupported) worst = OriginClass::Unsupported;
  else if (saw_synthetic) worst = OriginClass::Synthetic;
  else if (saw_unspecified) worst = OriginClass::Unspecified;
  return worst;
}

std::uint64_t EvidenceVector::digest() const noexcept {
  Digest64 digest;
  digest.update_u32(static_cast<std::uint32_t>(items_.size()));
  for (const EvidenceItem& item : items_) {
    digest.update_u64(item.id.value());
    digest.update_u64(item.publisher.value());
    digest.update_u64(item.publisher_boot.value());
    digest.update_u64(item.generation.value());
    digest.update_u16(static_cast<std::uint16_t>(item.kind));
    digest.update_u64(item.scope.value());
    digest.update_u16(static_cast<std::uint16_t>(item.state));
    digest.update_u64(item.value);
    digest.update_u64(item.observed_tick.value);
    digest.update_u16(static_cast<std::uint16_t>(item.origin));
  }
  return digest.value();
}

std::string EvidenceVector::render() const {
  if (items_.empty()) return "<no evidence>";
  std::string out;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (i != 0) out.append(" | ");
    out.append(items_[i].render());
  }
  return out;
}

std::uint64_t CapabilitySnapshot::digest() const noexcept {
  Digest64 digest;
  digest.update_u64(scope.value());
  digest.update_u64(generation.value());
  digest.update_u16(static_cast<std::uint16_t>(state));
  digest.update_u16(static_cast<std::uint16_t>(topology));
  digest.update_u64(bandwidth_kbps);
  digest.update_u64(rtt_p99_us);
  digest.update_u32(path_diversity);
  digest.update_u32(reachable_ppm);
  digest.update_u32(node_coverage_ppm);
  digest.update_bool(synchronous_durability);
  digest.update_u64(observed_tick.value);
  digest.update_u16(static_cast<std::uint16_t>(origin));
  return digest.value();
}

std::string CapabilitySnapshot::render() const {
  const std::string scope_text = scope.to_string();
  const std::string generation_text = generation.to_string();
  const std::string_view state_text = to_string(state);
  const std::string_view topology_text = to_string(topology);
  const std::string_view origin_text = to_string(origin);
  char buffer[320];
  std::snprintf(buffer, sizeof(buffer),
                "scope=%s gen=%s state=%.*s topology=%.*s bw=%llu kbps rtt=%llu us paths=%u "
                "avail=%u ppm coverage=%u ppm sync=%s origin=%.*s",
                scope_text.c_str(), generation_text.c_str(), static_cast<int>(state_text.size()),
                state_text.data(), static_cast<int>(topology_text.size()), topology_text.data(),
                static_cast<unsigned long long>(bandwidth_kbps),
                static_cast<unsigned long long>(rtt_p99_us), path_diversity, reachable_ppm,
                node_coverage_ppm, synchronous_durability ? "yes" : "no",
                static_cast<int>(origin_text.size()), origin_text.data());
  return std::string(buffer);
}

namespace {

struct CapabilityField {
  EvidenceKind kind;
  bool required;
};

constexpr CapabilityField kCapabilityFields[] = {
    {EvidenceKind::FabricTopology, true},        {EvidenceKind::AggregateBandwidth, true},
    {EvidenceKind::RoundTripLatency, true},      {EvidenceKind::PathDiversity, true},
    {EvidenceKind::Reachability, true},          {EvidenceKind::NodeCoverage, true},
    {EvidenceKind::SynchronousDurability, true},
};

}  // namespace

Result<CapabilitySnapshot> derive_capability(const EvidenceVector& evidence, ScopeId scope,
                                             CapacityGeneration generation,
                                             const FreshnessWindow& freshness) noexcept {
  if (!scope.valid() || !generation.valid()) {
    return Status(ErrorCode::InvalidArgument, "capability derivation needs a scope and generation");
  }
  CapabilitySnapshot snapshot;
  snapshot.scope = scope;
  snapshot.generation = generation;
  snapshot.origin = evidence.origin_class();

  EvidenceState worst = EvidenceState::Known;
  bool any_present = false;
  for (const CapabilityField& field : kCapabilityFields) {
    const EvidenceState state = evidence.state_for(scope, field.kind);
    if (state != EvidenceState::Known) {
      if (evidence_severity(state) > evidence_severity(worst)) worst = state;
      continue;
    }
    const Tick newest = evidence.newest_tick_for(scope, field.kind);
    if (!freshness.fresh(newest)) {
      if (evidence_severity(EvidenceState::Stale) > evidence_severity(worst)) {
        worst = EvidenceState::Stale;
      }
      continue;
    }
    any_present = true;
    const auto value = evidence.value_for(scope, field.kind);
    if (!value.has_value()) {
      if (evidence_severity(EvidenceState::Unknown) > evidence_severity(worst)) {
        worst = EvidenceState::Unknown;
      }
      continue;
    }
    switch (field.kind) {
      case EvidenceKind::FabricTopology:
        snapshot.topology = static_cast<FabricTopologyClass>(*value);
        if (!is_valid(snapshot.topology)) {
          return Status(ErrorCode::Invalid, "topology evidence carries an invalid class");
        }
        break;
      case EvidenceKind::AggregateBandwidth: snapshot.bandwidth_kbps = *value; break;
      case EvidenceKind::RoundTripLatency: snapshot.rtt_p99_us = *value; break;
      case EvidenceKind::PathDiversity: snapshot.path_diversity = static_cast<std::uint32_t>(*value); break;
      case EvidenceKind::Reachability: snapshot.reachable_ppm = static_cast<std::uint32_t>(*value); break;
      case EvidenceKind::NodeCoverage: snapshot.node_coverage_ppm = static_cast<std::uint32_t>(*value); break;
      case EvidenceKind::SynchronousDurability: snapshot.synchronous_durability = (*value != 0); break;
    }
    if (newest > snapshot.observed_tick) snapshot.observed_tick = newest;
  }

  if (!any_present && worst == EvidenceState::Known) worst = EvidenceState::Unknown;
  snapshot.state = worst;
  return snapshot;
}

SupportTri supports(const CapabilitySnapshot& capability, const Guarantee& guarantee) noexcept {
  if (!guarantee.valid() || !is_valid(capability.state)) return SupportTri::Invalid;
  if (!capability.usable()) return SupportTri::Indeterminate;
  switch (guarantee.kind) {
    case GuaranteeKind::Availability:
      return capability.reachable_ppm >= guarantee.value ? SupportTri::Supported
                                                         : SupportTri::NotSupported;
    case GuaranteeKind::Bandwidth:
      return capability.bandwidth_kbps >= guarantee.value ? SupportTri::Supported
                                                          : SupportTri::NotSupported;
    case GuaranteeKind::LatencyP99:
      return capability.rtt_p99_us <= guarantee.value ? SupportTri::Supported
                                                      : SupportTri::NotSupported;
    case GuaranteeKind::PathDiversity:
      return capability.path_diversity >= guarantee.value ? SupportTri::Supported
                                                          : SupportTri::NotSupported;
    case GuaranteeKind::Durability: {
      const std::uint64_t level = capability.synchronous_durability ? kMaxDurabilityLevel : 0ULL;
      return level >= guarantee.value ? SupportTri::Supported : SupportTri::NotSupported;
    }
    case GuaranteeKind::Reachability:
      return capability.node_coverage_ppm >= guarantee.value ? SupportTri::Supported
                                                             : SupportTri::NotSupported;
  }
  return SupportTri::NotModelled;
}

ReasonCode shortfall_reason(const CapabilitySnapshot& capability,
                            const Guarantee& guarantee) noexcept {
  if (!capability.usable()) {
    switch (capability.state) {
      case EvidenceState::Stale: return ReasonCode::EvidenceStale;
      case EvidenceState::Conflict: return ReasonCode::EvidenceConflict;
      case EvidenceState::Invalid: return ReasonCode::EvidenceInvalid;
      case EvidenceState::Unsupported: return ReasonCode::EvidenceUnsupported;
      case EvidenceState::Unknown: return ReasonCode::EvidenceUnknown;
      case EvidenceState::Known: break;
    }
  }
  if (capability.topology == FabricTopologyClass::Partitioned ||
      capability.topology == FabricTopologyClass::Isolated) {
    return ReasonCode::FabricPartitioned;
  }
  (void)guarantee;
  return ReasonCode::FabricCapabilityShortfall;
}

}  // namespace dmf
