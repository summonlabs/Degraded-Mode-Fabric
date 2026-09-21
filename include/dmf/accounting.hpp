// Degraded Mode Fabric - exact accounting and closure.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every counter is monotone and every increment is overflow-checked. Closure
// identities are re-checked after restart, so an accounting defect cannot hide
// behind a process boundary.
#ifndef DMF_ACCOUNTING_HPP
#define DMF_ACCOUNTING_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "dmf/authority.hpp"
#include "dmf/core.hpp"
#include "dmf/reason.hpp"

namespace dmf {

struct AccountingCounters {
  std::uint64_t decisions_total = 0;
  std::uint64_t decisions_full = 0;
  std::uint64_t decisions_degraded = 0;
  std::uint64_t decisions_refused = 0;
  std::uint64_t decisions_escalated = 0;
  std::uint64_t decisions_unsupported = 0;
  std::uint64_t decisions_indeterminate = 0;
  std::uint64_t decisions_invalid = 0;

  std::uint64_t grants_issued = 0;
  std::uint64_t grants_acknowledged = 0;
  std::uint64_t grants_applied = 0;
  std::uint64_t grants_fenced = 0;
  std::uint64_t grants_expired = 0;
  std::uint64_t grants_revoked = 0;
  std::uint64_t grants_pruned = 0;
  std::uint64_t grants_released = 0;

  std::uint64_t fences_total = 0;
  std::uint64_t fences_pruned = 0;

  std::uint64_t evidence_published = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t evidence_superseded = 0;

  std::uint64_t plans_total = 0;
  std::uint64_t plans_optimal = 0;
  std::uint64_t plans_feasible = 0;
  std::uint64_t plans_infeasible = 0;
  std::uint64_t plans_search_limit = 0;
  std::uint64_t allocator_nodes_visited = 0;

  std::uint64_t frames_received = 0;
  std::uint64_t frames_sent = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t frames_oversized = 0;
  std::uint64_t sessions_opened = 0;
  std::uint64_t sessions_closed = 0;
  std::uint64_t sessions_rejected = 0;
  std::uint64_t requests_rejected = 0;

  std::uint64_t journal_records_written = 0;
  std::uint64_t journal_records_read = 0;
  std::uint64_t journal_bytes_written = 0;
  std::uint64_t snapshots_taken = 0;
  std::uint64_t snapshots_loaded = 0;
  std::uint64_t torn_tails_recovered = 0;
  std::uint64_t boots = 0;
  std::uint64_t term_advances = 0;
  std::uint64_t restorations_proven = 0;
  std::uint64_t restorations_refused = 0;

  friend bool operator==(const AccountingCounters& a, const AccountingCounters& b) noexcept;
  friend bool operator!=(const AccountingCounters& a, const AccountingCounters& b) noexcept {
    return !(a == b);
  }
};

/// One closure identity was violated.
struct ClosureViolation {
  std::string identity{};
  std::uint64_t expected = 0;
  std::uint64_t observed = 0;
};

struct ClosureReport {
  bool closed = false;
  std::vector<ClosureViolation> violations{};

  [[nodiscard]] std::string render() const;
};

/// Live-state inputs the closure identities cannot derive from counters alone.
struct ClosureInputs {
  std::uint64_t live_grants = 0;
  std::uint64_t retained_terminated_grants = 0;
  std::uint64_t retained_fences = 0;
  std::uint64_t retained_decisions = 0;
};

/// Applies checked increments to a counter set and verifies closure identities.
/// On overflow the increment is refused and the counters are left unchanged.
///
/// The facade does not own the counters: the single authoritative counter block
/// lives in the durable state, so store-level and runtime-level accounting can
/// never diverge. A const-constructed instance refuses every mutation.
class Accounting {
 public:
  explicit Accounting(AccountingCounters& counters) noexcept;
  explicit Accounting(const AccountingCounters& counters) noexcept;
  Accounting(const Accounting&) = delete;
  Accounting& operator=(const Accounting&) = delete;
  Accounting(Accounting&&) = delete;
  Accounting& operator=(Accounting&&) = delete;

  [[nodiscard]] Status record_decision(DecisionOutcome outcome);
  [[nodiscard]] Status record_plan(PlanStatus status, std::uint64_t nodes_visited);
  [[nodiscard]] Status record_grant_issued();
  [[nodiscard]] Status record_grant_acknowledged();
  [[nodiscard]] Status record_grant_applied();
  [[nodiscard]] Status record_grant_fenced();
  [[nodiscard]] Status record_grant_expired();
  [[nodiscard]] Status record_grant_revoked();
  [[nodiscard]] Status record_grant_pruned(std::uint64_t count);
  [[nodiscard]] Status record_fences(std::uint64_t count);
  [[nodiscard]] Status record_fences_pruned(std::uint64_t count);
  [[nodiscard]] Status record_evidence_published();
  [[nodiscard]] Status record_evidence_rejected();
  [[nodiscard]] Status record_evidence_superseded(std::uint64_t count);
  [[nodiscard]] Status record_frame_received();
  [[nodiscard]] Status record_frame_sent();
  [[nodiscard]] Status record_frame_rejected(bool oversized);
  [[nodiscard]] Status record_session_opened();
  [[nodiscard]] Status record_session_closed();
  [[nodiscard]] Status record_session_rejected();
  [[nodiscard]] Status record_request_rejected();
  [[nodiscard]] Status record_journal_write(std::uint64_t records, std::uint64_t bytes);
  [[nodiscard]] Status record_journal_read(std::uint64_t records);
  [[nodiscard]] Status record_snapshot_taken();
  [[nodiscard]] Status record_snapshot_loaded();
  [[nodiscard]] Status record_torn_tail();
  [[nodiscard]] Status record_boot();
  [[nodiscard]] Status record_term_advance();
  [[nodiscard]] Status record_restoration_proven();
  [[nodiscard]] Status record_restoration_refused();

  /// Returns a consistent snapshot. The counter block is shared with every
  /// thread that reports store or protocol activity, so a reference is never
  /// handed out.
  [[nodiscard]] AccountingCounters counters() const;

  /// Null when this instance was constructed from a read-only counter block.
  [[nodiscard]] AccountingCounters* mutable_counters() noexcept { return mutable_; }

  /// Replaces the whole counter block, e.g. when an accounting checkpoint record
  /// is applied. Refused on a read-only instance.
  [[nodiscard]] Status replace(const AccountingCounters& counters);

  /// Verifies every closure identity. The live-grant input must equal the
  /// number of non-terminal grants currently retained.
  [[nodiscard]] ClosureReport check_closure(const ClosureInputs& inputs) const;

 private:
  [[nodiscard]] Status bump(std::uint64_t& counter, std::uint64_t delta);

  AccountingCounters* mutable_ = nullptr;
  const AccountingCounters* readonly_ = nullptr;
  mutable std::mutex mutex_{};
};

void encode(ByteWriter& writer, const AccountingCounters& value);
Status decode(ByteReader& reader, AccountingCounters& value);

}  // namespace dmf

#endif  // DMF_ACCOUNTING_HPP
