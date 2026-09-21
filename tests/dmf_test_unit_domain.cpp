// Degraded Mode Fabric - unit and codec suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <limits>
#include <set>
#include <vector>

#include "dmf/codec.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

// ---------------------------------------------------------------------------
// Guarantee algebra
// ---------------------------------------------------------------------------

DMF_TEST(guarantee, weakening_relation_is_exact) {
  GuaranteeSet original;
  DMF_CHECK_OK(original.insert(GuaranteeKind::Bandwidth, 1000));
  DMF_CHECK_OK(original.insert(GuaranteeKind::LatencyP99, 100));

  GuaranteeSet weaker = original;
  DMF_CHECK_OK(weaker.upsert(GuaranteeKind::Bandwidth, 400));
  DMF_CHECK_EQ(classify_weakening(original, weaker), WeakeningResult::WeakensOrEquals);
  DMF_CHECK(classify_weakening(original, original) == WeakeningResult::WeakensOrEquals);

  GuaranteeSet stronger = original;
  DMF_CHECK_OK(stronger.upsert(GuaranteeKind::Bandwidth, 1001));
  DMF_CHECK_EQ(classify_weakening(original, stronger), WeakeningResult::Strengthens);

  GuaranteeSet tighter_latency = original;
  DMF_CHECK_OK(tighter_latency.upsert(GuaranteeKind::LatencyP99, 99));
  DMF_CHECK_EQ(classify_weakening(original, tighter_latency), WeakeningResult::Strengthens);

  GuaranteeSet extra = original;
  DMF_CHECK_OK(extra.insert(GuaranteeKind::PathDiversity, 2));
  DMF_CHECK_EQ(classify_weakening(original, extra), WeakeningResult::AddsGuarantee);
}

DMF_TEST(guarantee, soft_to_hard_is_a_strengthening) {
  GuaranteeSet original;
  DMF_CHECK_OK(original.insert(GuaranteeKind::Bandwidth, 1000, /*hard=*/false));
  GuaranteeSet candidate;
  DMF_CHECK_OK(candidate.insert(GuaranteeKind::Bandwidth, 1000, /*hard=*/true));
  DMF_CHECK_EQ(classify_weakening(original, candidate), WeakeningResult::Strengthens);
}

DMF_TEST(guarantee, delta_records_every_concession_and_withdrawal) {
  GuaranteeSet original;
  DMF_CHECK_OK(original.insert(GuaranteeKind::Bandwidth, 1000));
  DMF_CHECK_OK(original.insert(GuaranteeKind::LatencyP99, 100));
  DMF_CHECK_OK(original.insert(GuaranteeKind::Reachability, 990000));

  GuaranteeSet approved;
  DMF_CHECK_OK(approved.insert(GuaranteeKind::Bandwidth, 250));
  DMF_CHECK_OK(approved.insert(GuaranteeKind::LatencyP99, 400));

  auto delta = compute_delta(original, approved, ReasonCode::ConcessionWithinEnvelope);
  DMF_CHECK(delta.ok());
  DMF_CHECK_EQ(delta.value().concessions.size(), std::size_t{2});
  DMF_CHECK_EQ(delta.value().withdrawals.size(), std::size_t{1});
  DMF_CHECK_EQ(delta.value().withdrawals[0].kind, GuaranteeKind::Reachability);
}

DMF_TEST(guarantee, delta_refuses_a_non_weakening) {
  GuaranteeSet original;
  DMF_CHECK_OK(original.insert(GuaranteeKind::Bandwidth, 100));
  GuaranteeSet approved;
  DMF_CHECK_OK(approved.insert(GuaranteeKind::Bandwidth, 200));
  auto delta = compute_delta(original, approved, ReasonCode::None);
  DMF_CHECK(!delta.ok());
}

DMF_TEST(guarantee, values_are_bounded_per_kind) {
  Guarantee too_fast;
  too_fast.kind = GuaranteeKind::LatencyP99;
  too_fast.comparator = Comparator::AtMost;
  too_fast.value = max_value_for(GuaranteeKind::LatencyP99) + 1;
  DMF_CHECK(!too_fast.valid());

  Guarantee wrong_comparator;
  wrong_comparator.kind = GuaranteeKind::Bandwidth;
  wrong_comparator.comparator = Comparator::AtMost;
  wrong_comparator.value = 10;
  DMF_CHECK(!wrong_comparator.valid());

  GuaranteeSet set;
  DMF_CHECK(!set.insert(wrong_comparator).ok());
}

