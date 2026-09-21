// Degraded Mode Fabric - guarantee algebra.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/guarantee.hpp"

#include <algorithm>
#include <cstdio>

namespace dmf {

namespace {

void append_u64(std::string& out, std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  out.append(buffer);
}

}  // namespace

bool is_valid(ServiceClass value) noexcept {
  switch (value) {
    case ServiceClass::Protected:
    case ServiceClass::Standard:
    case ServiceClass::BestEffort:
    case ServiceClass::Scavenger:
      return true;
  }
  return false;
}

std::string_view to_string(ServiceClass value) noexcept {
  switch (value) {
    case ServiceClass::Protected: return "PROTECTED";
    case ServiceClass::Standard: return "STANDARD";
    case ServiceClass::BestEffort: return "BEST_EFFORT";
    case ServiceClass::Scavenger: return "SCAVENGER";
  }
  return "UNRECOGNISED";
}

std::uint8_t class_rank(ServiceClass value) noexcept {
  switch (value) {
    case ServiceClass::Protected: return 0;
    case ServiceClass::Standard: return 1;
    case ServiceClass::BestEffort: return 2;
    case ServiceClass::Scavenger: return 3;
  }
  return 4;
}

bool is_valid(GuaranteeKind value) noexcept {
  switch (value) {
    case GuaranteeKind::Availability:
    case GuaranteeKind::Bandwidth:
    case GuaranteeKind::LatencyP99:
    case GuaranteeKind::PathDiversity:
    case GuaranteeKind::Durability:
    case GuaranteeKind::Reachability:
      return true;
  }
  return false;
}

std::string_view to_string(GuaranteeKind value) noexcept {
  switch (value) {
    case GuaranteeKind::Availability: return "AVAILABILITY";
    case GuaranteeKind::Bandwidth: return "BANDWIDTH";
    case GuaranteeKind::LatencyP99: return "LATENCY_P99";
    case GuaranteeKind::PathDiversity: return "PATH_DIVERSITY";
    case GuaranteeKind::Durability: return "DURABILITY";
    case GuaranteeKind::Reachability: return "REACHABILITY";
  }
  return "UNRECOGNISED";
}

std::string_view guarantee_unit(GuaranteeKind value) noexcept {
  switch (value) {
    case GuaranteeKind::Availability: return "ppm";
    case GuaranteeKind::Bandwidth: return "kbps";
    case GuaranteeKind::LatencyP99: return "us";
    case GuaranteeKind::PathDiversity: return "paths";
    case GuaranteeKind::Durability: return "level";
    case GuaranteeKind::Reachability: return "ppm";
  }
  return "unknown";
}

bool is_valid(Comparator value) noexcept {
  return value == Comparator::AtLeast || value == Comparator::AtMost;
}

std::string_view to_string(Comparator value) noexcept {
  switch (value) {
    case Comparator::AtLeast: return "AT_LEAST";
    case Comparator::AtMost: return "AT_MOST";
  }
  return "UNRECOGNISED";
}

Comparator comparator_for(GuaranteeKind kind) noexcept {
  switch (kind) {
    case GuaranteeKind::LatencyP99: return Comparator::AtMost;
    case GuaranteeKind::Availability:
    case GuaranteeKind::Bandwidth:
    case GuaranteeKind::PathDiversity:
    case GuaranteeKind::Durability:
    case GuaranteeKind::Reachability:
      return Comparator::AtLeast;
  }
  return Comparator::AtLeast;
}

std::uint64_t max_value_for(GuaranteeKind kind) noexcept {
  switch (kind) {
    case GuaranteeKind::Availability: return kPpmScale;
    case GuaranteeKind::Bandwidth: return kMaxBandwidthKbps;
    case GuaranteeKind::LatencyP99: return kMaxLatencyUs;
    case GuaranteeKind::PathDiversity: return kMaxPathDiversity;
    case GuaranteeKind::Durability: return kMaxDurabilityLevel;
    case GuaranteeKind::Reachability: return kPpmScale;
  }
  return 0;
}

bool Guarantee::valid() const noexcept {
  if (!is_valid(kind)) return false;
  if (!is_valid(comparator)) return false;
  if (comparator != comparator_for(kind)) return false;
  return value <= max_value_for(kind);
}

bool operator<(const Guarantee& a, const Guarantee& b) noexcept {
  if (a.kind != b.kind) {
    return static_cast<std::uint16_t>(a.kind) < static_cast<std::uint16_t>(b.kind);
  }
  if (a.comparator != b.comparator) {
    return static_cast<std::uint16_t>(a.comparator) < static_cast<std::uint16_t>(b.comparator);
  }
  return a.value < b.value;
}

bool operator==(const Guarantee& a, const Guarantee& b) noexcept {
  return a.kind == b.kind && a.comparator == b.comparator && a.value == b.value && a.hard == b.hard;
}

std::string Guarantee::render() const {
  std::string out;
  out.append(to_string(kind));
  out.append(comparator == Comparator::AtLeast ? ">=" : "<=");
  append_u64(out, value);
  out.push_back(' ');
  out.append(guarantee_unit(kind));
  if (!hard) out.append(" (soft)");
  return out;
}

void Guarantee::digest_into(Digest64& digest) const noexcept {
  digest.update_u16(static_cast<std::uint16_t>(kind));
  digest.update_u16(static_cast<std::uint16_t>(comparator));
  digest.update_u64(value);
  digest.update_bool(hard);
}

std::uint64_t Guarantee::digest() const noexcept {
  Digest64 digest;
  digest_into(digest);
  return digest.value();
}

Status GuaranteeSet::insert(const Guarantee& guarantee) {
  if (!guarantee.valid()) {
    return Status(ErrorCode::InvalidArgument, "guarantee outside its per-kind domain");
  }
  const auto position = std::lower_bound(
      items_.begin(), items_.end(), guarantee,
      [](const Guarantee& lhs, const Guarantee& rhs) { return lhs.kind < rhs.kind; });
  if (position != items_.end() && position->kind == guarantee.kind) {
    return Status(ErrorCode::AlreadyExists, "duplicate guarantee kind");
  }
  items_.insert(position, guarantee);
  return Status{};
}

Status GuaranteeSet::insert(GuaranteeKind kind, std::uint64_t value, bool hard) {
  Guarantee guarantee;
  guarantee.kind = kind;
  guarantee.comparator = comparator_for(kind);
  guarantee.value = value;
  guarantee.hard = hard;
  return insert(guarantee);
}

Status GuaranteeSet::upsert(const Guarantee& guarantee) {
  if (!guarantee.valid()) {
    return Status(ErrorCode::InvalidArgument, "guarantee outside its per-kind domain");
  }
  const auto position = std::lower_bound(
      items_.begin(), items_.end(), guarantee,
      [](const Guarantee& lhs, const Guarantee& rhs) { return lhs.kind < rhs.kind; });
  if (position != items_.end() && position->kind == guarantee.kind) {
    *position = guarantee;
    return Status{};
  }
  items_.insert(position, guarantee);
  return Status{};
}

Status GuaranteeSet::upsert(GuaranteeKind kind, std::uint64_t value, bool hard) {
  Guarantee guarantee;
  guarantee.kind = kind;
  guarantee.comparator = comparator_for(kind);
  guarantee.value = value;
  guarantee.hard = hard;
  return upsert(guarantee);
}

const Guarantee* GuaranteeSet::find(GuaranteeKind kind) const noexcept {
  const auto position = std::lower_bound(
      items_.begin(), items_.end(), kind,
      [](const Guarantee& lhs, GuaranteeKind rhs) { return lhs.kind < rhs; });
  if (position == items_.end() || position->kind != kind) return nullptr;
  return &(*position);
}

std::optional<std::uint64_t> GuaranteeSet::value_of(GuaranteeKind kind) const noexcept {
  const Guarantee* found = find(kind);
  if (found == nullptr) return std::nullopt;
  return found->value;
}

Status GuaranteeSet::validate() const {
  GuaranteeKind previous = GuaranteeKind::Availability;
  bool first = true;
  for (const Guarantee& guarantee : items_) {
    if (!guarantee.valid()) {
      return Status(ErrorCode::Invalid, "guarantee outside its per-kind domain");
    }
    if (!first && static_cast<std::uint16_t>(guarantee.kind) <= static_cast<std::uint16_t>(previous)) {
      return Status(ErrorCode::Invalid, "guarantee set is not canonically ordered or has duplicates");
    }
    previous = guarantee.kind;
    first = false;
  }
  return Status{};
}

Status GuaranteeSet::normalize() {
  std::sort(items_.begin(), items_.end(),
            [](const Guarantee& a, const Guarantee& b) { return a.kind < b.kind; });
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (!items_[i].valid()) {
      return Status(ErrorCode::Invalid, "guarantee outside its per-kind domain");
    }
    if (i > 0 && items_[i - 1].kind == items_[i].kind) {
      return Status(ErrorCode::Invalid, "duplicate guarantee kind");
    }
  }
  return Status{};
}

