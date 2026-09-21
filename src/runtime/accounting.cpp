// Degraded Mode Fabric - accounting implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every counter lives in exactly one block, owned by the durable store, and the
// facade that mutates it is internally synchronised: the coordinator's session
// threads report frame and session activity directly while the coordinator
// itself reports decisions, grants and fences under its own lock, and both reach
// the same block.
#include "dmf/accounting.hpp"

#include <cstdio>

namespace dmf {

bool operator==(const AccountingCounters& a, const AccountingCounters& b) noexcept {
  return a.decisions_total == b.decisions_total && a.decisions_full == b.decisions_full &&
         a.decisions_degraded == b.decisions_degraded && a.decisions_refused == b.decisions_refused &&
         a.decisions_escalated == b.decisions_escalated &&
         a.decisions_unsupported == b.decisions_unsupported &&
         a.decisions_indeterminate == b.decisions_indeterminate &&
         a.decisions_invalid == b.decisions_invalid && a.grants_issued == b.grants_issued &&
         a.grants_acknowledged == b.grants_acknowledged && a.grants_applied == b.grants_applied &&
         a.grants_fenced == b.grants_fenced && a.grants_expired == b.grants_expired &&
         a.grants_revoked == b.grants_revoked && a.grants_pruned == b.grants_pruned &&
         a.grants_released == b.grants_released && a.fences_total == b.fences_total &&
         a.fences_pruned == b.fences_pruned && a.evidence_published == b.evidence_published &&
         a.evidence_rejected == b.evidence_rejected &&
         a.evidence_superseded == b.evidence_superseded && a.plans_total == b.plans_total &&
         a.plans_optimal == b.plans_optimal && a.plans_feasible == b.plans_feasible &&
         a.plans_infeasible == b.plans_infeasible && a.plans_search_limit == b.plans_search_limit &&
         a.allocator_nodes_visited == b.allocator_nodes_visited &&
         a.frames_received == b.frames_received && a.frames_sent == b.frames_sent &&
         a.frames_rejected == b.frames_rejected && a.frames_oversized == b.frames_oversized &&
         a.sessions_opened == b.sessions_opened && a.sessions_closed == b.sessions_closed &&
         a.sessions_rejected == b.sessions_rejected &&
         a.requests_rejected == b.requests_rejected &&
         a.journal_records_written == b.journal_records_written &&
         a.journal_records_read == b.journal_records_read &&
         a.journal_bytes_written == b.journal_bytes_written &&
         a.snapshots_taken == b.snapshots_taken && a.snapshots_loaded == b.snapshots_loaded &&
         a.torn_tails_recovered == b.torn_tails_recovered && a.boots == b.boots &&
         a.term_advances == b.term_advances && a.restorations_proven == b.restorations_proven &&
         a.restorations_refused == b.restorations_refused;
}

Accounting::Accounting(AccountingCounters& counters) noexcept
    : mutable_(&counters), readonly_(&counters) {}

Accounting::Accounting(const AccountingCounters& counters) noexcept : readonly_(&counters) {}

Status Accounting::bump(std::uint64_t& counter, std::uint64_t delta) {
  if (mutable_ == nullptr) {
    return Status(ErrorCode::InvalidState, "accounting counter block is read only");
  }
  const auto next = checked_add(counter, delta);
  if (!next.has_value()) {
    return Status(ErrorCode::Overflow, "accounting counter overflow");
  }
  counter = *next;
  return Status{};
}

AccountingCounters Accounting::counters() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return *readonly_;
}

Status Accounting::replace(const AccountingCounters& counters) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (mutable_ == nullptr) {
    return Status(ErrorCode::InvalidState, "accounting counter block is read only");
  }
  *mutable_ = counters;
  return Status{};
}

Status Accounting::record_decision(DecisionOutcome outcome) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Status status = bump(mutable_->decisions_total, 1);
  if (!status.ok()) return status;
  switch (outcome) {
    case DecisionOutcome::Full: return bump(mutable_->decisions_full, 1);
    case DecisionOutcome::Degraded: return bump(mutable_->decisions_degraded, 1);
    case DecisionOutcome::Refused: return bump(mutable_->decisions_refused, 1);
    case DecisionOutcome::Escalated: return bump(mutable_->decisions_escalated, 1);
    case DecisionOutcome::Unsupported: return bump(mutable_->decisions_unsupported, 1);
    case DecisionOutcome::Indeterminate: return bump(mutable_->decisions_indeterminate, 1);
    case DecisionOutcome::Invalid: return bump(mutable_->decisions_invalid, 1);
  }
  return Status(ErrorCode::InvalidEnum, "unknown decision outcome");
}

