// Degraded Mode Fabric - live evidence frontier.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>

#include "dmf/runtime.hpp"

namespace dmf {

EvidenceStore::EvidenceStore(std::size_t limit) : live_(limit) {}

Status EvidenceStore::publish(const EvidenceItem& item, bool& superseded) {
  superseded = false;
  Status status = item.validate();
  if (!status.ok()) return status;

  // Publisher incarnation ordering: a previous incarnation's observation is
  // stale forever, because the process that made it no longer exists.
  const auto known = publisher_incarnation_.find(item.publisher);
  if (known != publisher_incarnation_.end()) {
    if (item.publisher_boot < known->second) {
      return Status(ErrorCode::Stale,
                    "evidence comes from a superseded publisher incarnation");
    }
    if (known->second < item.publisher_boot) {
      // A fresh incarnation replaces everything the old one contributed.
      superseded = true;
    }
  }

  EvidenceVector rebuilt(live_.limit());
  for (const EvidenceItem& existing : live_.items()) {
    const bool replaced = existing.scope == item.scope && existing.kind == item.kind &&
                          (existing.publisher == item.publisher ||
                           (known != publisher_incarnation_.end() &&
                            known->second < item.publisher_boot &&
                            existing.publisher == item.publisher));
    if (replaced) {
      if (existing.publisher == item.publisher) superseded = true;
      continue;
    }
    status = rebuilt.push(existing);
    if (!status.ok()) return status;
  }
  status = rebuilt.push(item);
  if (!status.ok()) {
    if (status.code() == ErrorCode::CapacityExceeded) {
      return Status(ErrorCode::CapacityExceeded, "evidence table is full");
    }
    return status;
  }
  status = rebuilt.normalize();
  if (!status.ok()) return status;
  live_ = rebuilt;
  if (known == publisher_incarnation_.end() || known->second < item.publisher_boot) {
    publisher_incarnation_[item.publisher] = item.publisher_boot;
  }
  return Status{};
}

void EvidenceStore::clear() noexcept {
  live_ = EvidenceVector(live_.limit());
}

Result<CapabilitySnapshot> EvidenceStore::capability(ScopeId scope,
                                                     CapacityGeneration generation,
                                                     const FreshnessWindow& freshness) const {
  return derive_capability(live_, scope, generation, freshness);
}

bool EvidenceStore::has_scope(ScopeId scope) const noexcept {
  for (const EvidenceItem& item : live_.items()) {
    if (item.scope == scope) return true;
  }
  return false;
}

GrantTable::GrantTable(std::size_t max_live) : max_live_(max_live) {}

void GrantTable::rebuild(const std::vector<Grant>& grants) {
  index_.clear();
  live_ids_.clear();
  index_.reserve(grants.size());
  for (std::size_t i = 0; i < grants.size(); ++i) {
    index_.emplace(grants[i].id, i);
    if (grants[i].live()) live_ids_.push_back(grants[i].id);
  }
  std::sort(live_ids_.begin(), live_ids_.end());
}

Status GrantTable::note_insert(const Grant& grant) {
  if (index_.find(grant.id) != index_.end()) {
    return Status(ErrorCode::AlreadyExists, "grant identity was reused");
  }
  if (grant.live() && live_ids_.size() >= max_live_) {
    return Status(ErrorCode::Exhausted, "live grant table is full");
  }
  index_.emplace(grant.id, index_.size());
  if (grant.live()) {
    live_ids_.push_back(grant.id);
    std::sort(live_ids_.begin(), live_ids_.end());
  }
  return Status{};
}

Status GrantTable::note_transition(GrantId id, GrantState from, GrantState to) {
  if (index_.find(id) == index_.end()) {
    return Status(ErrorCode::NotFound, "grant is not indexed");
  }
  if (grant_state_live(from) && grant_state_terminal(to)) {
    const auto position = std::lower_bound(live_ids_.begin(), live_ids_.end(), id);
    if (position != live_ids_.end() && *position == id) {
      live_ids_.erase(position);
    }
  }
  return Status{};
}

const Grant* GrantTable::find(const std::vector<Grant>& grants, GrantId id) const noexcept {
  const auto position = index_.find(id);
  if (position == index_.end() || position->second >= grants.size()) return nullptr;
  return &grants[position->second];
}

Grant* GrantTable::find(std::vector<Grant>& grants, GrantId id) const noexcept {
  const auto position = index_.find(id);
  if (position == index_.end() || position->second >= grants.size()) return nullptr;
  return &grants[position->second];
}

}  // namespace dmf