std::uint64_t GuaranteeSet::digest() const noexcept {
  Digest64 digest;
  digest.update_u32(static_cast<std::uint32_t>(items_.size()));
  for (const Guarantee& guarantee : items_) guarantee.digest_into(digest);
  return digest.value();
}

std::string GuaranteeSet::render() const {
  if (items_.empty()) return "{}";
  std::string out = "{";
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (i != 0) out.append(", ");
    out.append(items_[i].render());
  }
  out.push_back('}');
  return out;
}

bool is_valid(WeakeningResult value) noexcept {
  switch (value) {
    case WeakeningResult::WeakensOrEquals:
    case WeakeningResult::Strengthens:
    case WeakeningResult::AddsGuarantee:
    case WeakeningResult::Invalid:
      return true;
  }
  return false;
}

std::string_view to_string(WeakeningResult value) noexcept {
  switch (value) {
    case WeakeningResult::WeakensOrEquals: return "WEAKENS_OR_EQUALS";
    case WeakeningResult::Strengthens: return "STRENGTHENS";
    case WeakeningResult::AddsGuarantee: return "ADDS_GUARANTEE";
    case WeakeningResult::Invalid: return "INVALID";
  }
  return "UNRECOGNISED";
}

WeakeningResult classify_weakening(const GuaranteeSet& original,
                                   const GuaranteeSet& candidate) noexcept {
  if (!original.validate().ok() || !candidate.validate().ok()) {
    return WeakeningResult::Invalid;
  }
  for (const Guarantee& proposed : candidate.items()) {
    const Guarantee* base = original.find(proposed.kind);
    if (base == nullptr) {
      return WeakeningResult::AddsGuarantee;
    }
    if (proposed.comparator != base->comparator) {
      return WeakeningResult::Invalid;
    }
    if (proposed.hard && !base->hard) {
      return WeakeningResult::Strengthens;
    }
    if (base->comparator == Comparator::AtLeast) {
      if (proposed.value > base->value) return WeakeningResult::Strengthens;
    } else {
      if (proposed.value < base->value) return WeakeningResult::Strengthens;
    }
  }
  return WeakeningResult::WeakensOrEquals;
}