Status Accounting::record_plan(PlanStatus status, std::uint64_t nodes_visited) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Status result = bump(mutable_->plans_total, 1);
  if (!result.ok()) return result;
  result = bump(mutable_->allocator_nodes_visited, nodes_visited);
  if (!result.ok()) return result;
  switch (status) {
    case PlanStatus::Optimal: return bump(mutable_->plans_optimal, 1);
    case PlanStatus::Feasible: return bump(mutable_->plans_feasible, 1);
    case PlanStatus::ProvenInfeasible: return bump(mutable_->plans_infeasible, 1);
    case PlanStatus::SearchLimitReached: return bump(mutable_->plans_search_limit, 1);
    case PlanStatus::Indeterminate:
    case PlanStatus::Invalid:
      return Status{};
  }
  return Status{};
}

Status Accounting::record_grant_issued() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_issued, 1);
}
Status Accounting::record_grant_acknowledged() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_acknowledged, 1);
}
Status Accounting::record_grant_applied() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_applied, 1);
}
Status Accounting::record_grant_fenced() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_fenced, 1);
}
Status Accounting::record_grant_expired() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_expired, 1);
}
Status Accounting::record_grant_revoked() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_revoked, 1);
}
Status Accounting::record_grant_pruned(std::uint64_t count) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->grants_pruned, count);
}
Status Accounting::record_fences(std::uint64_t count) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->fences_total, count);
}
Status Accounting::record_fences_pruned(std::uint64_t count) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->fences_pruned, count);
}
Status Accounting::record_evidence_published() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->evidence_published, 1);
}
Status Accounting::record_evidence_rejected() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->evidence_rejected, 1);
}
Status Accounting::record_evidence_superseded(std::uint64_t count) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->evidence_superseded, count);
}
Status Accounting::record_frame_received() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->frames_received, 1);
}
Status Accounting::record_frame_sent() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->frames_sent, 1);
}
Status Accounting::record_frame_rejected(bool oversized) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Status status = bump(mutable_->frames_rejected, 1);
  if (!status.ok()) return status;
  if (oversized) return bump(mutable_->frames_oversized, 1);
  return Status{};
}
Status Accounting::record_session_opened() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->sessions_opened, 1);
}
Status Accounting::record_session_closed() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->sessions_closed, 1);
}
Status Accounting::record_session_rejected() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->sessions_rejected, 1);
}
Status Accounting::record_request_rejected() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->requests_rejected, 1);
}
Status Accounting::record_journal_write(std::uint64_t records, std::uint64_t bytes) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Status status = bump(mutable_->journal_records_written, records);
  if (!status.ok()) return status;
  return bump(mutable_->journal_bytes_written, bytes);
}
Status Accounting::record_journal_read(std::uint64_t records) {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->journal_records_read, records);
}
Status Accounting::record_snapshot_taken() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->snapshots_taken, 1);
}
Status Accounting::record_snapshot_loaded() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->snapshots_loaded, 1);
}
Status Accounting::record_torn_tail() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->torn_tails_recovered, 1);
}
Status Accounting::record_boot() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->boots, 1);
}
Status Accounting::record_term_advance() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->term_advances, 1);
}
Status Accounting::record_restoration_proven() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->restorations_proven, 1);
}
Status Accounting::record_restoration_refused() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bump(mutable_->restorations_refused, 1);
}

namespace {

void add_violation(ClosureReport& report, const char* identity, std::uint64_t expected,
                   std::uint64_t observed) {
  if (report.violations.size() >= 32) return;
  ClosureViolation violation;
  violation.identity = identity;
  violation.expected = expected;
  violation.observed = observed;
  report.violations.push_back(std::move(violation));
}

bool equal_or(ClosureReport& report, const char* identity, std::uint64_t expected,
              std::uint64_t observed) {
  if (expected == observed) return true;
  add_violation(report, identity, expected, observed);
  return false;
}

}  // namespace

