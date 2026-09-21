// Degraded Mode Fabric - coordinator implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Locking contract (see docs/CONCURRENCY-AUDIT.md):
//   * exactly one non-recursive mutex guards every mutable member of Impl;
//   * every public entry point takes it once and then calls a *_locked helper,
//     so no public entry point can re-enter the same lock;
//   * no socket write, thread join, callback or handler invocation happens while
//     it is held;
//   * pointers into the durable vectors are never held across an append, because
//     apply_record can reallocate them.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dmf/runtime.hpp"

namespace dmf {

namespace {

bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = static_cast<unsigned char>(
        difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
  }
  return difference == 0;
}

PrincipalRole required_role(MessageType type) noexcept {
  switch (type) {
    case MessageType::RegisterContract:
    case MessageType::InstallPolicy:
      return PrincipalRole::Administrator;
    case MessageType::PublishEvidence:
    case MessageType::AcquireAuthority:
    case MessageType::AcknowledgeGrant:
    case MessageType::ReportApplied:
    case MessageType::RequestRestoration:
    case MessageType::FenceGrant:
      return PrincipalRole::Operator;
    default:
      return PrincipalRole::Observer;
  }
}

std::string reason_text(ReasonCode reason, DecisionOutcome outcome) {
  std::string out(to_string(outcome));
  out.append(": ");
  out.append(to_string(reason));
  return out;
}

}  // namespace

struct Coordinator::Impl {
  CoordinatorConfig config{};
  std::unique_ptr<StateStore> store{};
  EvidenceStore evidence{1U << 14};
  GrantTable grants{200000};
  std::unique_ptr<Server> server{};
  mutable std::mutex mutex{};
  IdAllocator<DecisionTag> decision_ids{};
  IdAllocator<GrantTag> grant_ids{};
  IdAllocator<FenceTag> fence_ids{};
  IdAllocator<EvidenceTag> evidence_ids{};
  IdAllocator<PlanTag> plan_ids{};
  IdAllocator<AttemptTag> attempt_ids{};
  std::unordered_map<ContractId, std::size_t> contract_index{};
  /// Contracts grouped by scope, so an evaluation sees the whole contention set
  /// instead of only the contract it was asked about.
  std::unordered_map<ScopeId, std::vector<ContractId>> scope_index{};
  std::map<SessionId, PrincipalRole> session_roles{};
  std::unordered_map<std::uint64_t, std::uint64_t> scope_capability_digest{};
  bool policy_installed = false;
  bool stopped = false;
  Tick clock{1};

  [[nodiscard]] Accounting& acct() noexcept { return store->accounting(); }
  [[nodiscard]] DurableState& state() noexcept { return store->mutable_state(); }
  [[nodiscard]] const DurableState& state() const noexcept { return store->state(); }
};

namespace {

Status append_record(Coordinator::Impl& impl, RecordType type,
                     const std::vector<std::uint8_t>& payload) {
  return impl.store->append(type, payload);
}

template <class T>
std::vector<std::uint8_t> encode_body(const T& value) {
  ByteWriter writer;
  encode(writer, value);
  return writer.bytes();
}

/// Generation-advance records carry a single bare counter and nothing else.
std::vector<std::uint8_t> encode_counter_body(std::uint64_t value) {
  ByteWriter writer(16);
  writer.u64(value);
  return writer.bytes();
}

AuthorityVector current_authority(const DurableState& state) noexcept {
  AuthorityVector vector;
  vector.coordinator_term = state.term;
  vector.boot = state.boot;
  vector.fabric = state.fabric_generation;
  vector.capacity = state.capacity_generation;
  vector.policy = state.policy.generation;
  vector.evidence = state.evidence_generation;
  return vector;
}

const ServiceContract* find_contract(Coordinator::Impl& impl, ContractId id) noexcept {
  const auto position = impl.contract_index.find(id);
  if (position == impl.contract_index.end()) return nullptr;
  if (position->second >= impl.state().contracts.size()) return nullptr;
  return &impl.state().contracts[position->second];
}

void rebuild_contract_index(Coordinator::Impl& impl) {
  impl.contract_index.clear();
  impl.scope_index.clear();
  const std::vector<ServiceContract>& contracts = impl.state().contracts;
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    impl.contract_index.emplace(contracts[i].id, i);
    if (contracts[i].active) impl.scope_index[contracts[i].scope].push_back(contracts[i].id);
  }
  for (auto& entry : impl.scope_index) {
    std::sort(entry.second.begin(), entry.second.end());
  }
}

/// The rebinding a surviving grant receives: same contract, subject and scope,
/// current generations.
AuthorityBinding current_binding_for_grant(const Grant& grant, const AuthorityVector& current) {
  AuthorityBinding binding = grant.binding;
  binding.vector = current;
  return binding;
}

FreshnessWindow freshness_for(const DurableState& state, Tick now, bool enforced) noexcept {
  FreshnessWindow window;
  window.now = now;
  window.budget_ticks = state.policy.evidence_freshness_ticks;
  window.enforced = enforced;
  return window;
}

