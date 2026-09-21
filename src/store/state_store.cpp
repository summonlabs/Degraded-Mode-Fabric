// Degraded Mode Fabric - durable state store.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>

#include "store/file_io.hpp"
#include "store/journal.hpp"
#include "store/snapshot.hpp"

namespace dmf {

bool is_valid(RecordType value) noexcept {
  return static_cast<std::uint16_t>(value) >= 1U && static_cast<std::uint16_t>(value) <= 21U;
}

std::string_view to_string(RecordType value) noexcept {
  switch (value) {
    case RecordType::ContractRegistered: return "CONTRACT_REGISTERED";
    case RecordType::ContractRetired: return "CONTRACT_RETIRED";
    case RecordType::PolicyInstalled: return "POLICY_INSTALLED";
    case RecordType::FabricGenerationAdvanced: return "FABRIC_GENERATION_ADVANCED";
    case RecordType::CapacityGenerationAdvanced: return "CAPACITY_GENERATION_ADVANCED";
    case RecordType::EvidencePublished: return "EVIDENCE_PUBLISHED";
    case RecordType::GrantIssued: return "GRANT_ISSUED";
    case RecordType::GrantAcknowledged: return "GRANT_ACKNOWLEDGED";
    case RecordType::GrantApplied: return "GRANT_APPLIED";
    case RecordType::GrantFenced: return "GRANT_FENCED";
    case RecordType::GrantExpired: return "GRANT_EXPIRED";
    case RecordType::GrantRevoked: return "GRANT_REVOKED";
    case RecordType::DecisionRecorded: return "DECISION_RECORDED";
    case RecordType::BootAdvanced: return "BOOT_ADVANCED";
    case RecordType::CoordinatorTermAdvanced: return "COORDINATOR_TERM_ADVANCED";
    case RecordType::TornTailObserved: return "TORN_TAIL_OBSERVED";
    case RecordType::RestorationProven: return "RESTORATION_PROVEN";
    case RecordType::RestorationRefused: return "RESTORATION_REFUSED";
    case RecordType::AccountingCheckpoint: return "ACCOUNTING_CHECKPOINT";
    case RecordType::EvidenceGenerationAdvanced: return "EVIDENCE_GENERATION_ADVANCED";
    case RecordType::GrantRebound: return "GRANT_REBOUND";
  }
  return "UNRECOGNISED";
}

bool is_valid(RecoveryOutcome value) noexcept {
  switch (value) {
    case RecoveryOutcome::FreshStore:
    case RecoveryOutcome::CleanReopen:
    case RecoveryOutcome::TornTailRecovered:
    case RecoveryOutcome::Corrupt:
    case RecoveryOutcome::VersionUnsupported:
    case RecoveryOutcome::IntegrityFailure:
    case RecoveryOutcome::SequenceFailure:
      return true;
  }
  return false;
}

std::string_view to_string(RecoveryOutcome value) noexcept {
  switch (value) {
    case RecoveryOutcome::FreshStore: return "FRESH_STORE";
    case RecoveryOutcome::CleanReopen: return "CLEAN_REOPEN";
    case RecoveryOutcome::TornTailRecovered: return "TORN_TAIL_RECOVERED";
    case RecoveryOutcome::Corrupt: return "CORRUPT";
    case RecoveryOutcome::VersionUnsupported: return "VERSION_UNSUPPORTED";
    case RecoveryOutcome::IntegrityFailure: return "INTEGRITY_FAILURE";
    case RecoveryOutcome::SequenceFailure: return "SEQUENCE_FAILURE";
  }
  return "UNRECOGNISED";
}

std::uint64_t DurableState::live_grant_count() const noexcept {
  std::uint64_t count = 0;
  for (const Grant& grant : grants) {
    if (grant.live()) ++count;
  }
  return count;
}

std::uint64_t DurableState::terminated_grant_count() const noexcept {
  std::uint64_t count = 0;
  for (const Grant& grant : grants) {
    if (!grant.live()) ++count;
  }
  return count;
}

void encode(ByteWriter& writer, const DurableState& value) {
  writer.u64(value.boot.value());
  writer.u64(value.term.value());
  writer.u64(value.last_commit_tick.value);
  encode(writer, value.policy);
  writer.u32(static_cast<std::uint32_t>(value.contracts.size()));
  for (const ServiceContract& contract : value.contracts) encode(writer, contract);
  writer.u32(static_cast<std::uint32_t>(value.grants.size()));
  for (const Grant& grant : value.grants) encode(writer, grant);
  writer.u32(static_cast<std::uint32_t>(value.fences.size()));
  for (const FenceRecord& fence : value.fences) encode(writer, fence);
  writer.u32(static_cast<std::uint32_t>(value.decisions.size()));
  for (const Decision& decision : value.decisions) encode(writer, decision);
  writer.u32(static_cast<std::uint32_t>(value.restorations.size()));
  for (const RestorationEvaluation& restoration : value.restorations) encode(writer, restoration);
  encode(writer, value.historical_evidence);
  writer.u64(value.evidence_generation.value());
  writer.u64(value.capacity_generation.value());
  writer.u64(value.fabric_generation.value());
  encode(writer, value.counters);
  writer.u64(value.journal_sequence.value());
  writer.u64(value.journal_records);
  writer.u64(value.boot_count);
  writer.u64(value.grant_id_high_water);
  writer.u64(value.decision_id_high_water);
  writer.u64(value.fence_id_high_water);
  writer.u64(value.evidence_id_high_water);
  writer.u64(value.plan_id_high_water);
}

Status decode(ByteReader& reader, DurableState& value) {
  DurableState candidate;
  candidate.boot = BootIncarnation::from_value(reader.u64());
  candidate.term = CoordinatorTerm::from_value(reader.u64());
  candidate.last_commit_tick.value = reader.u64();
  Status status = decode(reader, candidate.policy);
  if (!status.ok()) return status;
  const std::uint32_t contracts = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < contracts; ++i) {
    ServiceContract contract;
    status = decode(reader, contract);
    if (!status.ok()) return status;
    candidate.contracts.push_back(contract);
  }
  const std::uint32_t grants = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < grants; ++i) {
    Grant grant;
    status = decode(reader, grant);
    if (!status.ok()) return status;
    candidate.grants.push_back(grant);
  }
  const std::uint32_t fences = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < fences; ++i) {
    FenceRecord fence;
    status = decode(reader, fence);
    if (!status.ok()) return status;
    candidate.fences.push_back(fence);
  }
  const std::uint32_t decisions = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < decisions; ++i) {
    Decision decision;
    status = decode(reader, decision);
    if (!status.ok()) return status;
    candidate.decisions.push_back(decision);
  }
  const std::uint32_t restorations = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < restorations; ++i) {
    RestorationEvaluation restoration;
    status = decode(reader, restoration);
    if (!status.ok()) return status;
    candidate.restorations.push_back(restoration);
  }
  status = decode(reader, candidate.historical_evidence);
  if (!status.ok()) return status;
  candidate.evidence_generation = EvidenceGeneration::from_value(reader.u64());
  candidate.capacity_generation = CapacityGeneration::from_value(reader.u64());
  candidate.fabric_generation = FabricGeneration::from_value(reader.u64());
  status = decode(reader, candidate.counters);
  if (!status.ok()) return status;
  candidate.journal_sequence = SequenceNumber::from_value(reader.u64());
  candidate.journal_records = reader.u64();
  candidate.boot_count = reader.u64();
  candidate.grant_id_high_water = reader.u64();
  candidate.decision_id_high_water = reader.u64();
  candidate.fence_id_high_water = reader.u64();
  candidate.evidence_id_high_water = reader.u64();
  candidate.plan_id_high_water = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  // An all-zero policy means "no policy has been installed", which is legal
  // durable state. Anything else must be a valid policy.
  if (candidate.policy.installed() && !candidate.policy.validate().ok()) {
    return Status(ErrorCode::Invalid, "snapshot carries an invalid policy");
  }
  value = candidate;
  return Status{};
}