DMF_TEST(guarantee, envelope_composition_takes_the_stricter_bound) {
  GuaranteeEnvelope contract_envelope;
  DMF_CHECK_OK(contract_envelope.insert(ConcessionBound{GuaranteeKind::Bandwidth, 1000}));
  contract_envelope.set_max_concessions(4);
  GuaranteeEnvelope ceiling;
  DMF_CHECK_OK(ceiling.insert(ConcessionBound{GuaranteeKind::Bandwidth, 2000}));
  ceiling.set_max_concessions(2);
  auto composed = compose_envelopes(contract_envelope, ceiling, 3);
  DMF_CHECK(composed.ok());
  const ConcessionBound* bound = composed.value().find(GuaranteeKind::Bandwidth);
  DMF_CHECK(bound != nullptr);
  DMF_CHECK_EQ(bound->weakest_allowed_value, std::uint64_t{1000});
  DMF_CHECK_EQ(composed.value().max_concessions(), std::uint32_t{2});
}

// ---------------------------------------------------------------------------
// Contracts and policy
// ---------------------------------------------------------------------------

DMF_TEST(contract, rigid_contracts_may_not_carry_a_concession_envelope) {
  // A concession envelope and a non-degradable obligation are contradictory: a
  // contract that may not be weakened may not authorise a weakening.
  ServiceContract contradictory =
      contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  DMF_CHECK_OK(contradictory.validate());
  contradictory.non_degradable = true;
  DMF_CHECK(!contradictory.validate().ok());

  // Marking it protected and dropping the envelope makes it legal.
  contradictory.envelope = GuaranteeEnvelope{};
  DMF_CHECK_OK(contradictory.validate());
  DMF_CHECK(contradictory.protected_obligation());
  DMF_CHECK(contradictory.frozen());

  // The protected service class is rigid even without the explicit marker.
  ServiceContract by_class = contract(2, ServiceClass::Protected, 10, 1000, 100, 0, 0);
  DMF_CHECK_OK(by_class.validate());
  DMF_CHECK(by_class.protected_obligation());
}