/// Commits a fence for one grant. p out receives the record when requested.
Status fence_grant_locked(Coordinator::Impl& impl, GrantId id, FenceReason reason,
                          AuthorityDelta delta, Tick now, FenceRecord* out) {
  DurableState& state = impl.state();
  const Grant* grant = impl.grants.find(state.grants, id);
  if (grant == nullptr) return Status(ErrorCode::NotFound, "grant is not retained");
  if (!grant->live()) return Status(ErrorCode::InvalidState, "grant is already terminal");
  FenceRecord record;
  const auto fence_id = impl.fence_ids.allocate();
  if (!fence_id.has_value()) return Status(ErrorCode::Exhausted, "fence identity space exhausted");
  record.id = *fence_id;
  record.grant = id;
  record.reason = reason;
  record.delta = delta;
  record.prior = grant->binding;
  record.current = grant->binding;
  record.current.vector = current_authority(state);
  record.fenced_tick = now;
  const GrantState prior = grant->state;

  RecordType type = RecordType::GrantFenced;
  if (reason == FenceReason::Expiry) type = RecordType::GrantExpired;
  if (reason == FenceReason::Revocation || reason == FenceReason::Restored) {
    type = RecordType::GrantRevoked;
  }
  const GrantState terminal = type == RecordType::GrantExpired  ? GrantState::Expired
                              : type == RecordType::GrantRevoked ? GrantState::Revoked
                                                                 : GrantState::Fenced;
  Status status = append_record(impl, type, encode_body(record));
  if (!status.ok()) return status;
  status = impl.grants.note_transition(id, prior, terminal);
  if (!status.ok()) return status;
  if (out != nullptr) *out = record;
  return Status{};
}

Result<Decision> evaluate_locked(Coordinator::Impl& impl, ContractId contract,
                                AllocationPlan* out_plan);

/// Sweeps every live grant.
///
/// Expiry and every change to the grant's *authority* (boot, term, policy,
/// contract, subject, scope) fence it outright. A change to the observed
/// capability is different: the grant is re-evaluated, and it survives only when
/// the runtime can still prove that what it promised is serviceable. Everything
/// else is fenced. A grant is therefore never carried across a change on the
/// strength of its identity alone.
RevalidateReport sweep_locked(Coordinator::Impl& impl, Tick now) {
  RevalidateReport report;
  DurableState& state = impl.state();
  const AuthorityVector current = current_authority(state);
  const std::vector<GrantId> live = impl.grants.live_ids();
  for (const GrantId id : live) {
    const Grant* grant = impl.grants.find(state.grants, id);
    if (grant == nullptr || !grant->live()) continue;
    const Grant snapshot = *grant;
    ++report.evaluated;
    if (snapshot.expired_at(now)) {
      Status status =
          fence_grant_locked(impl, id, FenceReason::Expiry, AuthorityDelta::None, now, nullptr);
      if (status.ok()) ++report.expired;
      continue;
    }
    AuthorityBinding refreshed = snapshot.binding;
    refreshed.vector = current;
    const AuthorityDelta delta = snapshot.binding.classify(refreshed);
    if (!invalidates_authority(delta)) continue;

    const AuthorityDelta authority_delta =
        delta & (AuthorityDelta::Boot | AuthorityDelta::CoordinatorTerm | AuthorityDelta::Policy |
                 AuthorityDelta::Contract | AuthorityDelta::ContractGeneration |
                 AuthorityDelta::Subject | AuthorityDelta::SubjectGeneration |
                 AuthorityDelta::Scope);
    if (invalidates_authority(authority_delta)) {
      Status status =
          fence_grant_locked(impl, id, fence_reason_for(authority_delta), authority_delta, now,
                             nullptr);
      if (status.ok()) ++report.fenced;
      continue;
    }

    // Only the observed capability moved. Re-evaluate the obligation and keep the
    // grant alive only if what it promised is still serviceable.
    auto decision = evaluate_locked(impl, snapshot.binding.contract, nullptr);
    if (!decision.ok() || !is_authorising(decision.value().outcome) ||
        !weakens_or_equals(decision.value().approved, snapshot.degraded)) {
      Status status =
          fence_grant_locked(impl, id, fence_reason_for(delta), delta, now, nullptr);
      if (status.ok()) ++report.fenced;
      continue;
    }
    ByteWriter rebind;
    rebind.u64(id.value());
    encode(rebind, current_binding_for_grant(snapshot, current));
    const Status rebound = append_record(impl, RecordType::GrantRebound, rebind.bytes());
    if (!rebound.ok()) {
      Status status =
          fence_grant_locked(impl, id, FenceReason::AuthorityUnprovable, delta, now, nullptr);
      if (status.ok()) ++report.fenced;
    }
  }
  return report;
}

