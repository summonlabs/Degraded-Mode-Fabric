// Degraded Mode Fabric - coordinator runtime.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The coordinator owns the single authoritative view of the fabric: coordinator
// term, boot incarnation, generations, policy, contracts, evidence, grants and
// fences. Every externally visible answer is produced here and every committed
// mutation reaches the journal before it is reported.
#ifndef DMF_RUNTIME_HPP
#define DMF_RUNTIME_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dmf/accounting.hpp"
#include "dmf/authority.hpp"
#include "dmf/contract.hpp"
#include "dmf/core.hpp"
#include "dmf/decision.hpp"
#include "dmf/engine.hpp"
#include "dmf/evidence.hpp"
#include "dmf/grant.hpp"
#include "dmf/ids.hpp"
#include "dmf/net.hpp"
#include "dmf/policy.hpp"
#include "dmf/store.hpp"

namespace dmf {

/// Role of an authenticated session. Roles are ordered and a session's role is
/// a property of the bearer token it presented, never of a claim it made.
enum class PrincipalRole : std::uint16_t {
  Observer = 1,
  Operator = 2,
  Administrator = 3,
};

bool is_valid(PrincipalRole value) noexcept;
std::string_view to_string(PrincipalRole value) noexcept;

/// Live evidence frontier. Cleared on every boot: a durable observation is
/// history, never current capability.
class EvidenceStore {
 public:
  explicit EvidenceStore(std::size_t limit = 1U << 14);
  EvidenceStore(const EvidenceStore&) = delete;
  EvidenceStore& operator=(const EvidenceStore&) = delete;

  /// Records one observation.
  ///
  /// Ordering rules, all enforced here rather than hoped for downstream:
  ///   * an item from an older boot incarnation of the same publisher is
  ///     refused as STALE, because a previous incarnation's observation can
  ///     never become current again;
  ///   * an item from a newer incarnation supersedes every item the publisher
  ///     previously contributed, regardless of generation;
  ///   * within one incarnation the higher generation wins.
  Status publish(const EvidenceItem& item, bool& superseded);
  /// Drops every live item. Called on boot and on an explicit reset.
  void clear() noexcept;

  /// Replaces the retention bound. Only legitimate before the first publish.
  void reset_limit(std::size_t limit) noexcept { live_ = EvidenceVector(limit); }

  [[nodiscard]] const EvidenceVector& live() const noexcept { return live_; }
  [[nodiscard]] std::size_t size() const noexcept { return live_.size(); }
  [[nodiscard]] std::size_t limit() const noexcept { return live_.limit(); }
  [[nodiscard]] Result<CapabilitySnapshot> capability(ScopeId scope,
                                                      CapacityGeneration generation,
                                                      const FreshnessWindow& freshness) const;
  /// True when at least one item is held for the scope.
  [[nodiscard]] bool has_scope(ScopeId scope) const noexcept;

 private:
  EvidenceVector live_{};
  /// Highest publisher incarnation observed per publisher. A lower incarnation
  /// is stale forever, which is what makes a publisher restart safe.
  std::unordered_map<PublisherId, BootIncarnation> publisher_incarnation_{};
};

/// Index over the durable grant table. The authoritative grant records live in
/// DurableState; this table only makes lookup and accounting cheap.
class GrantTable {
 public:
  explicit GrantTable(std::size_t max_live = 200000);

  /// Rebuilds the index from the durable table, e.g. after a snapshot pruned it.
  void rebuild(const std::vector<Grant>& grants);
  [[nodiscard]] Status note_insert(const Grant& grant);
  [[nodiscard]] Status note_transition(GrantId id, GrantState from, GrantState to);
  [[nodiscard]] const Grant* find(const std::vector<Grant>& grants, GrantId id) const noexcept;
  [[nodiscard]] Grant* find(std::vector<Grant>& grants, GrantId id) const noexcept;
  [[nodiscard]] const std::vector<GrantId>& live_ids() const noexcept { return live_ids_; }

  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t live_count() const noexcept { return live_ids_.size(); }
  [[nodiscard]] std::size_t terminated_count() const noexcept {
    return index_.size() - live_ids_.size();
  }
  [[nodiscard]] std::size_t max_live() const noexcept { return max_live_; }