DMF_TEST(contract, an_envelope_never_strengthens_and_never_names_an_absent_guarantee) {
  ServiceContract value = contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  DMF_CHECK_OK(value.validate());
  // Tightening the floor is accepted; loosening it is not, because the stricter
  // of the two bounds always wins.
  DMF_CHECK_OK(value.envelope.upsert(ConcessionBound{GuaranteeKind::Bandwidth, 400}));
  DMF_CHECK_EQ(value.envelope.find(GuaranteeKind::Bandwidth)->weakest_allowed_value,
               std::uint64_t{400});
  DMF_CHECK_OK(value.envelope.upsert(ConcessionBound{GuaranteeKind::Bandwidth, 5000}));
  DMF_CHECK_EQ(value.envelope.find(GuaranteeKind::Bandwidth)->weakest_allowed_value,
               std::uint64_t{400});
  DMF_CHECK_OK(value.validate());

  // A bound for a guarantee the contract does not carry is a configuration
  // error, not a harmless extra.
  ServiceContract absent = contract(2, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  DMF_CHECK_OK(absent.envelope.insert(ConcessionBound{GuaranteeKind::Durability, 1}));
  DMF_CHECK(!absent.validate().ok());
}

DMF_TEST(policy, rule_selection_is_by_precedence_and_total) {
  const Policy policy = demonstration_policy();
  DMF_CHECK_OK(policy.validate());
  const PolicyRule* protected_rule =
      policy.select_rule(ServiceClass::Protected, scope_id(1), GuaranteeKind::Bandwidth);
  DMF_CHECK(protected_rule != nullptr);
  DMF_CHECK_EQ(protected_rule->action, DegradeAction::Protect);
  const PolicyRule* standard_rule =
      policy.select_rule(ServiceClass::Standard, scope_id(1), GuaranteeKind::Bandwidth);
  DMF_CHECK(standard_rule != nullptr);
  DMF_CHECK_EQ(standard_rule->action, DegradeAction::Degrade);
}

DMF_TEST(policy, duplicate_precedence_is_rejected) {
  Policy policy = demonstration_policy();
  PolicyRule duplicate;
  duplicate.id = RuleId::from_value(9);
  duplicate.precedence = 10;
  duplicate.action = DegradeAction::Refuse;
  policy.rules.push_back(duplicate);
  DMF_CHECK(!policy.validate().ok());
}

DMF_TEST(policy, evidence_actions_default_to_refusal) {
  Policy policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = PolicyGeneration::from_value(1);
  DMF_CHECK_EQ(policy.evidence_action(EvidenceState::Unknown), DegradeAction::Refuse);
  DMF_CHECK_EQ(policy.evidence_action(EvidenceState::Stale), DegradeAction::Refuse);
  DMF_CHECK_EQ(policy.evidence_action(EvidenceState::Conflict), DegradeAction::Refuse);
  DMF_CHECK_EQ(policy.evidence_action(EvidenceState::Invalid), DegradeAction::Refuse);
}

DMF_TEST(policy, protected_class_cannot_be_degradable) {
  Policy policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = PolicyGeneration::from_value(1);
  ClassProfile profile;
  profile.service_class = ServiceClass::Protected;
  profile.may_be_degraded = true;
  policy.class_profiles.push_back(profile);
  DMF_CHECK(!policy.validate().ok());
}

// ---------------------------------------------------------------------------
// Evidence aggregation
// ---------------------------------------------------------------------------

DMF_TEST(evidence, absent_evidence_is_unknown_not_healthy) {
  EvidenceVector vector;
  DMF_CHECK_EQ(vector.aggregate_state(), EvidenceState::Unknown);
  DMF_CHECK_EQ(vector.state_for(scope_id(1), EvidenceKind::AggregateBandwidth),
               EvidenceState::Unknown);
}

DMF_TEST(evidence, aggregation_picks_the_worst_state) {
  EvidenceVector vector;
  DMF_CHECK_OK(vector.push(evidence_item(1, 1, EvidenceKind::AggregateBandwidth, 100, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(2, 1, EvidenceKind::RoundTripLatency, 10, 1, Tick{1},
                                         BootIncarnation::from_value(1), EvidenceState::Stale)));
  DMF_CHECK_OK(vector.normalize());
  DMF_CHECK_EQ(vector.state_for(scope_id(1), EvidenceKind::AggregateBandwidth),
               EvidenceState::Known);
  DMF_CHECK_EQ(vector.state_for(scope_id(1), EvidenceKind::RoundTripLatency), EvidenceState::Stale);
  DMF_CHECK_EQ(vector.aggregate_state(), EvidenceState::Stale);
}

DMF_TEST(evidence, contradictory_publishers_produce_a_conflict) {
  EvidenceVector vector;
  DMF_CHECK_OK(vector.push(evidence_item(1, 1, EvidenceKind::AggregateBandwidth, 100, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(2, 2, EvidenceKind::AggregateBandwidth, 200, 1, Tick{1})));
  DMF_CHECK_OK(vector.normalize());
  DMF_CHECK_EQ(vector.state_for(scope_id(1), EvidenceKind::AggregateBandwidth),
               EvidenceState::Conflict);
  DMF_CHECK(!vector.value_for(scope_id(1), EvidenceKind::AggregateBandwidth).has_value());
}

DMF_TEST(evidence, a_newer_generation_supersedes_an_older_one) {
  EvidenceVector vector;
  DMF_CHECK_OK(vector.push(evidence_item(1, 1, EvidenceKind::AggregateBandwidth, 100, 1, Tick{1},
                                         BootIncarnation::from_value(1), EvidenceState::Known,
                                         EvidenceGeneration::from_value(1))));
  DMF_CHECK_OK(vector.push(evidence_item(2, 1, EvidenceKind::AggregateBandwidth, 200, 1, Tick{2},
                                         BootIncarnation::from_value(1), EvidenceState::Known,
                                         EvidenceGeneration::from_value(2))));
  DMF_CHECK_OK(vector.normalize());
  DMF_CHECK_EQ(vector.size(), std::size_t{1});
  DMF_CHECK_EQ(vector.value_for(scope_id(1), EvidenceKind::AggregateBandwidth).value_or(0),
               std::uint64_t{200});
}

DMF_TEST(evidence, capability_derivation_fails_closed) {
  EvidenceVector vector;
  DMF_CHECK_OK(vector.push(evidence_item(1, 1, EvidenceKind::AggregateBandwidth, 100, 1, Tick{1})));
  DMF_CHECK_OK(vector.normalize());
  FreshnessWindow window;
  window.now = Tick{1};
  window.budget_ticks = 10;
  auto snapshot = derive_capability(vector, scope_id(1), CapacityGeneration::from_value(1), window);
  DMF_CHECK(snapshot.ok());
  DMF_CHECK(!snapshot.value().usable());
  DMF_CHECK_EQ(snapshot.value().state, EvidenceState::Unknown);

  Guarantee bandwidth;
  bandwidth.kind = GuaranteeKind::Bandwidth;
  bandwidth.comparator = Comparator::AtLeast;
  bandwidth.value = 1;
  DMF_CHECK_EQ(supports(snapshot.value(), bandwidth), SupportTri::Indeterminate);
}

DMF_TEST(evidence, stale_evidence_is_not_current) {
  CapabilitySpec spec;
  spec.observed_tick = Tick{1};
  const CapabilitySnapshot snapshot = capability(spec);
  EvidenceVector vector;
  DMF_CHECK_OK(vector.push(evidence_item(1, 1, EvidenceKind::FabricTopology, 1, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(2, 1, EvidenceKind::AggregateBandwidth, 1000, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(3, 1, EvidenceKind::RoundTripLatency, 10, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(4, 1, EvidenceKind::PathDiversity, 2, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(5, 1, EvidenceKind::Reachability, 1000000, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(6, 1, EvidenceKind::NodeCoverage, 1000000, 1, Tick{1})));
  DMF_CHECK_OK(vector.push(evidence_item(7, 1, EvidenceKind::SynchronousDurability, 1, 1, Tick{1})));
  DMF_CHECK_OK(vector.normalize());
  FreshnessWindow window;
  window.now = Tick{1000};
  window.budget_ticks = 10;
  auto fresh = derive_capability(vector, scope_id(1), CapacityGeneration::from_value(1), window);
  DMF_CHECK(fresh.ok());
  DMF_CHECK_EQ(fresh.value().state, EvidenceState::Stale);
  window.enforced = false;
  auto unfresh = derive_capability(vector, scope_id(1), CapacityGeneration::from_value(1), window);
  DMF_CHECK(unfresh.ok());
  DMF_CHECK_EQ(unfresh.value().state, EvidenceState::Known);
  (void)snapshot;
}

// ---------------------------------------------------------------------------
// Authority bindings
// ---------------------------------------------------------------------------

DMF_TEST(authority, identity_change_and_generation_change_are_different_deltas) {
  const ServiceContract base = contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  AuthorityBinding first = binding_for(base, CapacityGeneration::from_value(1));
  AuthorityBinding second = first;
  second.contract_generation = ContractGeneration::from_value(2);
  const AuthorityDelta generation_delta = first.classify(second);
  DMF_CHECK(has_flag(generation_delta, AuthorityDelta::ContractGeneration));
  DMF_CHECK(!has_flag(generation_delta, AuthorityDelta::Contract));

  AuthorityBinding third = first;
  third.contract = contract_id(99);
  const AuthorityDelta identity_delta = first.classify(third);
  DMF_CHECK(has_flag(identity_delta, AuthorityDelta::Contract));
  DMF_CHECK(!has_flag(identity_delta, AuthorityDelta::ContractGeneration));
}

DMF_TEST(authority, fence_reason_precedence_is_deterministic) {
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::Boot), FenceReason::BootAdvance);
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::CoordinatorTerm),
               FenceReason::CoordinatorTermAdvance);
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::Fabric), FenceReason::TopologyChange);
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::Capacity), FenceReason::CapacityChange);
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::Policy), FenceReason::PolicyChange);
  DMF_CHECK_EQ(fence_reason_for(AuthorityDelta::ContractGeneration), FenceReason::ContractChange);
}