Result<Decision> evaluate_locked(Coordinator::Impl& impl, ContractId contract,
                                  AllocationPlan* out_plan) {
  DurableState& state = impl.state();
  const ServiceContract* registered = find_contract(impl, contract);
  if (registered == nullptr) return Status(ErrorCode::NotFound, "contract is not registered");
  if (!impl.policy_installed) return Status(ErrorCode::InvalidState, "no policy is installed");
  const ServiceContract snapshot = *registered;
  auto capability =
      impl.evidence.capability(snapshot.scope, state.capacity_generation,
                               freshness_for(state, impl.clock, true));
  if (!capability.ok()) return capability.status();

  const auto decision_id = impl.decision_ids.allocate();
  if (!decision_id.has_value()) {
    return Status(ErrorCode::Exhausted, "decision identity space exhausted");
  }
  const auto plan_id = impl.plan_ids.allocate();
  if (!plan_id.has_value()) return Status(ErrorCode::Exhausted, "plan identity space exhausted");

  EvaluationContext context;
  context.binding.vector = current_authority(state);
  context.binding.contract = snapshot.id;
  context.binding.contract_generation = snapshot.generation;
  context.binding.subject = snapshot.subject;
  context.binding.subject_generation = snapshot.subject_generation;
  context.binding.scope = snapshot.scope;
  context.now = impl.clock;
  context.decision_id = *decision_id;
  context.plan_id = *plan_id;
  switch (impl.config.origin) {
    case OriginClass::Real: context.provenance = ProvenanceClass::Real; break;
    case OriginClass::Unsupported: context.provenance = ProvenanceClass::Unsupported; break;
    case OriginClass::Unspecified:
    case OriginClass::Synthetic: context.provenance = ProvenanceClass::Synthetic; break;
  }

  // Every contract contending for the same scope participates, so weaker
  // traffic absorbs degradation before protected traffic.
  std::vector<const ServiceContract*> contending;
  const auto scope_entry = impl.scope_index.find(snapshot.scope);
  if (scope_entry != impl.scope_index.end()) {
    contending.reserve(scope_entry->second.size());
    for (const ContractId id : scope_entry->second) {
      const ServiceContract* member = find_contract(impl, id);
      if (member != nullptr) contending.push_back(member);
    }
  }
  if (contending.empty()) contending.push_back(&snapshot);

  AllocationRequest allocation_request;
  allocation_request.scope = snapshot.scope;
  allocation_request.contracts = contending;
  allocation_request.capability = &capability.value();
  allocation_request.evidence = &impl.evidence.live();
  allocation_request.policy = &state.policy;
  allocation_request.context = context;
  auto plan = allocate_scope(allocation_request);
  if (!plan.ok()) return plan.status();
  const AllocationEntry* entry = plan.value().find(snapshot.id);
  if (entry == nullptr) {
    return Status(ErrorCode::InvalidState, "the allocation did not account for the contract");
  }

  ContractEvaluationRequest request;
  request.contract = &snapshot;
  request.capability = &capability.value();
  request.evidence = &impl.evidence.live();
  request.policy = &state.policy;
  request.context = context;
  request.allocation = entry;
  request.plan_status = plan.value().status;
  Decision decision = evaluate_contract(request);
  if (out_plan != nullptr) *out_plan = plan.value();
  impl.plan_ids.observe(plan_id->value());
  if (!decision.validate().ok()) {
    return Status(ErrorCode::InvalidState, "decision failed its own invariant check");
  }
  const Status recorded = append_record(impl, RecordType::DecisionRecorded, encode_body(decision));
  if (!recorded.ok()) return recorded;
  return decision;
}

Result<Grant> acquire_locked(Coordinator::Impl& impl, ContractId contract, Tick now) {
  if (impl.stopped) return Status(ErrorCode::Shutdown, "coordinator has stopped");
  DurableState& state = impl.state();
  const ServiceContract* registered = find_contract(impl, contract);
  if (registered == nullptr) return Status(ErrorCode::NotFound, "contract is not registered");
  const ServiceContract snapshot = *registered;
  auto decision = evaluate_locked(impl, contract, nullptr);
  if (!decision.ok()) return decision.status();
  if (!is_authorising(decision.value().outcome)) {
    return Status(ErrorCode::Refused,
                  reason_text(decision.value().primary_reason, decision.value().outcome));
  }
  if (impl.grants.live_count() >= impl.config.max_live_grants) {
    return Status(ErrorCode::Exhausted, "live grant table is full");
  }
  const auto grant_id = impl.grant_ids.allocate();
  const auto attempt_id = impl.attempt_ids.allocate();
  if (!grant_id.has_value()) return Status(ErrorCode::Exhausted, "grant identity space exhausted");
  if (!attempt_id.has_value()) return Status(ErrorCode::Exhausted, "attempt identity exhausted");

  Grant grant;
  grant.id = *grant_id;
  grant.decision = decision.value().id;
  grant.binding = decision.value().binding;
  grant.service_class = snapshot.service_class;
  grant.protected_obligation = snapshot.protected_obligation();
  grant.original = decision.value().original;
  grant.degraded = decision.value().approved;
  grant.delta = decision.value().delta;
  grant.reason = decision.value().primary_reason;
  grant.evidence = decision.value().evidence;
  grant.evidence_state = decision.value().evidence_state;
  grant.preconditions = make_preconditions(snapshot, state.policy);
  grant.sequence = SequenceNumber::from_value(1);
  grant.last_attempt = *attempt_id;
  grant.issued_tick = now;
  grant.expires_tick = decision.value().expires_tick;
  grant.last_transition_tick = now;
  grant.state = GrantState::Issued;
  grant.provenance = decision.value().provenance;
  Status status = grant.validate();
  if (!status.ok()) return status;
  status = append_record(impl, RecordType::GrantIssued, encode_body(grant));
  if (!status.ok()) return status;
  status = impl.grants.note_insert(grant);
  if (!status.ok()) return status;
  if (grant.id.value() > state.grant_id_high_water) state.grant_id_high_water = grant.id.value();
  impl.attempt_ids.observe(attempt_id->value());
  return grant;
}

Status install_policy_locked(Coordinator::Impl& impl, const Policy& policy) {
  Status status = policy.validate();
  if (!status.ok()) return status;
  DurableState& state = impl.state();
  if (state.policy.generation.valid() && policy.generation <= state.policy.generation) {
    return Status(ErrorCode::SequenceRegression, "policy generation did not advance");
  }
  status = append_record(impl, RecordType::PolicyInstalled, encode_body(policy));
  if (!status.ok()) return status;
  impl.policy_installed = true;
  (void)sweep_locked(impl, impl.clock);
  return Status{};
}