namespace {

/// Decoded payload of one durable record.
struct RecordPayload {
  ServiceContract contract{};
  Grant grant{};
  FenceRecord fence{};
  Decision decision{};
  EvidenceItem evidence{};
  RestorationEvaluation restoration{};
  Policy policy{};
  AccountingCounters counters{};
  AuthorityBinding binding{};
  std::uint64_t id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t attempt = 0;
  std::uint64_t tick = 0;
  std::uint64_t value = 0;
};

Status decode_record_payload(RecordType type, ByteReader& reader, RecordPayload& payload) {
  Status status;
  switch (type) {
    case RecordType::ContractRegistered: return decode(reader, payload.contract);
    case RecordType::ContractRetired:
    case RecordType::TornTailObserved:
      payload.id = reader.u64();
      return reader.ok() ? Status{} : Status(reader.error());
    case RecordType::PolicyInstalled: return decode(reader, payload.policy);
    case RecordType::FabricGenerationAdvanced:
    case RecordType::CapacityGenerationAdvanced:
    case RecordType::EvidenceGenerationAdvanced:
    case RecordType::CoordinatorTermAdvanced:
      payload.sequence = reader.u64();
      return reader.ok() ? Status{} : Status(reader.error());
    case RecordType::EvidencePublished: return decode(reader, payload.evidence);
    case RecordType::GrantIssued: return decode(reader, payload.grant);
    case RecordType::GrantAcknowledged:
    case RecordType::GrantApplied:
      payload.id = reader.u64();
      payload.tick = reader.u64();
      payload.attempt = reader.u64();
      payload.sequence = reader.u64();
      return reader.ok() ? Status{} : Status(reader.error());
    case RecordType::GrantFenced:
    case RecordType::GrantExpired:
    case RecordType::GrantRevoked:
      return decode(reader, payload.fence);
    case RecordType::GrantRebound: {
      payload.id = reader.u64();
      Status binding_status = decode(reader, payload.binding);
      if (!binding_status.ok()) return binding_status;
      return reader.ok() ? Status{} : Status(reader.error());
    }
    case RecordType::DecisionRecorded: return decode(reader, payload.decision);
    case RecordType::BootAdvanced:
      payload.id = reader.u64();
      payload.sequence = reader.u64();
      payload.tick = reader.u64();
      return reader.ok() ? Status{} : Status(reader.error());
    case RecordType::RestorationProven:
    case RecordType::RestorationRefused:
      status = decode(reader, payload.restoration);
      if (!status.ok()) return status;
      if (type == RecordType::RestorationProven &&
          payload.restoration.outcome != RestorationOutcome::Proven) {
        return Status(ErrorCode::Invalid, "restoration-proven record carries a different outcome");
      }
      return Status{};
    case RecordType::AccountingCheckpoint: return decode(reader, payload.counters);
  }
  return Status(ErrorCode::InvalidEnum, "unknown record type");
}

/// Returns the retained grant, or null when no such grant is retained. A null
/// result cannot be silently dereferenced, which is why this returns a pointer
/// rather than a Status with an out-parameter.
Grant* find_grant(DurableState& state, GrantId id) noexcept {
  for (Grant& candidate : state.grants) {
    if (candidate.id == id) return &candidate;
  }
  return nullptr;
}

Status transition_grant(DurableState& state, GrantId id, GrantState next, Tick tick,
                        AttemptId attempt, SequenceNumber sequence) {
  Grant* grant = find_grant(state, id);
  if (grant == nullptr) {
    return Status(ErrorCode::NotFound, "record names a grant that is not retained");
  }
  if (!grant_transition_allowed(grant->state, next)) {
    return Status(ErrorCode::InvalidState, "illegal grant lifecycle transition");
  }
  grant->state = next;
  grant->last_transition_tick = tick;
  if (attempt.valid()) grant->last_attempt = attempt;
  if (sequence.valid()) grant->sequence = sequence;
  return Status{};
}

/// Applies one decoded record. The same function runs for live mutation and for
/// replay, so a restarted process reconstructs exactly the state its predecessor
/// committed.
Status apply_record(RecordType type, const RecordPayload& payload, DurableState& state,
                    const StoreConfig& config, Accounting& accounting) {
  switch (type) {
    case RecordType::ContractRegistered: {
      for (ServiceContract& existing : state.contracts) {
        if (existing.id == payload.contract.id) {
          existing = payload.contract;
          return Status{};
        }
      }
      if (state.contracts.size() >= config.max_retained_contracts) {
        return Status(ErrorCode::CapacityExceeded, "contract table is full");
      }
      state.contracts.push_back(payload.contract);
      return Status{};
    }
    case RecordType::ContractRetired: {
      for (ServiceContract& existing : state.contracts) {
        if (existing.id == ContractId::from_value(payload.id)) {
          existing.active = false;
          return Status{};
        }
      }
      return Status(ErrorCode::NotFound, "retired contract is not retained");
    }
    case RecordType::PolicyInstalled:
      state.policy = payload.policy;
      return Status{};
    case RecordType::FabricGenerationAdvanced:
      state.fabric_generation = FabricGeneration::from_value(payload.sequence);
      return Status{};
    case RecordType::CapacityGenerationAdvanced:
      state.capacity_generation = CapacityGeneration::from_value(payload.sequence);
      return Status{};
    case RecordType::EvidenceGenerationAdvanced:
      state.evidence_generation = EvidenceGeneration::from_value(payload.sequence);
      return Status{};
    case RecordType::EvidencePublished: {
      Status status = state.historical_evidence.push(payload.evidence);
      if (!status.ok()) return status;
      status = state.historical_evidence.normalize();
      if (!status.ok()) return status;
      if (payload.evidence.generation > state.evidence_generation) {
        state.evidence_generation = payload.evidence.generation;
      }
      if (payload.evidence.id.value() > state.evidence_id_high_water) {
        state.evidence_id_high_water = payload.evidence.id.value();
      }
      return Status{};
    }
    case RecordType::GrantIssued: {
      for (Grant& existing : state.grants) {
        if (existing.id == payload.grant.id) {
          return Status(ErrorCode::AlreadyExists, "grant identity was reused");
        }
      }
      if (state.live_grant_count() >= config.max_retained_grants) {
        return Status(ErrorCode::CapacityExceeded, "live grant table is full");
      }
      state.grants.push_back(payload.grant);
      if (payload.grant.id.value() > state.grant_id_high_water) {
        state.grant_id_high_water = payload.grant.id.value();
      }
      return accounting.record_grant_issued();
    }
    case RecordType::GrantAcknowledged: {
      Status status = transition_grant(state, GrantId::from_value(payload.id),
                                       GrantState::Acknowledged, Tick{payload.tick},
                                       AttemptId::from_value(payload.attempt),
                                       SequenceNumber::from_value(payload.sequence));
      if (!status.ok()) return status;
      return accounting.record_grant_acknowledged();
    }
    case RecordType::GrantApplied: {
      Status status =
          transition_grant(state, GrantId::from_value(payload.id), GrantState::Applied,
                           Tick{payload.tick}, AttemptId::from_value(payload.attempt),
                           SequenceNumber::from_value(payload.sequence));
      if (!status.ok()) return status;
      return accounting.record_grant_applied();
    }
    case RecordType::GrantFenced:
    case RecordType::GrantExpired:
    case RecordType::GrantRevoked: {
      const GrantState next = type == RecordType::GrantFenced    ? GrantState::Fenced
                              : type == RecordType::GrantExpired ? GrantState::Expired
                                                                 : GrantState::Revoked;
      Status status = transition_grant(state, payload.fence.grant, next, payload.fence.fenced_tick,
                                       AttemptId{}, SequenceNumber{});
      if (!status.ok()) return status;
      state.fences.push_back(payload.fence);
      if (payload.fence.id.value() > state.fence_id_high_water) {
        state.fence_id_high_water = payload.fence.id.value();
      }
      Status counted = accounting.record_fences(1);
      if (!counted.ok()) return counted;
      switch (next) {
        case GrantState::Fenced: return accounting.record_grant_fenced();
        case GrantState::Expired: return accounting.record_grant_expired();
        case GrantState::Revoked: return accounting.record_grant_revoked();
        case GrantState::Issued:
        case GrantState::Acknowledged:
        case GrantState::Applied:
          return Status{};
      }
      return Status{};
    }
    case RecordType::GrantRebound: {
      Grant* grant = find_grant(state, GrantId::from_value(payload.id));
      if (grant == nullptr) {
        return Status(ErrorCode::NotFound, "record names a grant that is not retained");
      }
      if (!grant->live()) {
        return Status(ErrorCode::InvalidState, "a terminal grant cannot be rebound");
      }
      if (grant->binding.contract != payload.binding.contract ||
          grant->binding.contract_generation != payload.binding.contract_generation ||
          grant->binding.subject != payload.binding.subject ||
          grant->binding.subject_generation != payload.binding.subject_generation ||
          grant->binding.scope != payload.binding.scope) {
        return Status(ErrorCode::Invalid,
                      "a rebound grant must keep the same contract, subject and scope");
      }
      grant->binding = payload.binding;
      return Status{};
    }
    case RecordType::DecisionRecorded: {
      state.decisions.push_back(payload.decision);
      if (payload.decision.id.value() > state.decision_id_high_water) {
        state.decision_id_high_water = payload.decision.id.value();
      }
      return accounting.record_decision(payload.decision.outcome);
    }
    case RecordType::BootAdvanced: {
      const BootIncarnation boot = BootIncarnation::from_value(payload.id);
      const CoordinatorTerm term = CoordinatorTerm::from_value(payload.sequence);
      if (!boot.valid() || !term.valid()) {
        return Status(ErrorCode::Invalid, "boot advance record carries an unset authority value");
      }
      if (state.boot.valid() && boot <= state.boot) {
        return Status(ErrorCode::SequenceRegression, "boot incarnation did not advance");
      }
      if (state.term.valid() && term < state.term) {
        return Status(ErrorCode::SequenceRegression, "coordinator term regressed");
      }
      state.boot = boot;
      state.term = term;
      state.last_commit_tick = Tick{payload.tick};
      const auto next_boot_count = checked_add(state.boot_count, std::uint64_t{1});
      if (!next_boot_count.has_value()) {
        return Status(ErrorCode::Overflow, "boot count overflow");
      }
      state.boot_count = *next_boot_count;
      Status counted = accounting.record_boot();
      if (!counted.ok()) return counted;
      return accounting.record_term_advance();
    }
    case RecordType::CoordinatorTermAdvanced: {
      const CoordinatorTerm term = CoordinatorTerm::from_value(payload.sequence);
      if (!term.valid()) {
        return Status(ErrorCode::Invalid, "term advance record carries an unset term");
      }
      if (state.term.valid() && term <= state.term) {
        return Status(ErrorCode::SequenceRegression, "coordinator term did not advance");
      }
      state.term = term;
      return accounting.record_term_advance();
    }
    case RecordType::TornTailObserved:
      return accounting.record_torn_tail();
    case RecordType::RestorationProven:
      state.restorations.push_back(payload.restoration);
      return Status{};
    case RecordType::RestorationRefused:
      state.restorations.push_back(payload.restoration);
      return Status{};
    case RecordType::AccountingCheckpoint:
      return accounting.replace(payload.counters);
  }
  return Status(ErrorCode::InvalidEnum, "unknown record type");
}

/// Bounded retention. Live grants are never pruned; terminated history, fences,
/// decisions and historical evidence are.
Status prune(DurableState& state, const StoreConfig& config, Accounting& accounting) {
  const std::size_t live = static_cast<std::size_t>(state.live_grant_count());
  const std::size_t grant_budget =
      config.max_retained_grants > live ? config.max_retained_grants - live : 0;
  std::vector<Grant> retained_live;
  std::vector<Grant> retained_terminated;
  retained_live.reserve(live);
  for (const Grant& grant : state.grants) {
    if (grant.live()) {
      retained_live.push_back(grant);
    } else {
      retained_terminated.push_back(grant);
    }
  }
  std::sort(retained_terminated.begin(), retained_terminated.end(),
            [](const Grant& a, const Grant& b) {
              if (a.last_transition_tick != b.last_transition_tick) {
                return a.last_transition_tick > b.last_transition_tick;
              }
              return a.id > b.id;
            });
  if (retained_terminated.size() > grant_budget) {
    const std::size_t dropped = retained_terminated.size() - grant_budget;
    retained_terminated.resize(grant_budget);
    Status status = accounting.record_grant_pruned(dropped);
    if (!status.ok()) return status;
  }
  state.grants.clear();
  state.grants.insert(state.grants.end(), retained_live.begin(), retained_live.end());
  state.grants.insert(state.grants.end(), retained_terminated.begin(), retained_terminated.end());
  std::sort(state.grants.begin(), state.grants.end(),
            [](const Grant& a, const Grant& b) { return a.id < b.id; });

  if (state.fences.size() > config.max_retained_fences) {
    const std::size_t dropped = state.fences.size() - config.max_retained_fences;
    std::sort(state.fences.begin(), state.fences.end(),
              [](const FenceRecord& a, const FenceRecord& b) {
                if (a.fenced_tick != b.fenced_tick) return a.fenced_tick > b.fenced_tick;
                return a.id > b.id;
              });
    state.fences.resize(config.max_retained_fences);
    std::sort(state.fences.begin(), state.fences.end(),
              [](const FenceRecord& a, const FenceRecord& b) { return a.id < b.id; });
    Status status = accounting.record_fences_pruned(dropped);
    if (!status.ok()) return status;
  }

  if (state.decisions.size() > config.max_retained_decisions) {
    const std::size_t dropped = state.decisions.size() - config.max_retained_decisions;
    std::sort(state.decisions.begin(), state.decisions.end(),
              [](const Decision& a, const Decision& b) {
                if (a.decided_tick != b.decided_tick) return a.decided_tick > b.decided_tick;
                return a.id > b.id;
              });
    state.decisions.resize(config.max_retained_decisions);
    std::sort(state.decisions.begin(), state.decisions.end(),
              [](const Decision& a, const Decision& b) { return a.id < b.id; });
    (void)dropped;
  }

  if (state.restorations.size() > config.max_retained_decisions) {
    std::sort(state.restorations.begin(), state.restorations.end(),
              [](const RestorationEvaluation& a, const RestorationEvaluation& b) {
                if (a.evaluated_tick != b.evaluated_tick) return a.evaluated_tick > b.evaluated_tick;
                return a.grant > b.grant;
              });
    state.restorations.resize(config.max_retained_decisions);
  }

  if (state.historical_evidence.size() >= config.max_retained_evidence) {
    std::vector<EvidenceItem> items = state.historical_evidence.items();
    std::sort(items.begin(), items.end(), [](const EvidenceItem& a, const EvidenceItem& b) {
      if (a.observed_tick != b.observed_tick) return a.observed_tick > b.observed_tick;
      return a.id > b.id;
    });
    const std::size_t keep = config.max_retained_evidence > 4 ? config.max_retained_evidence * 3 / 4 : 0;
    if (items.size() > keep) items.resize(keep);
    EvidenceVector rebuilt(items.size() + 1);
    for (const EvidenceItem& item : items) {
      Status status = rebuilt.push(item);
      if (!status.ok()) return status;
    }
    Status status = rebuilt.normalize();
    if (!status.ok()) return status;
    state.historical_evidence = rebuilt;
  }
  return Status{};
}

std::string join_path(const std::string& root, const std::string& name) {
  return (std::filesystem::path(root) / name).string();
}

/// Terminates the process at a configured crash boundary. std::_Exit performs no
/// stack unwinding, runs no destructor and flushes no stream, which is exactly
/// the semantics of a hard kill at that byte of progress.
[[noreturn]] void crash_at_boundary() { std::_Exit(70); }

std::string join_path(const std::string& root, const std::string& name);

}  // namespace