 private:
  std::unordered_map<GrantId, std::size_t> index_{};
  std::vector<GrantId> live_ids_{};
  std::size_t max_live_;
};

/// Result of a revalidation sweep.
struct RevalidateReport {
  std::uint64_t evaluated = 0;
  std::uint64_t fenced = 0;
  std::uint64_t expired = 0;
};

struct CoordinatorConfig {
  StoreConfig store{};
  ServerConfig server{};
  std::size_t max_evidence_items = 1U << 14;
  std::size_t max_live_grants = 200000;
  std::size_t max_contracts = 200000;
  std::size_t max_decisions = 200000;
  std::uint64_t snapshot_interval_records = 8192;
  /// Bearer token to role binding. A session's role is a property of the token
  /// it presents, never of the role string it claims, so a client cannot promote
  /// itself by asking. A token that is not listed is refused, and an empty map
  /// refuses every session.
  std::map<std::string, PrincipalRole> principals{};
  /// Single-process test harnesses only. It admits every session as an
  /// administrator and is never legitimate for a deployment.
  bool allow_anonymous_sessions = false;
  OriginClass origin = OriginClass::Synthetic;
};

/// The runtime. Every public method except state() is safe to call from any
/// thread: all mutable state is guarded by one non-recursive mutex, and no
/// callback, socket write or thread join is ever performed while it is held.
class Coordinator final : public SessionHandler {
 public:
  /// Opaque implementation type. Declared publicly so the translation unit's
  /// helpers can name it; its definition never leaves that translation unit.
  struct Impl;

  static Result<std::unique_ptr<Coordinator>> create(const CoordinatorConfig& config);
  ~Coordinator() override;
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  /// Starts the framed server. Fails when the port cannot be bound.
  Status start();
  /// Stops the server, snapshots the durable state and closes the store.
  Status stop();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // ---- direct control surface (used by tools, tests and the selftest) ----
  [[nodiscard]] Status install_policy(const Policy& policy);
  [[nodiscard]] Status register_contract(const ServiceContract& contract);
  /// Registers many definitions as one durable batch: every record is written,
  /// then a single flush commits the batch. Nothing is visible until the flush
  /// returns, so the batch is all-or-nothing at the durability boundary.
  [[nodiscard]] Status register_contracts(const std::vector<ServiceContract>& contracts);
  [[nodiscard]] Status publish_evidence(const EvidenceItem& item);
  [[nodiscard]] Result<Decision> evaluate(ContractId contract);
  /// The same evaluation, plus the whole-scope allocation it was derived from.
  [[nodiscard]] Result<EvaluationView> evaluate_with_plan(ContractId contract);
  [[nodiscard]] Result<Grant> acquire(ContractId contract, Tick now);
  [[nodiscard]] Result<Grant> acknowledge(GrantId grant, AttemptId attempt, Tick now);
  [[nodiscard]] Result<Grant> report_applied(GrantId grant, AttemptId attempt, Tick now);
  [[nodiscard]] Result<FenceRecord> fence(GrantId grant, FenceReason reason, Tick now);
  [[nodiscard]] Result<RestorationEvaluation> request_restoration(GrantId grant, Tick now);
  [[nodiscard]] RevalidateReport revalidate(Tick now);
  [[nodiscard]] StatusBody describe(Tick now) const;
  [[nodiscard]] Result<CapabilitySnapshot> capability(ScopeId scope, Tick now) const;

  /// Direct inspection of the durable projection. Together with recovery() this
  /// is one of the two accessors that take no lock: both are safe only when the
  /// caller is the sole thread using the coordinator (a single-threaded harness,
  /// or after stop()). Concurrent callers must use describe(), evaluate() or
  /// the query surface instead.
  [[nodiscard]] const DurableState& state() const;
  [[nodiscard]] AuthorityVector authority() const;
  [[nodiscard]] AccountingCounters counters() const;
  /// The recovery outcome of the last open. Not synchronised, for the same
  /// reason as state().
  [[nodiscard]] RecoveryOutcome recovery() const noexcept;

  /// Advances the runtime clock. The coordinator never reads wall-clock time.
  Tick advance(std::uint64_t delta);
  [[nodiscard]] Tick now() const;

  // ---- SessionHandler ----
  Status on_hello(SessionId session, const HelloRequest& request, HelloResponse& response) override;
  Status on_request(SessionId session, const RequestEnvelope& envelope, MessageType type,
                    const std::vector<std::uint8_t>& body, ResponseEnvelope& response,
                    std::vector<std::uint8_t>& response_body) override;
  void on_session_closed(SessionId session) override;

 private:
  Coordinator();
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
};

}  // namespace dmf

#endif  // DMF_RUNTIME_HPP