Status register_contract_locked(Coordinator::Impl& impl, const ServiceContract& contract) {
  DurableState& state = impl.state();
  ServiceContract stored = contract;
  const ServiceContract* existing = find_contract(impl, stored.id);
  if (!stored.generation.valid()) {
    const std::uint64_t next = existing != nullptr ? existing->generation.value() + 1 : 1;
    stored.generation = ContractGeneration::from_value(next);
  }
  Status status = stored.validate();
  if (!status.ok()) return status;
  if (existing != nullptr && stored.generation < existing->generation) {
    return Status(ErrorCode::SequenceRegression, "contract generation regressed");
  }
  if (existing != nullptr && stored.generation == existing->generation &&
      existing->digest() == stored.digest()) {
    return Status{};
  }
  if (existing == nullptr && state.contracts.size() >= impl.config.max_contracts) {
    return Status(ErrorCode::Exhausted, "contract table is full");
  }
  status = append_record(impl, RecordType::ContractRegistered, encode_body(stored));
  if (!status.ok()) return status;
  if (existing == nullptr) {
    impl.contract_index.emplace(stored.id, impl.state().contracts.size() - 1);
    impl.scope_index[stored.scope].push_back(stored.id);
    std::sort(impl.scope_index[stored.scope].begin(), impl.scope_index[stored.scope].end());
  }
  return Status{};
}

Status publish_evidence_locked(Coordinator::Impl& impl, const EvidenceItem& item) {
  DurableState& state = impl.state();
  EvidenceItem stamped = item;
  if (stamped.observed_tick.value == 0) stamped.observed_tick = impl.clock;
  if (stamped.observed_tick > impl.clock) {
    return Status(ErrorCode::InvalidArgument, "evidence is stamped in the future");
  }
  if (!stamped.id.valid()) {
    const auto id = impl.evidence_ids.allocate();
    if (!id.has_value()) return Status(ErrorCode::Exhausted, "evidence identity space exhausted");
    stamped.id = *id;
  }
  bool superseded = false;
  Status status = impl.evidence.publish(stamped, superseded);
  if (!status.ok()) {
    (void)impl.acct().record_evidence_rejected();
    return status;
  }
  (void)impl.acct().record_evidence_published();
  if (superseded) (void)impl.acct().record_evidence_superseded(1);

  const std::uint64_t scope_key = stamped.scope.value();
  auto capability = impl.evidence.capability(stamped.scope, state.capacity_generation,
                                             freshness_for(state, impl.clock, false));
  if (!capability.ok()) return capability.status();
  const std::uint64_t digest = capability.value().digest();
  const auto previous = impl.scope_capability_digest.find(scope_key);
  if (previous != impl.scope_capability_digest.end() && previous->second == digest) {
    return Status{};  // no material change: nothing to commit, no authority invalidated
  }
  const bool first_observation = previous == impl.scope_capability_digest.end();
  impl.scope_capability_digest[scope_key] = digest;

  status = append_record(impl, RecordType::EvidencePublished, encode_body(stamped));
  if (!status.ok()) return status;
  if (!state.fabric_generation.valid()) {
    status = append_record(impl, RecordType::FabricGenerationAdvanced,
                           encode_counter_body(1));
    if (!status.ok()) return status;
  }
  if (first_observation) {
    const auto next_fabric = state.fabric_generation.next();
    if (!next_fabric.has_value()) {
      return Status(ErrorCode::Exhausted, "fabric generation exhausted");
    }
    status = append_record(impl, RecordType::FabricGenerationAdvanced,
                           encode_counter_body(next_fabric->value()));
    if (!status.ok()) return status;
  }
  if (!state.capacity_generation.valid()) {
    status = append_record(impl, RecordType::CapacityGenerationAdvanced,
                           encode_counter_body(1));
    if (!status.ok()) return status;
  }
  const auto next_capacity = state.capacity_generation.next();
  if (!next_capacity.has_value()) {
    return Status(ErrorCode::Exhausted, "capacity generation exhausted");
  }
  status = append_record(impl, RecordType::CapacityGenerationAdvanced,
                         encode_counter_body(next_capacity->value()));
  if (!status.ok()) return status;
  if (!state.evidence_generation.valid()) {
    status = append_record(impl, RecordType::EvidenceGenerationAdvanced,
                           encode_counter_body(1));
    if (!status.ok()) return status;
  }
  const auto next_evidence = state.evidence_generation.next();
  if (!next_evidence.has_value()) {
    return Status(ErrorCode::Exhausted, "evidence generation exhausted");
  }
  status = append_record(impl, RecordType::EvidenceGenerationAdvanced,
                         encode_counter_body(next_evidence->value()));
  if (!status.ok()) return status;
  (void)sweep_locked(impl, impl.clock);
  return Status{};
}

Result<Grant> acknowledge_locked(Coordinator::Impl& impl, GrantId grant, AttemptId attempt,
                                 Tick now) {
  DurableState& state = impl.state();
  const Grant* found = impl.grants.find(state.grants, grant);
  if (found == nullptr) return Status(ErrorCode::NotFound, "grant is not retained");
  const Grant snapshot = *found;
  if (!snapshot.live()) return Status(ErrorCode::InvalidState, "grant is already terminal");
  if (snapshot.expired_at(now)) {
    (void)fence_grant_locked(impl, grant, FenceReason::Expiry, AuthorityDelta::None, now, nullptr);
    return Status(ErrorCode::Stale, "grant expired before it was acknowledged");
  }
  if (!attempt.valid() || attempt != snapshot.last_attempt) {
    return Status(ErrorCode::ReplayDetected, "attempt identity is not the current one");
  }
  if (!grant_transition_allowed(snapshot.state, GrantState::Acknowledged)) {
    return Status(ErrorCode::InvalidState, "grant cannot be acknowledged from its current state");
  }
  const auto next_attempt = impl.attempt_ids.allocate();
  const auto next_sequence = snapshot.sequence.next();
  if (!next_attempt.has_value() || !next_sequence.has_value()) {
    return Status(ErrorCode::Exhausted, "attempt or sequence identity space exhausted");
  }
  ByteWriter writer(48);
  writer.u64(grant.value());
  writer.u64(now.value);
  writer.u64(next_attempt->value());
  writer.u64(next_sequence->value());
  Status status = append_record(impl, RecordType::GrantAcknowledged, writer.bytes());
  if (!status.ok()) return status;
  status = impl.grants.note_transition(grant, snapshot.state, GrantState::Acknowledged);
  if (!status.ok()) return status;
  impl.attempt_ids.observe(next_attempt->value());
  const Grant* updated = impl.grants.find(impl.state().grants, grant);
  if (updated == nullptr) return Status(ErrorCode::NotFound, "grant disappeared after commit");
  return *updated;
}