struct StateStore::Impl {
  Impl() : accounting(state.counters) {}

  StoreConfig config{};
  DurableState state{};
  Accounting accounting;
  RecoveryOutcome recovery = RecoveryOutcome::FreshStore;
  std::string recovery_detail{};
  detail::FileHandle journal{};
  detail::LockFile lock{};
  std::string journal_name{};
  std::uint64_t base_sequence = 0;
  std::uint64_t records_in_segment = 0;
  std::uint64_t bytes_in_segment = 0;
  bool closed = false;
};

StateStore::StateStore() : impl_(std::make_unique<Impl>()) {}

StateStore::~StateStore() {
  if (impl_) {
    Status status = close();
    (void)status;
  }
}

Result<std::unique_ptr<StateStore>> StateStore::open(const StoreConfig& config) {
  if (config.root.empty()) {
    return Status(ErrorCode::InvalidArgument, "store root is empty");
  }
  if (!is_text_safe(config.root) || config.root.size() > 4096) {
    return Status(ErrorCode::InvalidArgument, "store root is not a safe text path");
  }
  if (config.max_retained_evidence == 0 || config.max_retained_grants == 0 ||
      config.max_retained_fences == 0) {
    return Status(ErrorCode::InvalidArgument, "store retention bounds must be non-zero");
  }
  auto store = std::unique_ptr<StateStore>(new StateStore());
  Impl& impl = *store->impl_;
  impl.config = config;

  Status status = Status{};
  if (config.read_only) {
    bool root_exists = false;
    status = detail::file_exists(config.root, root_exists);
    if (!status.ok()) return status;
    if (!root_exists) {
      return Status(ErrorCode::NotFound, "read-only open of a store root that does not exist");
    }
  } else {
    status = detail::ensure_directory(config.root);
    if (!status.ok()) return status;
    // One writer per store root. Without this, two runtimes appending to the
    // same journal would interleave sequences and corrupt the store.
    auto lock = detail::LockFile::acquire(join_path(config.root, "store.lock"));
    if (!lock.ok()) return lock.status();
    impl.lock = std::move(lock.value());
  }

  std::vector<std::string> names;
  status = detail::list_directory(config.root, names);
  if (!status.ok()) return status;

  // Any staging file is debris from a crash between create and rename; the
  // rename is the commit point, so the staging file is never authoritative.
  if (!config.read_only) {
    for (const std::string& name : names) {
      if (name.size() > 8 && name.compare(name.size() - 8, 8, ".staging") == 0) {
        status = detail::remove_file(join_path(config.root, name));
        if (!status.ok()) return status;
      }
    }
  }

  std::vector<std::uint64_t> snapshots;
  std::vector<std::uint64_t> journals;
  for (const std::string& name : names) {
    std::uint64_t sequence = 0;
    if (detail::parse_snapshot_file_name(name, sequence).ok()) {
      snapshots.push_back(sequence);
    } else if (detail::parse_journal_file_name(name, sequence).ok()) {
      journals.push_back(sequence);
    }
  }
  std::sort(snapshots.begin(), snapshots.end());
  std::sort(journals.begin(), journals.end());

  std::uint64_t snapshot_sequence = 0;
  bool have_snapshot = false;
  if (!snapshots.empty()) {
    snapshot_sequence = snapshots.back();
    std::vector<std::uint8_t> bytes;
    status = detail::read_file(join_path(config.root, detail::snapshot_file_name(snapshot_sequence)),
                               bytes, config.max_snapshot_bytes + 4096);
    if (!status.ok()) return status;
    detail::SnapshotHeader header;
    std::vector<std::uint8_t> payload;
    status = detail::decode_snapshot(bytes, header, payload, static_cast<std::size_t>(config.max_snapshot_bytes));
    if (!status.ok()) return status;
    if (header.sequence != snapshot_sequence) {
      return Status(ErrorCode::Corrupt, "snapshot file name disagrees with its payload");
    }
    ByteReader reader(payload);
    status = decode(reader, impl.state);
    if (!status.ok()) return status;
    status = decode_finish(reader);
    if (!status.ok()) return status;
    have_snapshot = true;
    (void)impl.accounting.record_snapshot_loaded();
    impl.base_sequence = snapshot_sequence;
  }

  // A journal whose base is below the snapshot is already folded in and is
  // removed once the new segment is durable.
  std::uint64_t active_base = impl.base_sequence;
  const std::string active_name = detail::journal_file_name(active_base);
  bool active_exists = false;
  status = detail::file_exists(join_path(config.root, active_name), active_exists);
  if (!status.ok()) return status;

  bool saw_footer = false;
  if (active_exists) {
    auto handle = detail::FileHandle::open_read(join_path(config.root, active_name));
    if (!handle.ok()) return handle.status();
    detail::FileHandle reader = std::move(handle.value());
    const std::uint64_t file_size = reader.size();
    std::vector<std::uint8_t> header_bytes(detail::kJournalHeaderSize);
    std::size_t read = 0;
    status = reader.read_some(header_bytes.data(), detail::kJournalHeaderSize, read);
    if (!status.ok()) return status;
    if (read != detail::kJournalHeaderSize) {
      return Status(ErrorCode::Truncated, "journal file is shorter than its header");
    }
    detail::JournalHeader journal_header;
    status = detail::decode_journal_header(header_bytes.data(), header_bytes.size(), journal_header);
    if (!status.ok()) return status;
    if (journal_header.base_sequence != active_base) {
      return Status(ErrorCode::Corrupt, "journal header disagrees with its file name");
    }
    std::uint64_t consumed = detail::kJournalHeaderSize;
    std::uint64_t expected = active_base + 1;
    std::uint64_t record_count = 0;
    bool torn = false;
    std::uint64_t torn_bytes = 0;
    while (consumed < file_size) {
      const std::uint64_t remaining = file_size - consumed;
      const std::uint64_t peek_size =
          remaining < detail::kRecordHeaderSize ? remaining : detail::kRecordHeaderSize;
      std::vector<std::uint8_t> record_header(static_cast<std::size_t>(peek_size));
      std::size_t got = 0;
      status = reader.read_some(record_header.data(), record_header.size(), got);
      if (!status.ok()) return status;
      if (got != record_header.size()) {
        return Status(ErrorCode::IoError, "journal shrank while reading");
      }
      // A cleanly closed segment ends with a fixed-size footer, which is also
      // the size of a record header. Distinguish them by magic before treating
      // the bytes as a record.
      if (record_header.size() == detail::kJournalFooterSize &&
          record_header[0] == detail::kJournalFooterMagic[0] &&
          record_header[1] == detail::kJournalFooterMagic[1] &&
          record_header[2] == detail::kJournalFooterMagic[2] &&
          record_header[3] == detail::kJournalFooterMagic[3]) {
        detail::JournalFooter footer;
        status = detail::decode_journal_footer(record_header.data(), record_header.size(), footer);
        if (!status.ok()) {
          return Status(status.code(), std::string("journal footer rejected: ") +
                                          std::string(status.detail()));
        }
        if (footer.record_count != record_count || footer.last_sequence + 1 != expected) {
          return Status(ErrorCode::Corrupt, "journal footer disagrees with the replayed records");
        }
        saw_footer = true;
        consumed += got;
        break;
      }
      if (remaining < detail::kRecordHeaderSize) {
        if (have_snapshot) {
          return Status(ErrorCode::Corrupt, "journal ends with a truncated record header");
        }
        torn = true;
        torn_bytes = remaining;
        break;
      }
      consumed += got;
      detail::RecordHeader record;
      status = detail::decode_record_header(record_header.data(), record_header.size(), record);
      if (!status.ok()) {
        return Status(status.code(), std::string("journal record header rejected: ") +
                                        std::string(status.detail()));
      }
      if (record.sequence != expected) {
        return Status(ErrorCode::SequenceRegression,
                      "journal record sequence is not the expected successor");
      }
      const std::uint64_t needed = record.payload_length + detail::kRecordTrailerSize;
      if (consumed + needed > file_size) {
        torn = true;
        torn_bytes = file_size - consumed;
        break;
      }
      std::vector<std::uint8_t> payload(record.payload_length);
      got = 0;
      status = reader.read_some(payload.data(), payload.size(), got);
      if (!status.ok()) return status;
      if (got != payload.size()) return Status(ErrorCode::IoError, "journal payload read was short");
      std::vector<std::uint8_t> trailer(detail::kRecordTrailerSize);
      got = 0;
      status = reader.read_some(trailer.data(), trailer.size(), got);
      if (!status.ok()) return status;
      if (got != trailer.size()) return Status(ErrorCode::IoError, "journal trailer read was short");
      consumed += needed;
      const std::uint32_t stored_crc = static_cast<std::uint32_t>(trailer[0]) |
                                       (static_cast<std::uint32_t>(trailer[1]) << 8) |
                                       (static_cast<std::uint32_t>(trailer[2]) << 16) |
                                       (static_cast<std::uint32_t>(trailer[3]) << 24);
      if (stored_crc != crc32c(payload.data(), payload.size())) {
        return Status(ErrorCode::IntegrityFailure,
                      "journal record payload fails its integrity check");
      }
      ByteReader payload_reader(payload);
      RecordPayload decoded;
      status = decode_record_payload(record.type, payload_reader, decoded);
      if (!status.ok()) return status;
      status = decode_finish(payload_reader);
      if (!status.ok()) return status;
      status = apply_record(record.type, decoded, impl.state, config, impl.accounting);
      if (!status.ok()) return status;
      ++record_count;
      ++expected;
    }

    if (!torn && consumed < file_size) {
      return Status(ErrorCode::TrailingGarbage, "journal carries bytes after its last record");
    }

    if (torn && !config.read_only) {
      // A genuine torn tail is the only safe thing to discard: either the
      // remaining bytes cannot hold a record header at all, or a header that
      // passed its own integrity check declares a payload the file does not
      // contain. Anything else was already rejected above as corruption.
      //
      // The bytes are removed rather than left in place, because a valid-looking
      // header followed by a later record's bytes would turn a recoverable tail
      // into an unrecoverable one on the next open. The truncation is reported
      // through the recovery outcome and recorded durably below; it is never
      // silent.
      Status close_status = reader.close();
      if (!close_status.ok()) return close_status;
      auto writable = detail::FileHandle::open_append(join_path(config.root, active_name));
      if (!writable.ok()) return writable.status();
      Status truncate_status = writable.value().truncate(consumed);
      if (!truncate_status.ok()) return truncate_status;
      truncate_status = writable.value().flush();
      if (!truncate_status.ok()) return truncate_status;
      truncate_status = writable.value().close();
      if (!truncate_status.ok()) return truncate_status;
    }

    if (torn) {
      // A partial record after a valid file header is a torn first write: no
      // record was committed, so recovering to an empty segment loses nothing.
      // A file whose *header* is invalid is corruption and was refused above.
      //
      // A read-only open reports the same outcome but changes nothing: the
      // verification path must be able to describe a damaged store without
      // repairing it.
      impl.recovery = RecoveryOutcome::TornTailRecovered;
      impl.recovery_detail = config.read_only
                                 ? "a torn journal tail is present; a read-only open "
                                   "leaves it untouched"
                                 : "discarded a torn journal tail and truncated the segment; "
                                   "every committed record was retained";
      status = impl.accounting.record_torn_tail();
      if (!status.ok()) return status;
    } else if (have_snapshot) {
      impl.recovery = RecoveryOutcome::CleanReopen;
      impl.recovery_detail = saw_footer ? "segment closed cleanly" : "segment replayed to its end";
    } else {
      impl.recovery = RecoveryOutcome::CleanReopen;
      impl.recovery_detail = "journal replayed without a snapshot";
    }

    // Fold the replayed counters into the accounting block.
    const auto next_records = checked_add(impl.state.journal_records, record_count);
    if (!next_records.has_value()) return Status(ErrorCode::Overflow, "journal record count overflow");
    impl.state.journal_records = *next_records;
    impl.state.journal_sequence = SequenceNumber::from_value(expected - 1);
    impl.records_in_segment = record_count;
    impl.bytes_in_segment = file_size;
    (void)impl.accounting.record_journal_read(record_count);

    impl.journal_name = active_name;
    impl.base_sequence = active_base;
    if (!config.read_only) {
      if (saw_footer) {
        // A footer means the segment was closed cleanly. It is never appended to
        // again: either a successor segment is started, or, when the segment is
        // empty, its footer is removed in place.
        const std::uint64_t base = impl.state.journal_sequence.value();
        const std::string successor = detail::journal_file_name(base);
        if (successor == active_name) {
          auto reopen = detail::FileHandle::open_append(join_path(config.root, active_name));
          if (!reopen.ok()) return reopen.status();
          Status truncated = reopen.value().truncate(detail::kJournalHeaderSize);
          if (!truncated.ok()) return truncated;
          truncated = reopen.value().flush();
          if (!truncated.ok()) return truncated;
          impl.journal = std::move(reopen.value());
          impl.bytes_in_segment = detail::kJournalHeaderSize;
        } else {
          detail::JournalHeader successor_header;
          successor_header.boot = impl.state.boot.valid() ? impl.state.boot
                                                         : BootIncarnation::from_value(1);
          successor_header.base_sequence = base;
          const std::vector<std::uint8_t> bytes = detail::encode_journal_header(successor_header);
          Status created = detail::write_file_atomic(join_path(config.root, successor), bytes.data(),
                                                     bytes.size());
          if (!created.ok()) return created;
          auto successor_handle = detail::FileHandle::open_append(join_path(config.root, successor));
          if (!successor_handle.ok()) return successor_handle.status();
          impl.journal = std::move(successor_handle.value());
          impl.journal_name = successor;
          impl.base_sequence = base;
          impl.bytes_in_segment = bytes.size();
        }
        impl.records_in_segment = 0;
      } else {
        auto append_handle = detail::FileHandle::open_append(join_path(config.root, active_name));
        if (!append_handle.ok()) return append_handle.status();
        impl.journal = std::move(append_handle.value());
      }
    }

    if (torn && !config.read_only) {
      // Record the observation durably so a later reader sees that a tail was
      // discarded rather than silently assuming a clean shutdown.
      const std::vector<std::uint8_t> payload = [&]() {
        ByteWriter writer(16);
        writer.u64(torn_bytes);
        return writer.bytes();
      }();
      status = store->append(RecordType::TornTailObserved, payload);
      if (!status.ok()) return status;
    }
  } else if (config.read_only) {
    impl.recovery = have_snapshot ? RecoveryOutcome::CleanReopen : RecoveryOutcome::FreshStore;
    impl.recovery_detail = "read-only open: no journal segment is present";
  } else {
    detail::JournalHeader journal_header;
    journal_header.boot = BootIncarnation::from_value(1);
    journal_header.base_sequence = active_base;
    const std::vector<std::uint8_t> bytes = detail::encode_journal_header(journal_header);
    status = detail::write_file_atomic(join_path(config.root, active_name), bytes.data(), bytes.size());
    if (!status.ok()) return status;
    auto append_handle = detail::FileHandle::open_append(join_path(config.root, active_name));
    if (!append_handle.ok()) return append_handle.status();
    impl.journal = std::move(append_handle.value());
    impl.journal_name = active_name;
    impl.base_sequence = active_base;
    impl.bytes_in_segment = bytes.size();
    impl.recovery = have_snapshot ? RecoveryOutcome::CleanReopen : RecoveryOutcome::FreshStore;
    impl.recovery_detail = have_snapshot ? "snapshot loaded without a journal segment"
                                         : "new store created";
  }

  if (config.read_only) return store;

  // Remove superseded segments and snapshots now that the active pair is durable.
  for (const std::uint64_t sequence : journals) {
    if (sequence >= impl.base_sequence) continue;
    status = detail::remove_file(join_path(config.root, detail::journal_file_name(sequence)));
    if (!status.ok()) return status;
  }
  for (const std::uint64_t sequence : snapshots) {
    if (sequence >= snapshot_sequence && have_snapshot) continue;
    status = detail::remove_file(join_path(config.root, detail::snapshot_file_name(sequence)));
    if (!status.ok()) return status;
  }
  (void)detail::sync_directory(config.root);
  return store;
}