ClosureReport Accounting::check_closure(const ClosureInputs& inputs) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const AccountingCounters& counts = *readonly_;
  ClosureReport report;
  bool ok = true;

  const std::uint64_t decision_sum =
      counts.decisions_full + counts.decisions_degraded + counts.decisions_refused +
      counts.decisions_escalated + counts.decisions_unsupported +
      counts.decisions_indeterminate + counts.decisions_invalid;
  ok &= equal_or(report, "decisions_total == sum(by outcome)", counts.decisions_total, decision_sum);

  const std::uint64_t plan_sum = counts.plans_optimal + counts.plans_feasible +
                                 counts.plans_infeasible + counts.plans_search_limit;
  if (plan_sum > counts.plans_total) {
    add_violation(report, "plan outcomes exceed plans_total", counts.plans_total, plan_sum);
    ok = false;
  }

  const std::uint64_t terminated =
      counts.grants_fenced + counts.grants_expired + counts.grants_revoked;
  ok &= equal_or(report, "grants_issued == live + retained_terminated + pruned",
                 counts.grants_issued,
                 inputs.live_grants + inputs.retained_terminated_grants + counts.grants_pruned);
  ok &= equal_or(report, "grants_terminated == retained_terminated + pruned", terminated,
                 inputs.retained_terminated_grants + counts.grants_pruned);
  ok &= equal_or(report, "fences_total == retained_fences + fences_pruned", counts.fences_total,
                 inputs.retained_fences + counts.fences_pruned);

  if (counts.grants_acknowledged > counts.grants_issued) {
    add_violation(report, "acknowledgements exceed issued grants", counts.grants_issued,
                  counts.grants_acknowledged);
    ok = false;
  }
  if (counts.grants_applied > counts.grants_acknowledged) {
    add_violation(report, "applications exceed acknowledgements", counts.grants_acknowledged,
                  counts.grants_applied);
    ok = false;
  }
  if (counts.fences_total < terminated) {
    add_violation(report, "fewer fences than terminated grants", terminated, counts.fences_total);
    ok = false;
  }
  if (counts.sessions_closed > counts.sessions_opened) {
    add_violation(report, "sessions closed exceed sessions opened", counts.sessions_opened,
                  counts.sessions_closed);
    ok = false;
  }
  if (counts.frames_oversized > counts.frames_rejected) {
    add_violation(report, "oversized frames exceed rejected frames", counts.frames_rejected,
                  counts.frames_oversized);
    ok = false;
  }
  report.closed = ok;
  return report;
}

std::string ClosureReport::render() const {
  if (closed) return "accounting closure holds";
  std::string out = "accounting closure violated:";
  for (const ClosureViolation& violation : violations) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "\n    %s expected=%llu observed=%llu",
                  violation.identity.c_str(), static_cast<unsigned long long>(violation.expected),
                  static_cast<unsigned long long>(violation.observed));
    out.append(buffer);
  }
  return out;
}

void encode(ByteWriter& writer, const AccountingCounters& value) {
  const std::uint64_t fields[] = {
      value.decisions_total,       value.decisions_full,        value.decisions_degraded,
      value.decisions_refused,     value.decisions_escalated,   value.decisions_unsupported,
      value.decisions_indeterminate, value.decisions_invalid,   value.grants_issued,
      value.grants_acknowledged,   value.grants_applied,        value.grants_fenced,
      value.grants_expired,        value.grants_revoked,        value.grants_pruned,
      value.grants_released,       value.fences_total,          value.fences_pruned,
      value.evidence_published,    value.evidence_rejected,     value.evidence_superseded,
      value.plans_total,           value.plans_optimal,         value.plans_feasible,
      value.plans_infeasible,      value.plans_search_limit,    value.allocator_nodes_visited,
      value.frames_received,       value.frames_sent,           value.frames_rejected,
      value.frames_oversized,      value.sessions_opened,       value.sessions_closed,
      value.sessions_rejected,     value.requests_rejected,     value.journal_records_written,
      value.journal_records_read,  value.journal_bytes_written, value.snapshots_taken,
      value.snapshots_loaded,      value.torn_tails_recovered,  value.boots,
      value.term_advances,         value.restorations_proven,   value.restorations_refused};
  static_assert(sizeof(fields) / sizeof(fields[0]) == 45, "accounting field count changed");
  for (const std::uint64_t field : fields) writer.u64(field);
}

Status decode(ByteReader& reader, AccountingCounters& value) {
  AccountingCounters candidate;
  std::uint64_t* fields[] = {
      &candidate.decisions_total,       &candidate.decisions_full,
      &candidate.decisions_degraded,    &candidate.decisions_refused,
      &candidate.decisions_escalated,   &candidate.decisions_unsupported,
      &candidate.decisions_indeterminate, &candidate.decisions_invalid,
      &candidate.grants_issued,         &candidate.grants_acknowledged,
      &candidate.grants_applied,        &candidate.grants_fenced,
      &candidate.grants_expired,        &candidate.grants_revoked,
      &candidate.grants_pruned,         &candidate.grants_released,
      &candidate.fences_total,          &candidate.fences_pruned,
      &candidate.evidence_published,    &candidate.evidence_rejected,
      &candidate.evidence_superseded,   &candidate.plans_total,
      &candidate.plans_optimal,         &candidate.plans_feasible,
      &candidate.plans_infeasible,      &candidate.plans_search_limit,
      &candidate.allocator_nodes_visited, &candidate.frames_received,
      &candidate.frames_sent,           &candidate.frames_rejected,
      &candidate.frames_oversized,      &candidate.sessions_opened,
      &candidate.sessions_closed,       &candidate.sessions_rejected,
      &candidate.requests_rejected,     &candidate.journal_records_written,
      &candidate.journal_records_read,  &candidate.journal_bytes_written,
      &candidate.snapshots_taken,       &candidate.snapshots_loaded,
      &candidate.torn_tails_recovered,  &candidate.boots,
      &candidate.term_advances,         &candidate.restorations_proven,
      &candidate.restorations_refused};
  for (std::uint64_t* field : fields) *field = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

}  // namespace dmf
