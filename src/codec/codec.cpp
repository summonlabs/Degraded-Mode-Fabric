// Degraded Mode Fabric - canonical serialisation implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/codec.hpp"

#include <algorithm>

namespace dmf {

namespace {

void write_id(ByteWriter& writer, const auto& id) noexcept { writer.u64(id.value()); }

template <class Tag>
StrongUInt<Tag> read_strong(ByteReader& reader) noexcept {
  return StrongUInt<Tag>::from_value(reader.u64());
}

Status bad(const char* what) { return Status(ErrorCode::Invalid, what); }

}  // namespace

Status decode_finish(const ByteReader& reader) {
  if (!reader.ok()) return Status(reader.error());
  if (!reader.at_end()) return Status(ErrorCode::TrailingGarbage);
  return Status{};
}

// ---------------------------------------------------------------------------
// Guarantees
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const Guarantee& value) {
  write_enum(writer, value.kind);
  write_enum(writer, value.comparator);
  writer.u64(value.value);
  writer.boolean(value.hard);
}

Status decode(ByteReader& reader, Guarantee& value) {
  Guarantee candidate;
  candidate.kind = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.comparator = read_enum<Comparator>(reader, Comparator::AtLeast);
  candidate.value = reader.u64();
  candidate.hard = reader.boolean();
  if (!reader.ok()) return Status(reader.error());
  if (!candidate.valid()) return bad("decoded guarantee is outside its per-kind domain");
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const GuaranteeSet& value) {
  writer.u32(static_cast<std::uint32_t>(value.size()));
  for (const Guarantee& guarantee : value.items()) encode(writer, guarantee);
}