bool weakens_or_equals(const GuaranteeSet& original, const GuaranteeSet& candidate) noexcept {
  return classify_weakening(original, candidate) == WeakeningResult::WeakensOrEquals;
}

std::string Concession::render() const {
  std::string out;
  out.append(to_string(kind));
  out.push_back(' ');
  append_u64(out, original_value);
  out.append(" -> ");
  append_u64(out, approved_value);
  out.push_back(' ');
  out.append(guarantee_unit(kind));
  return out;
}

std::string GuaranteeWithdrawal::render() const {
  std::string out;
  out.append(to_string(kind));
  out.append(" withdrawn (was ");
  append_u64(out, original_value);
  out.push_back(' ');
  out.append(guarantee_unit(kind));
  out.append(", reason ");
  out.append(to_string(reason));
  out.push_back(')');
  return out;
}

std::uint64_t GuaranteeDelta::digest() const noexcept {
  Digest64 digest;
  digest.update_u32(static_cast<std::uint32_t>(concessions.size()));
  for (const Concession& concession : concessions) {
    digest.update_u16(static_cast<std::uint16_t>(concession.kind));
    digest.update_u64(concession.original_value);
    digest.update_u64(concession.approved_value);
  }
  digest.update_u32(static_cast<std::uint32_t>(withdrawals.size()));
  for (const GuaranteeWithdrawal& withdrawal : withdrawals) {
    digest.update_u16(static_cast<std::uint16_t>(withdrawal.kind));
    digest.update_u64(withdrawal.original_value);
    digest.update_u16(static_cast<std::uint16_t>(withdrawal.reason));
  }
  return digest.value();
}