Result<Grant> report_applied_locked(Coordinator::Impl& impl, GrantId grant, AttemptId attempt,
                                    Tick now) {
  DurableState& state = impl.state();
  const Grant* found = impl.grants.find(state.grants, grant);
  if (found == nullptr) return Status(ErrorCode::NotFound, "grant is not retained");
  const Grant snapshot = *found;
  if (!snapshot.live()) return Status(ErrorCode::InvalidState, "grant is already terminal");
  if (snapshot.expired_at(now)) {
    (void)fence_grant_locked(impl, grant, FenceReason::Expiry, AuthorityDelta::None, now, nullptr);
    return Status(ErrorCode::Stale, "grant expired before the effect was reported");
  }
  if (!attempt.valid() || attempt != snapshot.last_attempt) {
    return Status(ErrorCode::ReplayDetected, "attempt identity is not the current one");
  }
  if (!grant_transition_allowed(snapshot.state, GrantState::Applied)) {
    return Status(ErrorCode::InvalidState, "grant cannot be applied from its current state");
  }
  const auto next_attempt = impl.attempt_ids.allocate();
  const auto next_sequence = snapshot.sequence.next();
  if (!next_attempt.has_value() || !next_sequence.has_value()) {
    return Status(ErrorCode::Exhausted, "attempt or sequence identity space exhausted");
  }
  ByteWriter writer(48);
  writer.u64(grant.value());
  writer.u64(now.value);
  writer.u64(next_attempt->value());
  writer.u64(next_sequence->value());
  Status status = append_record(impl, RecordType::GrantApplied, writer.bytes());
  if (!status.ok()) return status;
  status = impl.grants.note_transition(grant, snapshot.state, GrantState::Applied);
  if (!status.ok()) return status;
  impl.attempt_ids.observe(next_attempt->value());
  const Grant* updated = impl.grants.find(impl.state().grants, grant);
  if (updated == nullptr) return Status(ErrorCode::NotFound, "grant disappeared after commit");
  return *updated;
}

Result<RestorationEvaluation> request_restoration_locked(Coordinator::Impl& impl, GrantId grant,
                                                         Tick now) {
  DurableState& state = impl.state();
  const Grant* found = impl.grants.find(state.grants, grant);
  if (found == nullptr) return Status(ErrorCode::NotFound, "grant is not retained");
  const Grant snapshot = *found;
  const ServiceContract* registered = find_contract(impl, snapshot.binding.contract);
  if (registered == nullptr) {
    return Status(ErrorCode::NotFound, "the grant's contract is not registered");
  }
  auto capability =
      impl.evidence.capability(snapshot.binding.scope, state.capacity_generation,
                               freshness_for(state, impl.clock, true));
  if (!capability.ok()) return capability.status();

  RestorationRequest request;
  request.grant = &snapshot;
  request.contract = registered;
  request.capability = &capability.value();
  request.evidence = &impl.evidence.live();
  request.policy = &state.policy;
  request.current_authority = current_authority(state);
  request.fence_active = false;
  request.now = now;
  request.provenance = snapshot.provenance;
  RestorationEvaluation evaluation = evaluate_restoration(request);
  Status status = Status{};
  if (evaluation.outcome == RestorationOutcome::Proven) {
    status = append_record(impl, RecordType::RestorationProven, encode_body(evaluation));
    if (!status.ok()) return status;
    (void)impl.acct().record_restoration_proven();
    (void)fence_grant_locked(impl, grant, FenceReason::Restored, AuthorityDelta::None, now,
                             nullptr);
  } else {
    status = append_record(impl, RecordType::RestorationRefused, encode_body(evaluation));
    if (!status.ok()) return status;
    (void)impl.acct().record_restoration_refused();
  }
  return evaluation;
}

StatusBody describe_locked(Coordinator::Impl& impl) {
  const DurableState& state = impl.state();
  StatusBody body;
  body.authority = current_authority(state);
  body.boot = state.boot;
  body.term = state.term;
  body.evidence_generation = state.evidence_generation;
  body.capacity_generation = state.capacity_generation;
  body.fabric_generation = state.fabric_generation;
  body.live_grants = impl.grants.live_count();
  body.retained_grants = state.grants.size();
  body.retained_fences = state.fences.size();
  body.retained_decisions = state.decisions.size();
  body.retained_contracts = state.contracts.size();
  body.journal_records = state.journal_records;
  body.journal_bytes_in_segment = impl.store->bytes_in_segment();
  ClosureInputs inputs;
  inputs.live_grants = body.live_grants;
  inputs.retained_terminated_grants = impl.grants.terminated_count();
  inputs.retained_fences = state.fences.size();
  inputs.retained_decisions = state.decisions.size();
  const ClosureReport closure = impl.acct().check_closure(inputs);
  body.accounting_closed = closure.closed;
  body.policy_installed = impl.policy_installed;
  body.policy_digest = to_hex(state.policy.digest());
  body.recovery = std::string(to_string(impl.store->recovery()));
  body.recovery_detail = impl.store->recovery_detail();
  return body;
}

}  // namespace

