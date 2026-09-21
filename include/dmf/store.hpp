// Degraded Mode Fabric - versioned, integrity-checked durable state.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The store persists exactly what survives a restart legitimately: definitions
// (contracts), policy, committed lineage, completed outcomes and fences. It
// never resurrects liveness: evidence and live authority read back from disk are
// historical records, not current facts. The caller must fence them explicitly.
#ifndef DMF_STORE_HPP
#define DMF_STORE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dmf/accounting.hpp"
#include "dmf/authority.hpp"
#include "dmf/codec.hpp"
#include "dmf/contract.hpp"
#include "dmf/core.hpp"
#include "dmf/decision.hpp"
#include "dmf/evidence.hpp"
#include "dmf/grant.hpp"
#include "dmf/ids.hpp"
#include "dmf/policy.hpp"

namespace dmf {

/// Every durable mutation has a typed record. The set is append-only: new types
/// receive new numbers and existing numbers never change meaning.
enum class RecordType : std::uint16_t {
  ContractRegistered = 1,
  ContractRetired = 2,
  PolicyInstalled = 3,
  FabricGenerationAdvanced = 4,
  CapacityGenerationAdvanced = 5,
  EvidencePublished = 6,
  GrantIssued = 7,
  GrantAcknowledged = 8,
  GrantApplied = 9,
  GrantFenced = 10,
  GrantExpired = 11,
  GrantRevoked = 12,
  DecisionRecorded = 13,
  BootAdvanced = 14,
  CoordinatorTermAdvanced = 15,
  TornTailObserved = 16,
  RestorationProven = 17,
  RestorationRefused = 18,
  AccountingCheckpoint = 19,
  EvidenceGenerationAdvanced = 20,
  /// Rebinds a still-supportable grant to the current generation set. Only
  /// emitted when revalidation proved the granted obligations are still
  /// serviceable, so it never extends authority the runtime cannot justify.
  GrantRebound = 21,
};

bool is_valid(RecordType value) noexcept;
std::string_view to_string(RecordType value) noexcept;

/// Result of opening a store. Corruption is never silently repaired.
enum class RecoveryOutcome : std::uint16_t {
  FreshStore = 1,
  CleanReopen = 2,
  TornTailRecovered = 3,
  Corrupt = 4,
  VersionUnsupported = 5,
  IntegrityFailure = 6,
  SequenceFailure = 7,
};

bool is_valid(RecoveryOutcome value) noexcept;
std::string_view to_string(RecoveryOutcome value) noexcept;

struct StoreConfig {
  /// Directory that holds every durable artefact. Created when absent.
  std::string root{};
  /// Segment rotation thresholds.
  std::uint64_t max_journal_bytes = 8ULL << 20;
  std::uint64_t max_records_per_segment = 200000;
  std::uint64_t max_snapshot_bytes = kMaxDocumentBytes;
  /// Bounded retention for terminated grants, fences and decisions.
  std::size_t max_retained_grants = 200000;
  std::size_t max_retained_fences = 200000;
  std::size_t max_retained_decisions = 200000;
  std::size_t max_retained_evidence = 1U << 16;
  std::size_t max_retained_contracts = 200000;

  // Fault injection for crash-consistency proofs. When any of these equals the
  // ordinal of a durable append (1-based, counting every record this store has
  // ever written) the process terminates immediately with code 70: no
  // destructors, no unwinding, no flush beyond what already happened. A store
  // configured this way is a test fixture, never a production configuration.
  //
  //   crash_before_record      - before anything reaches the descriptor
  //   crash_after_write_record - after write(2) but before the flush
  //   crash_after_record       - after the flush, before the caller sees a result
  std::uint64_t crash_before_record = 0;
  std::uint64_t crash_after_write_record = 0;
  std::uint64_t crash_after_record = 0;

  /// Opens the store for inspection only: nothing is created, appended,
  /// truncated or removed, so an offline verifier can never change the state it
  /// is describing. Every mutating call then fails with InvalidState.
  bool read_only = false;
};

/// The complete durable projection of the runtime. Everything here is safe to
/// restore; nothing here is live.
struct DurableState {
  BootIncarnation boot{};
  CoordinatorTerm term{};
  Tick last_commit_tick{};
  Policy policy{};
  std::vector<ServiceContract> contracts{};
  std::vector<Grant> grants{};
  std::vector<FenceRecord> fences{};
  std::vector<Decision> decisions{};
  std::vector<RestorationEvaluation> restorations{};
  /// Historical evidence retained for lineage and forensics only. The runtime
  /// must never derive current capability from it after a restart.
  EvidenceVector historical_evidence{};
  EvidenceGeneration evidence_generation{};
  CapacityGeneration capacity_generation{};
  FabricGeneration fabric_generation{};
  AccountingCounters counters{};
  SequenceNumber journal_sequence{};
  std::uint64_t journal_records = 0;
  std::uint64_t boot_count = 0;
  std::uint64_t grant_id_high_water = 0;
  std::uint64_t decision_id_high_water = 0;
  std::uint64_t fence_id_high_water = 0;
  std::uint64_t evidence_id_high_water = 0;
  std::uint64_t plan_id_high_water = 0;

  [[nodiscard]] std::uint64_t live_grant_count() const noexcept;
  [[nodiscard]] std::uint64_t terminated_grant_count() const noexcept;
};

void encode(ByteWriter& writer, const DurableState& value);
Status decode(ByteReader& reader, DurableState& value);

/// Append-only, checksummed, versioned journal plus transactional snapshots.
class StateStore {
 public:
  /// Opens (or creates) the store. Any integrity failure is returned as a
  /// Status; the store is not opened and nothing is truncated.
  static Result<std::unique_ptr<StateStore>> open(const StoreConfig& config);
  ~StateStore();
  StateStore(const StateStore&) = delete;
  StateStore& operator=(const StateStore&) = delete;

  /// Appends a record and makes it durable before returning. This is the only
  /// point at which a mutation becomes committed.
  Status append(RecordType type, const std::vector<std::uint8_t>& payload);
  /// Appends without flushing. The caller must call flush() before treating the
  /// batch as committed.
  Status append_batched(RecordType type, const std::vector<std::uint8_t>& payload);
  Status flush();

  /// True when the active segment has reached a rotation threshold.
  [[nodiscard]] bool rotation_due() const noexcept;
  /// Writes a transactional snapshot and rotates the journal.
  Status take_snapshot();
  /// Writes the segment footer and closes every handle. Idempotent.
  Status close();

  [[nodiscard]] const DurableState& state() const noexcept;
  [[nodiscard]] DurableState& mutable_state() noexcept;
  /// The one accounting facade over the state's counter block. It is shared by
  /// the store, the runtime and the protocol server, and is internally
  /// synchronised, so no counter has two unsynchronised writers.
  [[nodiscard]] Accounting& accounting() noexcept;
  [[nodiscard]] const Accounting& accounting() const noexcept;
  [[nodiscard]] RecoveryOutcome recovery() const noexcept;
  [[nodiscard]] const std::string& recovery_detail() const noexcept;
  [[nodiscard]] const StoreConfig& config() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] std::uint64_t bytes_in_segment() const noexcept;
  [[nodiscard]] std::uint64_t records_in_segment() const noexcept;

 private:
  StateStore();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dmf

#endif  // DMF_STORE_HPP