Status decode(ByteReader& reader, GuaranteeSet& value) {
  const std::uint32_t count = reader.count(kMaxGuaranteesPerSet);
  if (!reader.ok()) return Status(reader.error());
  GuaranteeSet candidate;
  for (std::uint32_t i = 0; i < count; ++i) {
    Guarantee guarantee;
    Status status = decode(reader, guarantee);
    if (!status.ok()) return status;
    status = candidate.insert(guarantee);
    if (!status.ok()) return status;
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const Concession& value) {
  write_enum(writer, value.kind);
  writer.u64(value.original_value);
  writer.u64(value.approved_value);
}

Status decode(ByteReader& reader, Concession& value) {
  Concession candidate;
  candidate.kind = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.original_value = reader.u64();
  candidate.approved_value = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  if (candidate.original_value > max_value_for(candidate.kind) ||
      candidate.approved_value > max_value_for(candidate.kind)) {
    return bad("decoded concession is outside the guarantee domain");
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const GuaranteeWithdrawal& value) {
  write_enum(writer, value.kind);
  writer.u64(value.original_value);
  write_enum(writer, value.reason);
}

Status decode(ByteReader& reader, GuaranteeWithdrawal& value) {
  GuaranteeWithdrawal candidate;
  candidate.kind = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.original_value = reader.u64();
  candidate.reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  if (!reader.ok()) return Status(reader.error());
  if (candidate.original_value > max_value_for(candidate.kind)) {
    return bad("decoded withdrawal is outside the guarantee domain");
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const GuaranteeDelta& value) {
  writer.u32(static_cast<std::uint32_t>(value.concessions.size()));
  for (const Concession& concession : value.concessions) encode(writer, concession);
  writer.u32(static_cast<std::uint32_t>(value.withdrawals.size()));
  for (const GuaranteeWithdrawal& withdrawal : value.withdrawals) encode(writer, withdrawal);
}

Status decode(ByteReader& reader, GuaranteeDelta& value) {
  GuaranteeDelta candidate;
  const std::uint32_t concessions = reader.count(kMaxDeltaEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < concessions; ++i) {
    Concession concession;
    Status status = decode(reader, concession);
    if (!status.ok()) return status;
    candidate.concessions.push_back(concession);
  }
  const std::uint32_t withdrawals = reader.count(kMaxDeltaEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < withdrawals; ++i) {
    GuaranteeWithdrawal withdrawal;
    Status status = decode(reader, withdrawal);
    if (!status.ok()) return status;
    candidate.withdrawals.push_back(withdrawal);
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const ConcessionBound& value) {
  write_enum(writer, value.kind);
  writer.u64(value.weakest_allowed_value);
}

Status decode(ByteReader& reader, ConcessionBound& value) {
  ConcessionBound candidate;
  candidate.kind = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.weakest_allowed_value = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  if (candidate.weakest_allowed_value > max_value_for(candidate.kind)) {
    return bad("decoded concession bound is outside the guarantee domain");
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const GuaranteeEnvelope& value) {
  writer.u32(static_cast<std::uint32_t>(value.bounds().size()));
  for (const ConcessionBound& bound : value.bounds()) encode(writer, bound);
  writer.u32(value.max_concessions());
}

Status decode(ByteReader& reader, GuaranteeEnvelope& value) {
  GuaranteeEnvelope candidate;
  const std::uint32_t count = reader.count(kMaxEnvelopeBounds);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < count; ++i) {
    ConcessionBound bound;
    Status status = decode(reader, bound);
    if (!status.ok()) return status;
    status = candidate.insert(bound);
    if (!status.ok()) return status;
  }
  candidate.set_max_concessions(reader.u32());
  if (!reader.ok()) return Status(reader.error());
  Status status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const ServiceContract& value) {
  write_id(writer, value.id);
  write_id(writer, value.generation);
  write_id(writer, value.scope);
  write_id(writer, value.subject);
  write_id(writer, value.subject_generation);
  write_enum(writer, value.service_class);
  writer.boolean(value.non_degradable);
  writer.u32(value.priority);
  encode(writer, value.original);
  encode(writer, value.envelope);
  writer.u64(value.registered_tick.value);
  writer.boolean(value.active);
}

Status decode(ByteReader& reader, ServiceContract& value) {
  ServiceContract candidate;
  candidate.id = read_strong<ContractTag>(reader);
  candidate.generation = read_strong<ContractGenerationTag>(reader);
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.subject = read_strong<SubjectTag>(reader);
  candidate.subject_generation = read_strong<SubjectGenerationTag>(reader);
  candidate.service_class = read_enum<ServiceClass>(reader, ServiceClass::Standard);
  candidate.non_degradable = reader.boolean();
  candidate.priority = reader.u32();
  Status status = decode(reader, candidate.original);
  if (!status.ok()) return status;
  status = decode(reader, candidate.envelope);
  if (!status.ok()) return status;
  candidate.registered_tick.value = reader.u64();
  candidate.active = reader.boolean();
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const EvidenceItem& value) {
  write_id(writer, value.id);
  write_id(writer, value.publisher);
  write_id(writer, value.publisher_boot);
  write_id(writer, value.generation);
  write_enum(writer, value.kind);
  write_id(writer, value.scope);
  write_enum(writer, value.state);
  writer.u64(value.value);
  writer.u64(value.observed_tick.value);
  write_enum(writer, value.origin);
}

Status decode(ByteReader& reader, EvidenceItem& value) {
  EvidenceItem candidate;
  candidate.id = read_strong<EvidenceTag>(reader);
  candidate.publisher = read_strong<PublisherTag>(reader);
  candidate.publisher_boot = read_strong<BootTag>(reader);
  candidate.generation = read_strong<EvidenceGenerationTag>(reader);
  candidate.kind = read_enum<EvidenceKind>(reader, EvidenceKind::FabricTopology);
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.state = read_enum<EvidenceState>(reader, EvidenceState::Unknown);
  candidate.value = reader.u64();
  candidate.observed_tick.value = reader.u64();
  candidate.origin = read_enum<OriginClass>(reader, OriginClass::Unspecified);
  if (!reader.ok()) return Status(reader.error());
  Status status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const EvidenceVector& value) {
  writer.u32(static_cast<std::uint32_t>(value.size()));
  for (const EvidenceItem& item : value.items()) encode(writer, item);
}

Status decode(ByteReader& reader, EvidenceVector& value) {
  const std::uint32_t count = reader.count(kMaxEvidenceItems);
  if (!reader.ok()) return Status(reader.error());
  EvidenceVector candidate;
  for (std::uint32_t i = 0; i < count; ++i) {
    EvidenceItem item;
    Status status = decode(reader, item);
    if (!status.ok()) return status;
    status = candidate.push(item);
    if (!status.ok()) return status;
  }
  Status status = candidate.normalize();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const CapabilitySnapshot& value) {
  write_id(writer, value.scope);
  write_id(writer, value.generation);
  write_enum(writer, value.state);
  write_enum(writer, value.topology);
  writer.u64(value.bandwidth_kbps);
  writer.u64(value.rtt_p99_us);
  writer.u32(value.path_diversity);
  writer.u32(value.reachable_ppm);
  writer.u32(value.node_coverage_ppm);
  writer.boolean(value.synchronous_durability);
  writer.u64(value.observed_tick.value);
  write_enum(writer, value.origin);
}

Status decode(ByteReader& reader, CapabilitySnapshot& value) {
  CapabilitySnapshot candidate;
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.generation = read_strong<CapacityGenerationTag>(reader);
  candidate.state = read_enum<EvidenceState>(reader, EvidenceState::Unknown);
  candidate.topology = read_enum<FabricTopologyClass>(reader, FabricTopologyClass::FullMesh);
  candidate.bandwidth_kbps = reader.u64();
  candidate.rtt_p99_us = reader.u64();
  candidate.path_diversity = reader.u32();
  candidate.reachable_ppm = reader.u32();
  candidate.node_coverage_ppm = reader.u32();
  candidate.synchronous_durability = reader.boolean();
  candidate.observed_tick.value = reader.u64();
  candidate.origin = read_enum<OriginClass>(reader, OriginClass::Unspecified);
  if (!reader.ok()) return Status(reader.error());
  if (candidate.bandwidth_kbps > kMaxBandwidthKbps || candidate.rtt_p99_us > kMaxLatencyUs ||
      candidate.path_diversity > kMaxPathDiversity || candidate.reachable_ppm > kPpmScale ||
      candidate.node_coverage_ppm > kPpmScale) {
    return bad("decoded capability snapshot is outside its domain");
  }
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const AuthorityVector& value) {
  write_id(writer, value.coordinator_term);
  write_id(writer, value.boot);
  write_id(writer, value.fabric);
  write_id(writer, value.capacity);
  write_id(writer, value.policy);
  write_id(writer, value.evidence);
}

Status decode(ByteReader& reader, AuthorityVector& value) {
  AuthorityVector candidate;
  candidate.coordinator_term = read_strong<CoordinatorTermTag>(reader);
  candidate.boot = read_strong<BootTag>(reader);
  candidate.fabric = read_strong<FabricGenerationTag>(reader);
  candidate.capacity = read_strong<CapacityGenerationTag>(reader);
  candidate.policy = read_strong<PolicyGenerationTag>(reader);
  candidate.evidence = read_strong<EvidenceGenerationTag>(reader);
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const AuthorityBinding& value) {
  encode(writer, value.vector);
  write_id(writer, value.contract);
  write_id(writer, value.contract_generation);
  write_id(writer, value.subject);
  write_id(writer, value.subject_generation);
  write_id(writer, value.scope);
}

Status decode(ByteReader& reader, AuthorityBinding& value) {
  AuthorityBinding candidate;
  Status status = decode(reader, candidate.vector);
  if (!status.ok()) return status;
  candidate.contract = read_strong<ContractTag>(reader);
  candidate.contract_generation = read_strong<ContractGenerationTag>(reader);
  candidate.subject = read_strong<SubjectTag>(reader);
  candidate.subject_generation = read_strong<SubjectGenerationTag>(reader);
  candidate.scope = read_strong<ScopeTag>(reader);
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Decision and plan
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const GuaranteeSupport& value) {
  write_enum(writer, value.kind);
  write_enum(writer, value.support);
  write_enum(writer, value.reason);
}

Status decode(ByteReader& reader, GuaranteeSupport& value) {
  GuaranteeSupport candidate;
  candidate.kind = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.support = read_enum<SupportTri>(reader, SupportTri::Indeterminate);
  candidate.reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const AllocationWork& value) {
  writer.u64(value.nodes_visited);
  writer.u64(value.nodes_pruned);
  writer.u64(value.budget);
  writer.boolean(value.budget_exhausted);
}

Status decode(ByteReader& reader, AllocationWork& value) {
  AllocationWork candidate;
  candidate.nodes_visited = reader.u64();
  candidate.nodes_pruned = reader.u64();
  candidate.budget = reader.u64();
  candidate.budget_exhausted = reader.boolean();
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const AllocationEntry& value) {
  write_id(writer, value.contract);
  write_id(writer, value.contract_generation);
  write_id(writer, value.scope);
  writer.boolean(value.admitted);
  writer.boolean(value.full);
  encode(writer, value.approved);
  encode(writer, value.delta);
  writer.u64(value.allocated_bandwidth_kbps);
  write_enum(writer, value.outcome);
  write_enum(writer, value.reason);
}

Status decode(ByteReader& reader, AllocationEntry& value) {
  AllocationEntry candidate;
  candidate.contract = read_strong<ContractTag>(reader);
  candidate.contract_generation = read_strong<ContractGenerationTag>(reader);
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.admitted = reader.boolean();
  candidate.full = reader.boolean();
  Status status = decode(reader, candidate.approved);
  if (!status.ok()) return status;
  status = decode(reader, candidate.delta);
  if (!status.ok()) return status;
  candidate.allocated_bandwidth_kbps = reader.u64();
  candidate.outcome = read_enum<DecisionOutcome>(reader, DecisionOutcome::Invalid);
  candidate.reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  if (!reader.ok()) return Status(reader.error());
  if (candidate.allocated_bandwidth_kbps > kMaxBandwidthKbps) {
    return bad("decoded allocation is outside the bandwidth domain");
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const AllocationPlan& value) {
  write_id(writer, value.id);
  encode(writer, value.authority);
  write_id(writer, value.scope);
  write_id(writer, value.capacity_generation);
  write_enum(writer, value.status);
  writer.u32(static_cast<std::uint32_t>(value.entries.size()));
  for (const AllocationEntry& entry : value.entries) encode(writer, entry);
  writer.u64(value.available_bandwidth_kbps);
  writer.u64(value.allocated_bandwidth_kbps);
  encode(writer, value.work);
  writer.boolean(value.certificate.valid);
  writer.u32(value.certificate.required_protected);
  writer.u32(value.certificate.available_protected);
  writer.u64(value.certificate.minimum_demand_kbps);
  writer.u64(value.certificate.offered_capacity_kbps);
  writer.u64(value.planned_tick.value);
  writer.boolean(value.protected_shortfall);
}

Status decode(ByteReader& reader, AllocationPlan& value) {
  AllocationPlan candidate;
  candidate.id = read_strong<PlanTag>(reader);
  Status status = decode(reader, candidate.authority);
  if (!status.ok()) return status;
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.capacity_generation = read_strong<CapacityGenerationTag>(reader);
  candidate.status = read_enum<PlanStatus>(reader, PlanStatus::Invalid);
  const std::uint32_t count = reader.count(kMaxPlanEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < count; ++i) {
    AllocationEntry entry;
    status = decode(reader, entry);
    if (!status.ok()) return status;
    candidate.entries.push_back(entry);
  }
  candidate.available_bandwidth_kbps = reader.u64();
  candidate.allocated_bandwidth_kbps = reader.u64();
  status = decode(reader, candidate.work);
  if (!status.ok()) return status;
  candidate.certificate.valid = reader.boolean();
  candidate.certificate.required_protected = reader.u32();
  candidate.certificate.available_protected = reader.u32();
  candidate.certificate.minimum_demand_kbps = reader.u64();
  candidate.certificate.offered_capacity_kbps = reader.u64();
  candidate.planned_tick.value = reader.u64();
  candidate.protected_shortfall = reader.boolean();
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const Decision& value) {
  write_id(writer, value.id);
  encode(writer, value.binding);
  write_enum(writer, value.service_class);
  writer.boolean(value.protected_obligation);
  writer.u32(value.priority);
  write_enum(writer, value.outcome);
  write_enum(writer, value.max_authority);
  encode(writer, value.original);
  encode(writer, value.approved);
  encode(writer, value.delta);
  write_enum(writer, value.primary_reason);
  writer.u32(static_cast<std::uint32_t>(value.reasons.size()));
  for (const ReasonCode reason : value.reasons) write_enum(writer, reason);
  writer.u32(static_cast<std::uint32_t>(value.support.size()));
  for (const GuaranteeSupport& support : value.support) encode(writer, support);
  encode(writer, value.evidence);
  write_enum(writer, value.evidence_state);
  write_enum(writer, value.plan_status);
  writer.boolean(value.escalation_required);
  write_enum(writer, value.provenance);
  writer.u64(value.decided_tick.value);
  writer.u64(value.expires_tick.value);
}

Status decode(ByteReader& reader, Decision& value) {
  Decision candidate;
  candidate.id = read_strong<DecisionTag>(reader);
  Status status = decode(reader, candidate.binding);
  if (!status.ok()) return status;
  candidate.service_class = read_enum<ServiceClass>(reader, ServiceClass::Standard);
  candidate.protected_obligation = reader.boolean();
  candidate.priority = reader.u32();
  candidate.outcome = read_enum<DecisionOutcome>(reader, DecisionOutcome::Invalid);
  candidate.max_authority = read_enum<AuthorityLevel>(reader, AuthorityLevel::None);
  status = decode(reader, candidate.original);
  if (!status.ok()) return status;
  status = decode(reader, candidate.approved);
  if (!status.ok()) return status;
  status = decode(reader, candidate.delta);
  if (!status.ok()) return status;
  candidate.primary_reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  const std::uint32_t reasons = reader.count(kMaxReasonsPerDecision);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < reasons; ++i) {
    candidate.reasons.push_back(read_enum<ReasonCode>(reader, ReasonCode::None));
  }
  const std::uint32_t support = reader.count(kMaxSupportEntries);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < support; ++i) {
    GuaranteeSupport entry;
    status = decode(reader, entry);
    if (!status.ok()) return status;
    candidate.support.push_back(entry);
  }
  status = decode(reader, candidate.evidence);
  if (!status.ok()) return status;
  candidate.evidence_state = read_enum<EvidenceState>(reader, EvidenceState::Unknown);
  candidate.plan_status = read_enum<PlanStatus>(reader, PlanStatus::Invalid);
  candidate.escalation_required = reader.boolean();
  candidate.provenance = read_enum<ProvenanceClass>(reader, ProvenanceClass::Synthetic);
  candidate.decided_tick.value = reader.u64();
  candidate.expires_tick.value = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Grant, fence and restoration
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const RestorationPrecondition& value) {
  write_enum(writer, value.kind);
  write_enum(writer, value.guarantee);
  write_id(writer, value.scope);
  writer.u64(value.required_value);
  writer.u64(value.dwell_ticks);
}

Status decode(ByteReader& reader, RestorationPrecondition& value) {
  RestorationPrecondition candidate;
  candidate.kind = read_enum<PreconditionKind>(reader, PreconditionKind::EvidenceKnown);
  candidate.guarantee = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  candidate.scope = read_strong<ScopeTag>(reader);
  candidate.required_value = reader.u64();
  candidate.dwell_ticks = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  if (candidate.required_value > max_value_for(candidate.guarantee)) {
    return bad("decoded precondition is outside the guarantee domain");
  }
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const PreconditionEvaluation& value) {
  encode(writer, value.precondition);
  write_enum(writer, value.result);
  write_enum(writer, value.reason);
}

Status decode(ByteReader& reader, PreconditionEvaluation& value) {
  PreconditionEvaluation candidate;
  Status status = decode(reader, candidate.precondition);
  if (!status.ok()) return status;
  candidate.result = read_enum<PreconditionResult>(reader, PreconditionResult::Indeterminate);
  candidate.reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const RestorationEvaluation& value) {
  write_enum(writer, value.outcome);
  write_id(writer, value.grant);
  write_id(writer, value.decision);
  encode(writer, value.binding);
  encode(writer, value.original);
  encode(writer, value.degraded);
  writer.u32(static_cast<std::uint32_t>(value.preconditions.size()));
  for (const PreconditionEvaluation& evaluation : value.preconditions) encode(writer, evaluation);
  write_enum(writer, value.primary_reason);
  write_enum(writer, value.evidence_state);
  write_enum(writer, value.provenance);
  writer.u64(value.evaluated_tick.value);
}

Status decode(ByteReader& reader, RestorationEvaluation& value) {
  RestorationEvaluation candidate;
  candidate.outcome = read_enum<RestorationOutcome>(reader, RestorationOutcome::Invalid);
  candidate.grant = read_strong<GrantTag>(reader);
  candidate.decision = read_strong<DecisionTag>(reader);
  Status status = decode(reader, candidate.binding);
  if (!status.ok()) return status;
  status = decode(reader, candidate.original);
  if (!status.ok()) return status;
  status = decode(reader, candidate.degraded);
  if (!status.ok()) return status;
  const std::uint32_t count = reader.count(kMaxPreconditionEvaluations);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < count; ++i) {
    PreconditionEvaluation evaluation;
    status = decode(reader, evaluation);
    if (!status.ok()) return status;
    candidate.preconditions.push_back(evaluation);
  }
  candidate.primary_reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  candidate.evidence_state = read_enum<EvidenceState>(reader, EvidenceState::Unknown);
  candidate.provenance = read_enum<ProvenanceClass>(reader, ProvenanceClass::Synthetic);
  candidate.evaluated_tick.value = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const Grant& value) {
  write_id(writer, value.id);
  write_id(writer, value.decision);
  encode(writer, value.binding);
  write_enum(writer, value.service_class);
  writer.boolean(value.protected_obligation);
  encode(writer, value.original);
  encode(writer, value.degraded);
  encode(writer, value.delta);
  write_enum(writer, value.reason);
  encode(writer, value.evidence);
  write_enum(writer, value.evidence_state);
  writer.u32(static_cast<std::uint32_t>(value.preconditions.size()));
  for (const RestorationPrecondition& precondition : value.preconditions) {
    encode(writer, precondition);
  }
  write_id(writer, value.sequence);
  write_id(writer, value.last_attempt);
  writer.u64(value.issued_tick.value);
  writer.u64(value.expires_tick.value);
  writer.u64(value.last_transition_tick.value);
  write_enum(writer, value.state);
  write_enum(writer, value.provenance);
}

Status decode(ByteReader& reader, Grant& value) {
  Grant candidate;
  candidate.id = read_strong<GrantTag>(reader);
  candidate.decision = read_strong<DecisionTag>(reader);
  Status status = decode(reader, candidate.binding);
  if (!status.ok()) return status;
  candidate.service_class = read_enum<ServiceClass>(reader, ServiceClass::Standard);
  candidate.protected_obligation = reader.boolean();
  status = decode(reader, candidate.original);
  if (!status.ok()) return status;
  status = decode(reader, candidate.degraded);
  if (!status.ok()) return status;
  status = decode(reader, candidate.delta);
  if (!status.ok()) return status;
  candidate.reason = read_enum<ReasonCode>(reader, ReasonCode::None);
  status = decode(reader, candidate.evidence);
  if (!status.ok()) return status;
  candidate.evidence_state = read_enum<EvidenceState>(reader, EvidenceState::Unknown);
  const std::uint32_t count = reader.count(kMaxPreconditions);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < count; ++i) {
    RestorationPrecondition precondition;
    status = decode(reader, precondition);
    if (!status.ok()) return status;
    candidate.preconditions.push_back(precondition);
  }
  candidate.sequence = read_strong<SequenceTag>(reader);
  candidate.last_attempt = read_strong<AttemptTag>(reader);
  candidate.issued_tick.value = reader.u64();
  candidate.expires_tick.value = reader.u64();
  candidate.last_transition_tick.value = reader.u64();
  candidate.state = read_enum<GrantState>(reader, GrantState::Issued);
  candidate.provenance = read_enum<ProvenanceClass>(reader, ProvenanceClass::Synthetic);
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const FenceRecord& value) {
  write_id(writer, value.id);
  write_id(writer, value.grant);
  write_enum(writer, value.reason);
  writer.u32(static_cast<std::uint32_t>(value.delta));
  encode(writer, value.prior);
  encode(writer, value.current);
  writer.u64(value.fenced_tick.value);
}

Status decode(ByteReader& reader, FenceRecord& value) {
  FenceRecord candidate;
  candidate.id = read_strong<FenceTag>(reader);
  candidate.grant = read_strong<GrantTag>(reader);
  candidate.reason = read_enum<FenceReason>(reader, FenceReason::Manual);
  const std::uint32_t delta = reader.u32();
  if (!reader.ok()) return Status(reader.error());
  const std::uint32_t known_flags =
      static_cast<std::uint32_t>(AuthorityDelta::CoordinatorTerm) |
      static_cast<std::uint32_t>(AuthorityDelta::Boot) |
      static_cast<std::uint32_t>(AuthorityDelta::Fabric) |
      static_cast<std::uint32_t>(AuthorityDelta::Capacity) |
      static_cast<std::uint32_t>(AuthorityDelta::Policy) |
      static_cast<std::uint32_t>(AuthorityDelta::Evidence) |
      static_cast<std::uint32_t>(AuthorityDelta::Contract) |
      static_cast<std::uint32_t>(AuthorityDelta::ContractGeneration) |
      static_cast<std::uint32_t>(AuthorityDelta::Subject) |
      static_cast<std::uint32_t>(AuthorityDelta::SubjectGeneration) |
      static_cast<std::uint32_t>(AuthorityDelta::Scope);
  if ((delta & ~known_flags) != 0U) return bad("decoded fence delta has unknown flags");
  candidate.delta = static_cast<AuthorityDelta>(delta);
  Status status = decode(reader, candidate.prior);
  if (!status.ok()) return status;
  status = decode(reader, candidate.current);
  if (!status.ok()) return status;
  candidate.fenced_tick.value = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, const PolicyRule& value) {
  write_id(writer, value.id);
  writer.u32(value.precedence);
  writer.boolean(value.service_class.has_value());
  if (value.service_class.has_value()) write_enum(writer, *value.service_class);
  writer.boolean(value.scope.has_value());
  if (value.scope.has_value()) write_id(writer, *value.scope);
  writer.boolean(value.trigger.has_value());
  if (value.trigger.has_value()) write_enum(writer, *value.trigger);
  write_enum(writer, value.action);
}

Status decode(ByteReader& reader, PolicyRule& value) {
  PolicyRule candidate;
  candidate.id = read_strong<RuleTag>(reader);
  candidate.precedence = reader.u32();
  if (reader.boolean()) {
    const auto parsed = read_enum<ServiceClass>(reader, ServiceClass::Standard);
    candidate.service_class = parsed;
  }
  if (!reader.ok()) return Status(reader.error());
  if (reader.boolean()) {
    candidate.scope = read_strong<ScopeTag>(reader);
  }
  if (!reader.ok()) return Status(reader.error());
  if (reader.boolean()) {
    candidate.trigger = read_enum<GuaranteeKind>(reader, GuaranteeKind::Availability);
  }
  candidate.action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  if (!reader.ok()) return Status(reader.error());
  Status status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const ClassProfile& value) {
  write_enum(writer, value.service_class);
  encode(writer, value.ceiling);
  writer.boolean(value.may_be_degraded);
}

Status decode(ByteReader& reader, ClassProfile& value) {
  ClassProfile candidate;
  candidate.service_class = read_enum<ServiceClass>(reader, ServiceClass::Standard);
  Status status = decode(reader, candidate.ceiling);
  if (!status.ok()) return status;
  candidate.may_be_degraded = reader.boolean();
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const Policy& value) {
  write_id(writer, value.id);
  write_id(writer, value.generation);
  writer.u32(static_cast<std::uint32_t>(value.rules.size()));
  for (const PolicyRule& rule : value.rules) encode(writer, rule);
  writer.u32(static_cast<std::uint32_t>(value.class_profiles.size()));
  for (const ClassProfile& profile : value.class_profiles) encode(writer, profile);
  write_enum(writer, value.unknown_evidence_action);
  write_enum(writer, value.stale_evidence_action);
  write_enum(writer, value.conflict_evidence_action);
  write_enum(writer, value.invalid_evidence_action);
  write_enum(writer, value.unsupported_evidence_action);
  writer.boolean(value.protect_first);
  writer.u64(value.default_ttl_ticks.value);
  writer.u64(value.minimum_ttl_ticks.value);
  writer.u32(value.max_concessions_per_contract);
  writer.u32(value.minimum_dwell_ticks);
  writer.u32(value.minimum_protected_served);
  writer.u64(value.search_node_budget);
  writer.u64(value.evidence_freshness_ticks);
}

Status decode(ByteReader& reader, Policy& value) {
  Policy candidate;
  candidate.id = read_strong<PolicyTag>(reader);
  candidate.generation = read_strong<PolicyGenerationTag>(reader);
  const std::uint32_t rules = reader.count(kMaxRulesPerPolicy);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < rules; ++i) {
    PolicyRule rule;
    Status status = decode(reader, rule);
    if (!status.ok()) return status;
    candidate.rules.push_back(rule);
  }
  const std::uint32_t profiles = reader.count(kMaxClassProfiles);
  if (!reader.ok()) return Status(reader.error());
  for (std::uint32_t i = 0; i < profiles; ++i) {
    ClassProfile profile;
    Status status = decode(reader, profile);
    if (!status.ok()) return status;
    candidate.class_profiles.push_back(profile);
  }
  candidate.unknown_evidence_action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  candidate.stale_evidence_action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  candidate.conflict_evidence_action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  candidate.invalid_evidence_action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  candidate.unsupported_evidence_action = read_enum<DegradeAction>(reader, DegradeAction::Refuse);
  candidate.protect_first = reader.boolean();
  candidate.default_ttl_ticks.value = reader.u64();
  candidate.minimum_ttl_ticks.value = reader.u64();
  candidate.max_concessions_per_contract = reader.u32();
  candidate.minimum_dwell_ticks = reader.u32();
  candidate.minimum_protected_served = reader.u32();
  candidate.search_node_budget = reader.u64();
  candidate.evidence_freshness_ticks = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  // An all-zero policy is the legal representation of "no policy has been
  // installed"; anything else must validate.
  if (candidate.installed()) {
    const Status status = candidate.validate();
    if (!status.ok()) return status;
  }
  value = candidate;
  return Status{};
}

}  // namespace dmf