Coordinator::Coordinator() : impl_(std::make_unique<Impl>()) {}

Coordinator::~Coordinator() {
  Status status = stop();
  (void)status;
}

Result<std::unique_ptr<Coordinator>> Coordinator::create(const CoordinatorConfig& config) {
  auto coordinator = std::unique_ptr<Coordinator>(new Coordinator());
  Impl& impl = *coordinator->impl_;
  impl.config = config;

  StoreConfig store_config = config.store;
  store_config.max_retained_grants =
      std::max<std::size_t>(store_config.max_retained_grants, config.max_live_grants);
  store_config.max_retained_decisions =
      std::max<std::size_t>(store_config.max_retained_decisions, config.max_decisions);
  auto store = StateStore::open(store_config);
  if (!store.ok()) return store.status();
  impl.store = std::move(store.value());
  impl.evidence.reset_limit(config.max_evidence_items);
  impl.grants = GrantTable(config.max_live_grants);
  // The index must exist before the pre-restart fence sweep, which looks every
  // live grant up through it.
  impl.grants.rebuild(impl.state().grants);

  DurableState& state = impl.state();
  impl.decision_ids.observe(state.decision_id_high_water);
  impl.grant_ids.observe(state.grant_id_high_water);
  impl.fence_ids.observe(state.fence_id_high_water);
  impl.evidence_ids.observe(state.evidence_id_high_water);
  impl.plan_ids.observe(state.plan_id_high_water);
  impl.clock = state.last_commit_tick.value > 0 ? state.last_commit_tick : Tick{1};

  // A restart creates a fresh incarnation and advances the coordinator term, so
  // no authority issued by the previous incarnation can still be current.
  const std::optional<BootIncarnation> next_boot =
      state.boot.valid() ? state.boot.next() : std::optional<BootIncarnation>(BootIncarnation::from_value(1));
  const std::optional<CoordinatorTerm> next_term =
      state.term.valid() ? state.term.next() : std::optional<CoordinatorTerm>(CoordinatorTerm::from_value(1));
  if (!next_boot.has_value() || !next_term.has_value()) {
    return Status(ErrorCode::Exhausted, "boot or term identity space is exhausted");
  }
  {
    ByteWriter writer(32);
    writer.u64(next_boot->value());
    writer.u64(next_term->value());
    writer.u64(impl.clock.value);
    Status status = append_record(impl, RecordType::BootAdvanced, writer.bytes());
    if (!status.ok()) return status;
  }
  struct GenerationBootstrap {
    bool missing;
    RecordType type;
  };
  const GenerationBootstrap bootstraps[] = {
      {!state.fabric_generation.valid(), RecordType::FabricGenerationAdvanced},
      {!state.capacity_generation.valid(), RecordType::CapacityGenerationAdvanced},
      {!state.evidence_generation.valid(), RecordType::EvidenceGenerationAdvanced},
  };
  for (const GenerationBootstrap& bootstrap : bootstraps) {
    if (!bootstrap.missing) continue;
    Status status = append_record(impl, bootstrap.type, encode_counter_body(1));
    if (!status.ok()) return status;
  }

  std::vector<GrantId> previously_live;
  for (const Grant& grant : state.grants) {
    if (grant.live()) previously_live.push_back(grant.id);
  }
  for (const GrantId id : previously_live) {
    const Grant* grant = impl.grants.find(state.grants, id);
    if (grant == nullptr) continue;
    FenceRecord record;
    const auto fence_id = impl.fence_ids.allocate();
    if (!fence_id.has_value()) break;
    record.id = *fence_id;
    record.grant = grant->id;
    record.reason = FenceReason::BootAdvance;
    record.delta = AuthorityDelta::Boot;
    record.prior = grant->binding;
    record.current = grant->binding;
    record.fenced_tick = impl.clock;
    Status status = append_record(impl, RecordType::GrantFenced, encode_body(record));
    if (!status.ok()) return status;
  }
  // The fence sweep above mutated the durable grant table through apply_record,
  // so the index is rebuilt once more before anything can observe it.
  impl.grants.rebuild(state.grants);
  rebuild_contract_index(impl);
  impl.evidence.clear();
  impl.policy_installed = state.policy.installed() && state.policy.validate().ok();
  impl.scope_capability_digest.clear();
  impl.stopped = false;
  return coordinator;
}

Status Coordinator::install_policy(const Policy& policy) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return install_policy_locked(*impl_, policy);
}

Status Coordinator::register_contract(const ServiceContract& contract) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return register_contract_locked(*impl_, contract);
}

Status Coordinator::register_contracts(const std::vector<ServiceContract>& contracts) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  DurableState& state = impl_->state();
  std::vector<ServiceContract> prepared;
  prepared.reserve(contracts.size());
  for (const ServiceContract& contract : contracts) {
    ServiceContract stored = contract;
    const ServiceContract* existing = find_contract(*impl_, stored.id);
    if (!stored.generation.valid()) {
      const std::uint64_t next = existing != nullptr ? existing->generation.value() + 1 : 1;
      stored.generation = ContractGeneration::from_value(next);
    }
    Status status = stored.validate();
    if (!status.ok()) return status;
    if (existing != nullptr && stored.generation < existing->generation) {
      return Status(ErrorCode::SequenceRegression, "contract generation regressed");
    }
    if (existing == nullptr && state.contracts.size() + prepared.size() >= impl_->config.max_contracts) {
      return Status(ErrorCode::Exhausted, "contract table is full");
    }
    prepared.push_back(stored);
  }
  for (const ServiceContract& stored : prepared) {
    const bool is_new = find_contract(*impl_, stored.id) == nullptr;
    Status status = impl_->store->append_batched(
        RecordType::ContractRegistered, encode_body(stored));
    if (!status.ok()) return status;
    if (is_new) {
      impl_->contract_index.emplace(stored.id, impl_->state().contracts.size() - 1);
      impl_->scope_index[stored.scope].push_back(stored.id);
    }
  }
  Status flushed = impl_->store->flush();
  if (!flushed.ok()) return flushed;
  for (auto& entry : impl_->scope_index) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  return Status{};
}