DMF_TEST(authority, the_ladder_separates_recommendation_from_authority) {
  DMF_CHECK(!is_authority(AuthorityLevel::Observed));
  DMF_CHECK(!is_authority(AuthorityLevel::Eligible));
  DMF_CHECK(!is_authority(AuthorityLevel::Recommended));
  DMF_CHECK(is_authority(AuthorityLevel::Authorized));
  DMF_CHECK(is_authority(AuthorityLevel::Acknowledged));
  DMF_CHECK(is_authority(AuthorityLevel::Applied));
  DMF_CHECK(!is_verified_effect(AuthorityLevel::Acknowledged));
  DMF_CHECK(is_verified_effect(AuthorityLevel::Applied));
}

DMF_TEST(authority, grant_transitions_are_strictly_forward) {
  DMF_CHECK(grant_transition_allowed(GrantState::Issued, GrantState::Acknowledged));
  DMF_CHECK(grant_transition_allowed(GrantState::Acknowledged, GrantState::Applied));
  DMF_CHECK(grant_transition_allowed(GrantState::Applied, GrantState::Fenced));
  DMF_CHECK(!grant_transition_allowed(GrantState::Applied, GrantState::Issued));
  DMF_CHECK(!grant_transition_allowed(GrantState::Fenced, GrantState::Applied));
  DMF_CHECK(!grant_transition_allowed(GrantState::Issued, GrantState::Issued));
  DMF_CHECK(grant_state_terminal(GrantState::Revoked));
  DMF_CHECK(grant_state_live(GrantState::Applied));
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

DMF_TEST(codec, every_type_round_trips_canonically) {
  const Policy policy = demonstration_policy();
  ByteWriter policy_writer;
  encode(policy_writer, policy);
  Policy decoded_policy;
  ByteReader policy_reader(policy_writer.bytes());
  DMF_CHECK_OK(decode(policy_reader, decoded_policy));
  DMF_CHECK_OK(decode_finish(policy_reader));
  DMF_CHECK_EQ(decoded_policy.digest(), policy.digest());

  const ServiceContract entry = contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  ByteWriter contract_writer;
  encode(contract_writer, entry);
  ServiceContract decoded_contract;
  ByteReader contract_reader(contract_writer.bytes());
  DMF_CHECK_OK(decode(contract_reader, decoded_contract));
  DMF_CHECK_EQ(decoded_contract.digest(), entry.digest());

  EvidenceVector evidence;
  DMF_CHECK_OK(evidence.push(evidence_item(1, 1, EvidenceKind::AggregateBandwidth, 100, 1, Tick{1})));
  DMF_CHECK_OK(evidence.normalize());
  ByteWriter evidence_writer;
  encode(evidence_writer, evidence);
  EvidenceVector decoded_evidence;
  ByteReader evidence_reader(evidence_writer.bytes());
  DMF_CHECK_OK(decode(evidence_reader, decoded_evidence));
  DMF_CHECK_EQ(decoded_evidence.digest(), evidence.digest());

  const CapabilitySnapshot snapshot = capability(CapabilitySpec{});
  ByteWriter capability_writer;
  encode(capability_writer, snapshot);
  CapabilitySnapshot decoded_snapshot;
  ByteReader capability_reader(capability_writer.bytes());
  DMF_CHECK_OK(decode(capability_reader, decoded_snapshot));
  DMF_CHECK_EQ(decoded_snapshot.digest(), snapshot.digest());
}

DMF_TEST(codec, truncated_inputs_are_rejected_at_every_length) {
  const Policy policy = demonstration_policy();
  ByteWriter writer;
  encode(writer, policy);
  const std::vector<std::uint8_t> bytes = writer.bytes();
  DMF_CHECK(bytes.size() > 8);
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    ByteReader reader(bytes.data(), length);
    Policy candidate;
    const Status status = decode(reader, candidate);
    if (status.ok() && reader.at_end()) {
      // Only a genuinely complete prefix may decode; nothing else is acceptable.
      DMF_CHECK_EQ(length, bytes.size());
    }
  }
}