Status StateStore::append(RecordType type, const std::vector<std::uint8_t>& payload) {
  Impl& impl = *impl_;
  const std::uint64_t ordinal = impl.state.journal_records + 1;
  if (impl.config.crash_before_record != 0 && impl.config.crash_before_record == ordinal) {
    crash_at_boundary();
  }
  Status status = append_batched(type, payload);
  if (!status.ok()) return status;
  if (impl.config.crash_after_write_record != 0 &&
      impl.config.crash_after_write_record == ordinal) {
    crash_at_boundary();
  }
  status = flush();
  if (!status.ok()) return status;
  if (impl.config.crash_after_record != 0 && impl.config.crash_after_record == ordinal) {
    crash_at_boundary();
  }
  return Status{};
}

Status StateStore::append_batched(RecordType type, const std::vector<std::uint8_t>& payload) {
  Impl& impl = *impl_;
  if (impl.closed) return Status(ErrorCode::InvalidState, "store is closed");
  if (impl.config.read_only) return Status(ErrorCode::InvalidState, "store is opened read-only");
  if (!is_valid(type)) return Status(ErrorCode::InvalidEnum, "unknown record type");
  if (payload.size() > detail::kMaxRecordPayload) {
    return Status(ErrorCode::CapacityExceeded, "record payload exceeds the journal bound");
  }
  ByteReader reader(payload);
  RecordPayload decoded;
  Status status = decode_record_payload(type, reader, decoded);
  if (!status.ok()) return status;
  status = decode_finish(reader);
  if (!status.ok()) return status;

  const auto next = checked_add(impl.state.journal_sequence.value(), std::uint64_t{1});
  if (!next.has_value()) return Status(ErrorCode::Overflow, "journal sequence exhausted");
  detail::RecordHeader header;
  header.type = type;
  header.sequence = *next;
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  const std::vector<std::uint8_t> bytes = detail::encode_record(header, payload);
  status = impl.journal.write_all(bytes.data(), bytes.size());
  if (!status.ok()) return status;

  impl.state.journal_sequence = SequenceNumber::from_value(*next);
  const auto next_count = checked_add(impl.state.journal_records, std::uint64_t{1});  // durable ordinal
  if (!next_count.has_value()) return Status(ErrorCode::Overflow, "journal record count overflow");
  impl.state.journal_records = *next_count;
  ++impl.records_in_segment;
  const auto next_bytes = checked_add(impl.bytes_in_segment, static_cast<std::uint64_t>(bytes.size()));
  if (!next_bytes.has_value()) return Status(ErrorCode::Overflow, "journal byte count overflow");
  impl.bytes_in_segment = *next_bytes;
  status = impl.accounting.record_journal_write(1, bytes.size());
  if (!status.ok()) return status;
  status = apply_record(type, decoded, impl.state, impl.config, impl.accounting);
  if (!status.ok()) return status;
  if (impl.state.grants.size() > impl.config.max_retained_grants * 2 ||
      impl.state.fences.size() > impl.config.max_retained_fences * 2 ||
      impl.state.decisions.size() > impl.config.max_retained_decisions * 2 ||
      impl.state.historical_evidence.size() >= impl.config.max_retained_evidence) {
    return prune(impl.state, impl.config, impl.accounting);
  }
  return Status{};
}