std::string GuaranteeDelta::render() const {
  if (empty()) return "no concession";
  std::string out;
  for (std::size_t i = 0; i < concessions.size(); ++i) {
    if (i != 0) out.append("; ");
    out.append(concessions[i].render());
  }
  for (std::size_t i = 0; i < withdrawals.size(); ++i) {
    if (!out.empty()) out.append("; ");
    out.append(withdrawals[i].render());
  }
  return out;
}

Result<GuaranteeDelta> compute_delta(const GuaranteeSet& original, const GuaranteeSet& approved,
                                     ReasonCode withdrawal_reason) {
  const WeakeningResult relation = classify_weakening(original, approved);
  if (relation != WeakeningResult::WeakensOrEquals) {
    return Status(ErrorCode::Invalid, std::string("approved set is not a weakening: ") +
                                          std::string(to_string(relation)));
  }
  GuaranteeDelta delta;
  for (const Guarantee& base : original.items()) {
    const Guarantee* proposed = approved.find(base.kind);
    if (proposed == nullptr) {
      delta.withdrawals.push_back(GuaranteeWithdrawal{base.kind, base.value, withdrawal_reason});
      continue;
    }
    if (proposed->value != base.value) {
      delta.concessions.push_back(Concession{base.kind, base.value, proposed->value});
    }
  }
  return delta;
}

Status GuaranteeEnvelope::insert(const ConcessionBound& bound) {
  if (!is_valid(bound.kind)) {
    return Status(ErrorCode::InvalidArgument, "envelope bound has an invalid guarantee kind");
  }
  if (bound.weakest_allowed_value > max_value_for(bound.kind)) {
    return Status(ErrorCode::InvalidArgument, "envelope bound outside the per-kind domain");
  }
  const auto position = std::lower_bound(
      bounds_.begin(), bounds_.end(), bound,
      [](const ConcessionBound& lhs, const ConcessionBound& rhs) { return lhs.kind < rhs.kind; });
  if (position != bounds_.end() && position->kind == bound.kind) {
    return Status(ErrorCode::AlreadyExists, "duplicate envelope bound");
  }
  bounds_.insert(position, bound);
  return Status{};
}

Status GuaranteeEnvelope::upsert(const ConcessionBound& bound) {
  if (!is_valid(bound.kind)) {
    return Status(ErrorCode::InvalidArgument, "envelope bound has an invalid guarantee kind");
  }
  if (bound.weakest_allowed_value > max_value_for(bound.kind)) {
    return Status(ErrorCode::InvalidArgument, "envelope bound outside the per-kind domain");
  }
  ConcessionBound* existing = nullptr;
  for (ConcessionBound& candidate : bounds_) {
    if (candidate.kind == bound.kind) {
      existing = &candidate;
      break;
    }
  }
  if (existing == nullptr) return insert(bound);
  const bool at_least = comparator_for(bound.kind) == Comparator::AtLeast;
  existing->weakest_allowed_value =
      at_least ? std::min(existing->weakest_allowed_value, bound.weakest_allowed_value)
               : std::max(existing->weakest_allowed_value, bound.weakest_allowed_value);
  return Status{};
}

Status GuaranteeEnvelope::normalize() {
  std::sort(bounds_.begin(), bounds_.end());
  for (std::size_t i = 0; i < bounds_.size(); ++i) {
    if (!is_valid(bounds_[i].kind) || bounds_[i].weakest_allowed_value > max_value_for(bounds_[i].kind)) {
      return Status(ErrorCode::Invalid, "envelope bound outside its domain");
    }
    if (i > 0 && bounds_[i - 1].kind == bounds_[i].kind) {
      return Status(ErrorCode::Invalid, "duplicate envelope bound");
    }
  }
  return Status{};
}