Status Coordinator::publish_evidence(const EvidenceItem& item) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return publish_evidence_locked(*impl_, item);
}

Result<Decision> Coordinator::evaluate(ContractId contract) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return evaluate_locked(*impl_, contract, nullptr);
}

Result<EvaluationView> Coordinator::evaluate_with_plan(ContractId contract) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AllocationPlan plan;
  auto decision = evaluate_locked(*impl_, contract, &plan);
  if (!decision.ok()) return decision.status();
  EvaluationView view;
  static_cast<Decision&>(view) = decision.value();
  view.plan = std::move(plan);
  return view;
}

Result<Grant> Coordinator::acquire(ContractId contract, Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return acquire_locked(*impl_, contract, now);
}

Result<Grant> Coordinator::acknowledge(GrantId grant, AttemptId attempt, Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return acknowledge_locked(*impl_, grant, attempt, now);
}

Result<Grant> Coordinator::report_applied(GrantId grant, AttemptId attempt, Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return report_applied_locked(*impl_, grant, attempt, now);
}

Result<FenceRecord> Coordinator::fence(GrantId grant, FenceReason reason, Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  FenceRecord record;
  Status status =
      fence_grant_locked(*impl_, grant, reason, AuthorityDelta::None, now, &record);
  if (!status.ok()) return status;
  return record;
}

Result<RestorationEvaluation> Coordinator::request_restoration(GrantId grant, Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return request_restoration_locked(*impl_, grant, now);
}

RevalidateReport Coordinator::revalidate(Tick now) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return sweep_locked(*impl_, now);
}

Result<CapabilitySnapshot> Coordinator::capability(ScopeId scope, Tick now) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const DurableState& state = impl_->state();
  return impl_->evidence.capability(scope, state.capacity_generation,
                                    freshness_for(state, now, true));
}

StatusBody Coordinator::describe(Tick now) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  (void)now;
  return describe_locked(*impl_);
}

Status Coordinator::start() {
  std::unique_lock<std::mutex> guard(impl_->mutex);
  if (impl_->server != nullptr) return Status(ErrorCode::InvalidState, "server already started");
  impl_->server = std::make_unique<Server>(impl_->config.server, *this, &impl_->acct());
  guard.unlock();
  Status status = impl_->server->start();
  if (!status.ok()) {
    guard.lock();
    impl_->server.reset();
    return status;
  }
  port_ = impl_->server->port();
  return Status{};
}

Status Coordinator::stop() {
  if (impl_ == nullptr) return Status{};
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->stopped) return Status{};
    impl_->stopped = true;
  }
  // The server is stopped without holding the state lock: a session thread
  // finishing its last request must be able to take it.
  if (impl_->server != nullptr) {
    Status status = impl_->server->stop();
    (void)status;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->store != nullptr && !impl_->store->closed()) {
    impl_->grants.rebuild(impl_->state().grants);
    Status status = impl_->store->take_snapshot();
    (void)status;
    return impl_->store->close();
  }
  return Status{};
}

Tick Coordinator::now() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->clock;
}

Tick Coordinator::advance(std::uint64_t delta) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto next = checked_add(impl_->clock.value, delta);
  impl_->clock = next.has_value() ? Tick{*next} : Tick{std::numeric_limits<std::uint64_t>::max()};
  return impl_->clock;
}

const DurableState& Coordinator::state() const { return impl_->store->state(); }

AuthorityVector Coordinator::authority() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return current_authority(impl_->state());
}

AccountingCounters Coordinator::counters() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->acct().counters();
}

RecoveryOutcome Coordinator::recovery() const noexcept {
  return impl_->store != nullptr ? impl_->store->recovery() : RecoveryOutcome::FreshStore;
}

Status Coordinator::on_hello(SessionId session, const HelloRequest& request,
                             HelloResponse& response) {
  Status status = request.validate();
  if (!status.ok()) return status;
  PrincipalRole role = PrincipalRole::Observer;
  if (impl_->config.allow_anonymous_sessions) {
    role = PrincipalRole::Administrator;
  } else {
    if (impl_->config.principals.empty()) {
      return Status(ErrorCode::Unauthorized, "coordinator has no principal configured");
    }
    bool matched = false;
    for (const auto& entry : impl_->config.principals) {
      // Every configured token is compared, so the comparison cost does not
      // reveal which token, if any, matched.
      const bool equal = constant_time_equals(request.token, entry.first);
      if (equal && !matched) {
        role = entry.second;
        matched = true;
      }
    }
    if (!matched) return Status(ErrorCode::Unauthorized, "bearer token was rejected");
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopped) return Status(ErrorCode::Shutdown, "coordinator has stopped");
  const DurableState& state = impl_->state();
  impl_->session_roles[session] = role;
  response.term = state.term;
  response.boot = state.boot;
  response.authority = current_authority(state);
  response.detail = "session accepted";
  return Status{};
}