Status StateStore::flush() {
  Impl& impl = *impl_;
  if (impl.closed) return Status(ErrorCode::InvalidState, "store is closed");
  if (impl.config.read_only) return Status(ErrorCode::InvalidState, "store is opened read-only");
  return impl.journal.flush();
}

bool StateStore::rotation_due() const noexcept {
  const Impl& impl = *impl_;
  if (impl.config.read_only) return false;
  return impl.records_in_segment >= impl.config.max_records_per_segment ||
         impl.bytes_in_segment >= impl.config.max_journal_bytes;
}

Status StateStore::take_snapshot() {
  Impl& impl = *impl_;
  if (impl.closed) return Status(ErrorCode::InvalidState, "store is closed");
  if (impl.config.read_only) return Status(ErrorCode::InvalidState, "store is opened read-only");
  Status status = flush();
  if (!status.ok()) return status;
  status = prune(impl.state, impl.config, impl.accounting);
  if (!status.ok()) return status;

  {
    // Checkpoint the counters that no record derives (protocol, session,
    // journal and evidence activity) so a clean shutdown preserves them. Every
    // lifecycle counter is already implied by the records themselves, so a hard
    // kill cannot break a closure identity.
    ByteWriter checkpoint;
    encode(checkpoint, impl.accounting.counters());
    const Status checkpoint_status = append(RecordType::AccountingCheckpoint, checkpoint.bytes());
    if (!checkpoint_status.ok()) return checkpoint_status;
  }
  const std::uint64_t sequence = impl.state.journal_sequence.value();
  ByteWriter writer(static_cast<std::size_t>(std::min<std::uint64_t>(
      impl.config.max_snapshot_bytes, static_cast<std::uint64_t>(kMaxDocumentBytes))));
  encode(writer, impl.state);
  if (!writer.ok()) return Status(ErrorCode::CapacityExceeded, "durable state exceeds the snapshot bound");

  detail::SnapshotHeader header;
  header.sequence = sequence;
  header.payload_length = writer.size();
  const std::vector<std::uint8_t> bytes = detail::encode_snapshot(header, writer.bytes());
  status = detail::write_file_atomic(join_path(impl.config.root, detail::snapshot_file_name(sequence)),
                                     bytes.data(), bytes.size());
  if (!status.ok()) return status;
  (void)impl.accounting.record_snapshot_taken();

  // Rotate: create the successor segment before removing the predecessor.
  detail::JournalHeader journal_header;
  journal_header.boot = impl.state.boot.valid() ? impl.state.boot : BootIncarnation::from_value(1);
  journal_header.base_sequence = sequence;
  const std::vector<std::uint8_t> journal_bytes = detail::encode_journal_header(journal_header);
  const std::string next_name = detail::journal_file_name(sequence);
  status = detail::write_file_atomic(join_path(impl.config.root, next_name), journal_bytes.data(),
                                     journal_bytes.size());
  if (!status.ok()) return status;
  auto handle = detail::FileHandle::open_append(join_path(impl.config.root, next_name));
  if (!handle.ok()) return handle.status();

  const std::string previous_name = impl.journal_name;
  impl.journal = std::move(handle.value());
  impl.journal_name = next_name;
  impl.base_sequence = sequence;
  impl.records_in_segment = 0;
  impl.bytes_in_segment = journal_bytes.size();

  std::vector<std::string> names;
  status = detail::list_directory(impl.config.root, names);
  if (!status.ok()) return status;
  std::uint64_t newest_snapshot = sequence;
  std::vector<std::string> stale_snapshots;
  std::vector<std::string> stale_journals;
  for (const std::string& name : names) {
    std::uint64_t parsed = 0;
    if (detail::parse_snapshot_file_name(name, parsed).ok()) {
      if (parsed < newest_snapshot) stale_snapshots.push_back(name);
    } else if (detail::parse_journal_file_name(name, parsed).ok()) {
      if (name != next_name && name != previous_name) stale_journals.push_back(name);
    }
  }
  for (const std::string& name : stale_snapshots) {
    (void)detail::remove_file(join_path(impl.config.root, name));
  }
  if (!previous_name.empty() && previous_name != next_name) {
    (void)detail::remove_file(join_path(impl.config.root, previous_name));
  }
  for (const std::string& name : stale_journals) {
    (void)detail::remove_file(join_path(impl.config.root, name));
  }
  (void)detail::sync_directory(impl.config.root);
  return Status{};
}