Status GuaranteeEnvelope::validate() const {
  bool first = true;
  GuaranteeKind previous = GuaranteeKind::Availability;
  for (const ConcessionBound& bound : bounds_) {
    if (!is_valid(bound.kind) || bound.weakest_allowed_value > max_value_for(bound.kind)) {
      return Status(ErrorCode::Invalid, "envelope bound outside its domain");
    }
    if (!first && static_cast<std::uint16_t>(bound.kind) <= static_cast<std::uint16_t>(previous)) {
      return Status(ErrorCode::Invalid, "envelope bounds are not canonically ordered");
    }
    previous = bound.kind;
    first = false;
  }
  return Status{};
}

const ConcessionBound* GuaranteeEnvelope::find(GuaranteeKind kind) const noexcept {
  const auto position = std::lower_bound(
      bounds_.begin(), bounds_.end(), kind,
      [](const ConcessionBound& lhs, GuaranteeKind rhs) { return lhs.kind < rhs; });
  if (position == bounds_.end() || position->kind != kind) return nullptr;
  return &(*position);
}

std::uint64_t GuaranteeEnvelope::floor_for(GuaranteeKind kind,
                                           std::uint64_t original_value) const noexcept {
  const ConcessionBound* bound = find(kind);
  if (bound == nullptr) return original_value;
  return bound->weakest_allowed_value;
}

bool GuaranteeEnvelope::permits(const GuaranteeSet& original,
                                const GuaranteeSet& approved) const noexcept {
  if (max_concessions_ == 0) {
    return approved == original;
  }
  std::uint32_t concessions = 0;
  for (const Guarantee& base : original.items()) {
    const Guarantee* proposed = approved.find(base.kind);
    if (proposed == nullptr) return false;  // withdrawals are never envelope-authorised
    if (proposed->value == base.value) continue;
    ++concessions;
    if (concessions > max_concessions_) return false;
    const std::uint64_t floor = floor_for(base.kind, base.value);
    if (base.comparator == Comparator::AtLeast) {
      if (proposed->value < floor) return false;
    } else {
      if (proposed->value > floor) return false;
    }
  }
  for (const Guarantee& proposed : approved.items()) {
    if (original.find(proposed.kind) == nullptr) return false;
  }
  return true;
}

std::uint64_t GuaranteeEnvelope::digest() const noexcept {
  Digest64 digest;
  digest.update_u32(static_cast<std::uint32_t>(bounds_.size()));
  for (const ConcessionBound& bound : bounds_) {
    digest.update_u16(static_cast<std::uint16_t>(bound.kind));
    digest.update_u64(bound.weakest_allowed_value);
  }
  digest.update_u32(max_concessions_);
  return digest.value();
}

Status validate_envelope_against(const GuaranteeEnvelope& envelope,
                                 const GuaranteeSet& original) noexcept {
  Status status = envelope.validate();
  if (!status.ok()) return status;
  status = original.validate();
  if (!status.ok()) return status;
  for (const ConcessionBound& bound : envelope.bounds()) {
    const Guarantee* base = original.find(bound.kind);
    if (base == nullptr) {
      return Status(ErrorCode::Invalid, "envelope names a guarantee the contract does not carry");
    }
    if (base->comparator == Comparator::AtLeast) {
      if (bound.weakest_allowed_value > base->value) {
        return Status(ErrorCode::Invalid, "envelope bound would strengthen an AtLeast guarantee");
      }
    } else {
      if (bound.weakest_allowed_value < base->value) {
        return Status(ErrorCode::Invalid, "envelope bound would strengthen an AtMost guarantee");
      }
    }
  }
  if (envelope.max_concessions() > static_cast<std::uint32_t>(original.size())) {
    return Status(ErrorCode::Invalid, "envelope authorises more concessions than there are guarantees");
  }
  return Status{};
}

}  // namespace dmf