Status Coordinator::on_request(SessionId session, const RequestEnvelope& envelope, MessageType type,
                               const std::vector<std::uint8_t>& body, ResponseEnvelope& response,
                               std::vector<std::uint8_t>& response_body) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopped) return Status(ErrorCode::Shutdown, "coordinator has stopped");
  const DurableState& state = impl_->state();
  response.term = state.term;
  response.boot = state.boot;

  const auto role_entry = impl_->session_roles.find(session);
  if (role_entry == impl_->session_roles.end()) {
    return Status(ErrorCode::Unauthorized, "session has no authenticated role");
  }
  if (static_cast<std::uint16_t>(role_entry->second) <
      static_cast<std::uint16_t>(required_role(type))) {
    return Status(ErrorCode::Unauthorized, "session role does not authorise this request");
  }
  if (envelope.term != state.term) {
    return Status(ErrorCode::Stale, "request carries a stale coordinator term");
  }
  if (envelope.boot != state.boot) {
    return Status(ErrorCode::Stale, "request carries a stale boot incarnation");
  }

  const auto numeric = [&body](std::uint64_t& out) { return decode_id_body(body, out); };
  switch (type) {
    case MessageType::PublishEvidence: {
      EvidenceItem item;
      Status status = decode_object_body(body, item);
      if (!status.ok()) return status;
      return publish_evidence_locked(*impl_, item);
    }
    case MessageType::RegisterContract: {
      ServiceContract contract;
      Status status = decode_object_body(body, contract);
      if (!status.ok()) return status;
      return register_contract_locked(*impl_, contract);
    }
    case MessageType::InstallPolicy: {
      Policy policy;
      Status status = decode_object_body(body, policy);
      if (!status.ok()) return status;
      return install_policy_locked(*impl_, policy);
    }
    case MessageType::EvaluateContract: {
      std::uint64_t raw = 0;
      Status status = numeric(raw);
      if (!status.ok()) return status;
      AllocationPlan out_plan;
      auto decision = evaluate_locked(*impl_, ContractId::from_value(raw), &out_plan);
      if (!decision.ok()) return decision.status();
      DecisionBody out;
      out.plan = out_plan;
      out.decision = decision.value();
      if (out.plan.entries.empty()) {
        AllocationEntry entry;
        entry.contract = decision.value().binding.contract;
        entry.contract_generation = decision.value().binding.contract_generation;
        entry.scope = decision.value().binding.scope;
        entry.admitted = is_authorising(decision.value().outcome);
        entry.full = decision.value().delta.empty();
        entry.approved = decision.value().approved;
        entry.delta = decision.value().delta;
        entry.outcome = decision.value().outcome;
        entry.reason = decision.value().primary_reason;
        out.plan.entries.push_back(entry);
      }
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::AcquireAuthority: {
      std::uint64_t raw = 0;
      Status status = numeric(raw);
      if (!status.ok()) return status;
      auto grant = acquire_locked(*impl_, ContractId::from_value(raw), impl_->clock);
      if (!grant.ok()) return grant.status();
      GrantBody out;
      out.grant = grant.value();
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::AcknowledgeGrant: {
      std::uint64_t raw_grant = 0;
      std::uint64_t raw_attempt = 0;
      Status status = decode_attempt_body(body, raw_grant, raw_attempt);
      if (!status.ok()) return status;
      auto result = acknowledge_locked(*impl_, GrantId::from_value(raw_grant),
                                       AttemptId::from_value(raw_attempt), impl_->clock);
      if (!result.ok()) return result.status();
      GrantBody out;
      out.grant = result.value();
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::ReportApplied: {
      std::uint64_t raw_grant = 0;
      std::uint64_t raw_attempt = 0;
      Status status = decode_attempt_body(body, raw_grant, raw_attempt);
      if (!status.ok()) return status;
      auto result = report_applied_locked(*impl_, GrantId::from_value(raw_grant),
                                          AttemptId::from_value(raw_attempt), impl_->clock);
      if (!result.ok()) return result.status();
      GrantBody out;
      out.grant = result.value();
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::FenceGrant: {
      std::uint64_t raw_grant = 0;
      FenceReason reason = FenceReason::Manual;
      Status status = decode_fence_body(body, raw_grant, reason);
      if (!status.ok()) return status;
      FenceRecord record;
      status = fence_grant_locked(*impl_, GrantId::from_value(raw_grant), reason,
                                  AuthorityDelta::None, impl_->clock, &record);
      if (!status.ok()) return status;
      return Status{};
    }
    case MessageType::RequestRestoration: {
      std::uint64_t raw = 0;
      Status status = numeric(raw);
      if (!status.ok()) return status;
      auto evaluation = request_restoration_locked(*impl_, GrantId::from_value(raw), impl_->clock);
      if (!evaluation.ok()) return evaluation.status();
      RestorationBody out;
      out.evaluation = evaluation.value();
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::Revalidate: {
      const RevalidateReport report = sweep_locked(*impl_, impl_->clock);
      RevalidateBody out;
      out.evaluated = report.evaluated;
      out.fenced = report.fenced;
      out.expired = report.expired;
      response_body = encode_body(out);
      return Status{};
    }
    case MessageType::QueryStatus: {
      if (!body.empty()) return Status(ErrorCode::InvalidArgument, "status takes no body");
      response_body = encode_body(describe_locked(*impl_));
      return Status{};
    }
    case MessageType::Goodbye:
      if (!body.empty()) return Status(ErrorCode::InvalidArgument, "goodbye takes no body");
      return Status{};
    default:
      return Status(ErrorCode::Unsupported, "message type is not handled");
  }
}

void Coordinator::on_session_closed(SessionId session) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->session_roles.erase(session);
}

}  // namespace dmf