Status StateStore::close() {
  Impl& impl = *impl_;
  if (impl.closed) return Status{};
  impl.closed = true;
  // A closed store must not keep the root locked: another runtime is then free
  // to open it, and an offline verifier can inspect it immediately.
  const Status unlock_status = impl.lock.release();
  (void)unlock_status;
  // A read-only open never writes a footer, so it cannot change the store it
  // just described.
  if (impl.config.read_only) return Status{};
  if (!impl.journal.valid()) return Status{};
  detail::JournalFooter footer;
  footer.record_count = impl.records_in_segment;
  footer.last_sequence = impl.state.journal_sequence.value();
  const std::vector<std::uint8_t> bytes = detail::encode_journal_footer(footer);
  Status status = impl.journal.write_all(bytes.data(), bytes.size());
  if (!status.ok()) {
    impl.journal.close();
    return status;
  }
  status = impl.journal.flush();
  Status close_status = impl.journal.close();
  if (!status.ok()) return status;
  return close_status;
}

const DurableState& StateStore::state() const noexcept { return impl_->state; }
DurableState& StateStore::mutable_state() noexcept { return impl_->state; }
Accounting& StateStore::accounting() noexcept { return impl_->accounting; }
const Accounting& StateStore::accounting() const noexcept { return impl_->accounting; }
RecoveryOutcome StateStore::recovery() const noexcept { return impl_->recovery; }
const std::string& StateStore::recovery_detail() const noexcept { return impl_->recovery_detail; }
const StoreConfig& StateStore::config() const noexcept { return impl_->config; }
bool StateStore::closed() const noexcept { return impl_->closed; }
std::uint64_t StateStore::bytes_in_segment() const noexcept { return impl_->bytes_in_segment; }
std::uint64_t StateStore::records_in_segment() const noexcept { return impl_->records_in_segment; }

}  // namespace dmf