DMF_TEST(codec, invalid_enumerations_are_rejected) {
  ByteWriter writer;
  writer.u64(1);
  writer.u64(1);
  writer.u64(1);
  writer.u64(0xFFFF);  // invalid service class
  writer.boolean(false);
  writer.u32(0);
  writer.u32(1);  // guarantee count
  writer.u16(0xFFFF);  // invalid guarantee kind
  writer.u16(1);
  writer.u64(0);
  writer.boolean(true);
  writer.u32(0);  // envelope bound count
  writer.u32(0);
  writer.u64(0);
  writer.boolean(true);
  ByteReader reader(writer.bytes());
  ServiceContract decoded;
  const Status status = decode(reader, decoded);
  DMF_CHECK(!status.ok());
}

DMF_TEST(codec, an_oversized_declared_collection_is_refused_before_allocation) {
  ByteWriter writer;
  writer.u32(0xFFFFFFFFU);  // guarantee count far beyond the bound
  ByteReader reader(writer.bytes());
  GuaranteeSet decoded;
  DMF_CHECK_CODE(decode(reader, decoded), ErrorCode::CapacityExceeded);
}

DMF_TEST(codec, trailing_bytes_are_rejected) {
  const Policy policy = demonstration_policy();
  ByteWriter writer;
  encode(writer, policy);
  std::vector<std::uint8_t> bytes = writer.bytes();
  bytes.push_back(0);
  ByteReader reader(bytes);
  Policy decoded;
  DMF_CHECK_OK(decode(reader, decoded));
  DMF_CHECK_CODE(decode_finish(reader), ErrorCode::TrailingGarbage);
}

DMF_TEST(codec, a_poisoned_reader_stays_poisoned) {
  const std::vector<std::uint8_t> empty;
  ByteReader reader(empty);
  DMF_CHECK_EQ(reader.u32(), 0U);
  DMF_CHECK(!reader.ok());
  DMF_CHECK_EQ(reader.error(), ErrorCode::Truncated);
  DMF_CHECK_EQ(reader.u64(), 0ULL);
  DMF_CHECK(!reader.ok());
  DMF_CHECK_EQ(reader.error(), ErrorCode::Truncated);
}

DMF_TEST(codec, checked_arithmetic_never_wraps) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  DMF_CHECK(!checked_add(maximum, std::uint64_t{1}).has_value());
  DMF_CHECK_EQ(checked_add(maximum, std::uint64_t{0}).value_or(0), maximum);
  DMF_CHECK(!checked_mul(maximum, std::uint64_t{2}).has_value());
  DMF_CHECK_EQ(checked_mul(maximum, std::uint64_t{0}).value_or(1), std::uint64_t{0});
  DMF_CHECK(!checked_sub(std::uint64_t{0}, std::uint64_t{1}).has_value());
}

DMF_TEST(accounting, counters_are_bounded_and_closure_is_exact) {
  AccountingCounters counters;
  Accounting accounting(counters);
  DMF_CHECK_OK(accounting.record_grant_issued());
  DMF_CHECK_OK(accounting.record_grant_fenced());
  DMF_CHECK_OK(accounting.record_fences(1));
  DMF_CHECK_OK(accounting.record_decision(DecisionOutcome::Degraded));
  ClosureInputs inputs;
  inputs.live_grants = 0;
  inputs.retained_terminated_grants = 1;
  inputs.retained_fences = 1;
  const ClosureReport report = accounting.check_closure(inputs);
  DMF_CHECK(report.closed);

  ClosureInputs wrong = inputs;
  wrong.live_grants = 1;
  wrong.retained_terminated_grants = 0;
  DMF_CHECK(!accounting.check_closure(wrong).closed);
}

DMF_TEST(accounting, a_read_only_facade_refuses_mutation) {
  const AccountingCounters counters;
  Accounting accounting(counters);  // constructed from a read-only block
  DMF_CHECK_CODE(accounting.record_grant_issued(), ErrorCode::InvalidState);
  DMF_CHECK(accounting.mutable_counters() == nullptr);
}
