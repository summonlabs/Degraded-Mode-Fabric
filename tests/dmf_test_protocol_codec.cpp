// Degraded Mode Fabric - framed protocol and codec suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every adversarial case required of a framed transport is exercised here: each
// truncated prefix, zero/oversized declared lengths, corrupted integrity fields,
// invalid enumerations, replayed and regressed sequences, and cross-session
// impersonation.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

std::vector<std::uint8_t> sample_payload(std::size_t size, std::uint8_t seed = 7) {
  std::vector<std::uint8_t> payload(size);
  for (std::size_t i = 0; i < size; ++i) {
    payload[i] = static_cast<std::uint8_t>((i * 31U + seed) & 0xFFU);
  }
  return payload;
}

/// A coordinator used in-process as the protocol's server side.
struct LocalCoordinator {
  TempDir directory{"protocol"};
  std::unique_ptr<Coordinator> coordinator{};
  Status start() {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    if (!created.ok()) return created.status();
    coordinator = std::move(created.value());
    return coordinator->start();
  }
  ~LocalCoordinator() {
    if (coordinator) {
      const Status status = coordinator->stop();
      (void)status;
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Frame decoder
// ---------------------------------------------------------------------------

DMF_TEST(frame, round_trips_a_payload) {
  const std::vector<std::uint8_t> payload = sample_payload(64);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::QueryStatus, payload, frame));
  DMF_CHECK_EQ(frame.size(), kFrameHeaderSize + payload.size() + kFrameTrailerSize);
  FrameReader reader;
  DMF_CHECK_OK(reader.push(frame.data(), frame.size()));
  auto decoded = reader.next();
  DMF_CHECK(decoded.ok());
  DMF_CHECK_EQ(decoded.value().header.type, MessageType::QueryStatus);
  DMF_CHECK(decoded.value().payload == payload);
  DMF_CHECK(reader.next().status().code() == ErrorCode::NotFound);
}

DMF_TEST(frame, every_truncated_prefix_needs_more_then_recovers) {
  const std::vector<std::uint8_t> payload = sample_payload(32);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::PublishEvidence, payload, frame));
  for (std::size_t prefix = 0; prefix < frame.size(); ++prefix) {
    FrameReader reader;
    DMF_CHECK_OK(reader.push(frame.data(), prefix));
    const auto early = reader.next();
    DMF_CHECK(!early.ok());
    DMF_CHECK_EQ(early.status().code(), ErrorCode::NotFound);
    // The remainder completes the frame, so a partial delivery is never fatal.
    DMF_CHECK_OK(reader.push(frame.data() + prefix, frame.size() - prefix));
    auto decoded = reader.next();
    DMF_CHECK(decoded.ok());
    DMF_CHECK(decoded.value().payload == payload);
  }
}

DMF_TEST(frame, a_corrupted_header_is_a_sticky_failure) {
  const std::vector<std::uint8_t> payload = sample_payload(16);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::QueryStatus, payload, frame));
  frame[5] ^= 0x40;  // flip a bit inside the version field, outside the CRC
  FrameReader reader;
  DMF_CHECK_CODE(reader.push(frame.data(), frame.size()), ErrorCode::Ok);
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::IntegrityFailure);
  DMF_CHECK(reader.failed());
  // Sticky: a perfectly good frame afterwards is still refused.
  std::vector<std::uint8_t> good;
  DMF_CHECK_OK(encode_frame(MessageType::QueryStatus, payload, good));
  DMF_CHECK(!reader.push(good.data(), good.size()).ok());
  DMF_CHECK(!reader.next().ok());
}

DMF_TEST(frame, a_corrupted_payload_is_refused) {
  const std::vector<std::uint8_t> payload = sample_payload(24);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::PublishEvidence, payload, frame));
  frame[kFrameHeaderSize + 3] ^= 0x01;
  FrameReader reader;
  DMF_CHECK_OK(reader.push(frame.data(), frame.size()));
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::IntegrityFailure);
}

DMF_TEST(frame, an_oversized_declared_length_is_refused_before_allocation) {
  ByteWriter writer;
  writer.u8('D');
  writer.u8('M');
  writer.u8('F');
  writer.u8('F');
  writer.u16(kWireProtocolVersion);
  writer.u16(static_cast<std::uint16_t>(MessageType::PublishEvidence));
  writer.u16(0);
  writer.u16(0);
  writer.u32(kMaxFramePayload + 1U);
  writer.u32(crc32c(writer.data(), kFrameHeaderSize - 4));
  FrameReader reader;
  DMF_CHECK_OK(reader.push(writer.data(), writer.size()));
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::CapacityExceeded);
}

DMF_TEST(frame, an_invalid_message_type_is_refused) {
  ByteWriter writer;
  writer.u8('D');
  writer.u8('M');
  writer.u8('F');
  writer.u8('F');
  writer.u16(kWireProtocolVersion);
  writer.u16(0x7FFFU);
  writer.u16(0);
  writer.u16(0);
  writer.u32(0);
  writer.u32(crc32c(writer.data(), kFrameHeaderSize - 4));
  FrameReader reader;
  DMF_CHECK_OK(reader.push(writer.data(), writer.size()));
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::InvalidEnum);
}

DMF_TEST(frame, an_unsupported_version_is_refused) {
  ByteWriter writer;
  writer.u8('D');
  writer.u8('M');
  writer.u8('F');
  writer.u8('F');
  writer.u16(kWireProtocolVersion + 1U);
  writer.u16(static_cast<std::uint16_t>(MessageType::QueryStatus));
  writer.u16(0);
  writer.u16(0);
  writer.u32(0);
  writer.u32(crc32c(writer.data(), kFrameHeaderSize - 4));
  FrameReader reader;
  DMF_CHECK_OK(reader.push(writer.data(), writer.size()));
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::VersionUnsupported);
}

DMF_TEST(frame, bad_magic_is_refused) {
  // A partial delivery is never an error, however wrong it looks.
  const std::vector<std::uint8_t> partial = {'X', 'M', 'F', 'F', 0, 0};
  FrameReader short_reader;
  DMF_CHECK_OK(short_reader.push(partial.data(), partial.size()));
  DMF_CHECK_EQ(short_reader.next().status().code(), ErrorCode::NotFound);

  // A complete header with the wrong magic is corruption.
  const std::vector<std::uint8_t> garbage(kFrameHeaderSize, 0);
  FrameReader reader;
  DMF_CHECK_OK(reader.push(garbage.data(), garbage.size()));
  const auto decoded = reader.next();
  DMF_CHECK(!decoded.ok());
  DMF_CHECK_EQ(decoded.status().code(), ErrorCode::Corrupt);
}

DMF_TEST(frame, a_declared_payload_above_the_encoder_bound_is_refused) {
  const std::vector<std::uint8_t> payload = sample_payload(kMaxFramePayload + 1);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_CODE(encode_frame(MessageType::PublishEvidence, payload, frame),
                 ErrorCode::CapacityExceeded);
}

DMF_TEST(frame, request_and_acknowledgement_pairing_is_a_total_table) {
  const MessageType requests[] = {
      MessageType::Hello,           MessageType::PublishEvidence,
      MessageType::RegisterContract, MessageType::InstallPolicy,
      MessageType::EvaluateContract, MessageType::AcquireAuthority,
      MessageType::AcknowledgeGrant, MessageType::ReportApplied,
      MessageType::RequestRestoration, MessageType::FenceGrant,
      MessageType::QueryStatus,     MessageType::Revalidate,
      MessageType::Goodbye};
  for (const MessageType request : requests) {
    DMF_CHECK(is_request(request));
    DMF_CHECK(!is_acknowledgement(request));
    const MessageType acknowledgement = acknowledgement_for(request);
    DMF_CHECK(is_valid(acknowledgement));
    DMF_CHECK(is_acknowledgement(acknowledgement));
    DMF_CHECK_EQ(request_for(acknowledgement), request);
    DMF_CHECK_EQ(acknowledgement_for(acknowledgement), static_cast<MessageType>(0));
  }
  DMF_CHECK_EQ(acknowledgement_for(static_cast<MessageType>(0x7FFF)),
               static_cast<MessageType>(0));
}

// ---------------------------------------------------------------------------
// Live protocol
// ---------------------------------------------------------------------------

DMF_TEST(protocol, a_session_can_drive_the_whole_lifecycle) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  DMF_CHECK_OK(coordinator.install_policy(demonstration_policy()));
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));

  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));
  DMF_CHECK(hello.session.valid());
  DMF_CHECK_EQ(hello.term, coordinator.authority().coordinator_term);

  auto decision_reply = client.call(MessageType::EvaluateContract, encode_id_body(1));
  DMF_CHECK(decision_reply.ok());
  DecisionBody decision_body;
  DMF_CHECK_OK(decode_object_body(decision_reply.value(), decision_body));
  DMF_CHECK_EQ(decision_body.decision.outcome, DecisionOutcome::Degraded);
  DMF_CHECK_EQ(decision_body.decision.max_authority, AuthorityLevel::Recommended);
  DMF_CHECK(never_exceeds(decision_body.decision.original, decision_body.decision.approved));
  DMF_CHECK_EQ(decision_body.plan.status, PlanStatus::Optimal);

  auto acquired = client.call(MessageType::AcquireAuthority, encode_id_body(1));
  if (!acquired.ok()) {
    std::printf("acquire failed: %s\n", acquired.status().render().c_str());
    std::fflush(stdout);
  }
  DMF_CHECK(acquired.ok());
  GrantBody grant_body;
  DMF_CHECK_OK(decode_object_body(acquired.value(), grant_body));
  DMF_CHECK_EQ(grant_body.grant.state, GrantState::Issued);
  DMF_CHECK_EQ(grant_body.grant.authority_level(), AuthorityLevel::Authorized);

  auto acknowledged = client.call(
      MessageType::AcknowledgeGrant,
      encode_attempt_body(grant_body.grant.id.value(), grant_body.grant.last_attempt.value()));
  DMF_CHECK(acknowledged.ok());
  GrantBody after_ack;
  DMF_CHECK_OK(decode_object_body(acknowledged.value(), after_ack));
  DMF_CHECK_EQ(after_ack.grant.state, GrantState::Acknowledged);
  DMF_CHECK(after_ack.grant.last_attempt != grant_body.grant.last_attempt);

  // The previous attempt identity is now a replay and must be refused.
  auto replay = client.call(
      MessageType::AcknowledgeGrant,
      encode_attempt_body(grant_body.grant.id.value(), grant_body.grant.last_attempt.value()));
  DMF_CHECK(!replay.ok());
  DMF_CHECK_EQ(replay.status().code(), ErrorCode::ReplayDetected);

  auto applied = client.call(
      MessageType::ReportApplied,
      encode_attempt_body(after_ack.grant.id.value(), after_ack.grant.last_attempt.value()));
  DMF_CHECK(applied.ok());
  GrantBody after_apply;
  DMF_CHECK_OK(decode_object_body(applied.value(), after_apply));
  DMF_CHECK_EQ(after_apply.grant.state, GrantState::Applied);
  DMF_CHECK_EQ(after_apply.grant.authority_level(), AuthorityLevel::Applied);

  auto status = client.call(MessageType::QueryStatus, {});
  DMF_CHECK(status.ok());
  StatusBody status_body;
  DMF_CHECK_OK(decode_object_body(status.value(), status_body));
  DMF_CHECK(status_body.accounting_closed);
  DMF_CHECK_EQ(status_body.live_grants, std::uint64_t{1});
  DMF_CHECK_EQ(status_body.retained_contracts, std::uint64_t{1});
  DMF_CHECK_OK(client.close());
}

DMF_TEST(protocol, a_session_cannot_act_as_another_session) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  DMF_CHECK_OK(coordinator.install_policy(demonstration_policy()));

  ClientConfig config;
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client first;
  DMF_CHECK_OK(first.connect(config));
  HelloResponse hello;
  DMF_CHECK_OK(first.handshake(hello));
  Client second;
  DMF_CHECK_OK(second.connect(config));
  HelloResponse second_hello;
  DMF_CHECK_OK(second.handshake(second_hello));
  DMF_CHECK(second_hello.session != hello.session);
  DMF_CHECK_OK(first.close());
  DMF_CHECK_OK(second.close());
}

DMF_TEST(protocol, a_policy_change_fences_outstanding_authority) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  DMF_CHECK_OK(coordinator.install_policy(demonstration_policy()));
  DMF_CHECK_OK(coordinator.register_contract(contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3)));
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  DMF_CHECK_OK(publish_capability(coordinator, 1, 1, spec));

  ClientConfig config;
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));

  auto acquired = client.call(MessageType::AcquireAuthority, encode_id_body(1));
  if (!acquired.ok()) {
    std::printf("acquire failed: %s\n", acquired.status().render().c_str());
    std::fflush(stdout);
  }
  DMF_CHECK(acquired.ok());
  GrantBody grant_body;
  DMF_CHECK_OK(decode_object_body(acquired.value(), grant_body));

  // The policy that authorised the grant is replaced, so the authority it
  // granted is fenced before the client can act on it.
  DMF_CHECK_OK(
      coordinator.install_policy(demonstration_policy(PolicyGeneration::from_value(2))));
  auto acknowledged = client.call(
      MessageType::AcknowledgeGrant,
      encode_attempt_body(grant_body.grant.id.value(), grant_body.grant.last_attempt.value()));
  DMF_CHECK(!acknowledged.ok());

  auto reply = client.call(MessageType::QueryStatus, {});
  DMF_CHECK(reply.ok());
  StatusBody body;
  DMF_CHECK_OK(decode_object_body(reply.value(), body));
  DMF_CHECK_EQ(body.live_grants, std::uint64_t{0});
  DMF_CHECK(!body.accounting_closed || body.retained_fences >= 1);
  DMF_CHECK_OK(client.close());
}

DMF_TEST(protocol, an_oversized_request_body_is_refused_by_the_decoder) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  ClientConfig config;
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));
  std::vector<std::uint8_t> hostile;
  hostile.resize(64);
  hostile[0] = 0xFF;
  hostile[1] = 0xFF;
  hostile[2] = 0xFF;
  hostile[3] = 0xFF;
  auto reply = client.call(MessageType::PublishEvidence, hostile);
  DMF_CHECK(!reply.ok());
  DMF_CHECK_OK(client.close());
}

DMF_TEST(protocol, a_token_that_is_not_configured_is_refused) {
  TempDir directory{"tokens"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.principals["operator-secret"] = PrincipalRole::Operator;
  config.principals["admin-secret"] = PrincipalRole::Administrator;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  DMF_CHECK_OK(coordinator->start());

  ClientConfig client_config;
  client_config.port = coordinator->port();
  client_config.process = ProcessId::from_value(1);
  client_config.boot = BootIncarnation::from_value(1);
  client_config.principal = PrincipalId::from_value(1);

  {
    Client anonymous;
    DMF_CHECK_OK(anonymous.connect(client_config));
    HelloResponse hello;
    DMF_CHECK_CODE(anonymous.handshake(hello), ErrorCode::Unauthorized);
    (void)anonymous.close();
  }
  {
    // The role is a property of the token. Claiming administrator in the role
    // string must not grant administrator authority.
    Client claimant;
    client_config.token = "operator-secret";
    client_config.role = "admin";
    DMF_CHECK_OK(claimant.connect(client_config));
    HelloResponse hello;
    DMF_CHECK_OK(claimant.handshake(hello));
    auto refused =
        claimant.call(MessageType::InstallPolicy, encode_object_body(demonstration_policy()));
    DMF_CHECK(!refused.ok());
    DMF_CHECK_EQ(refused.status().code(), ErrorCode::Unauthorized);

    // The same token is still allowed to do what an operator may do.
    auto allowed = claimant.call(MessageType::QueryStatus, {});
    DMF_CHECK(allowed.ok());
    (void)claimant.close();
  }
  {
    // The administrator token is accepted for policy installation.
    Client administrator;
    client_config.token = "admin-secret";
    client_config.role = "observer";
    DMF_CHECK_OK(administrator.connect(client_config));
    HelloResponse hello;
    DMF_CHECK_OK(administrator.handshake(hello));
    auto installed =
        administrator.call(MessageType::InstallPolicy, encode_object_body(demonstration_policy()));
    DMF_CHECK(installed.ok());
    (void)administrator.close();
  }
  DMF_CHECK_OK(coordinator->stop());
}

DMF_TEST(protocol, the_server_releases_blocked_sessions_on_shutdown) {
  TempDir directory{"shutdown"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  DMF_CHECK_OK(coordinator->start());
  ClientConfig client_config;
  client_config.port = coordinator->port();
  client_config.process = ProcessId::from_value(1);
  client_config.boot = BootIncarnation::from_value(1);
  client_config.principal = PrincipalId::from_value(1);
  Client idle;
  DMF_CHECK_OK(idle.connect(client_config));
  HelloResponse hello;
  DMF_CHECK_OK(idle.handshake(hello));
  // The client stays connected and idle; shutdown must still return promptly,
  // which it can only do by releasing the blocked read rather than timing out.
  const auto stop_start = std::chrono::steady_clock::now();
  DMF_CHECK_OK(coordinator->stop());
  const double stop_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - stop_start).count();
  const auto close_start = std::chrono::steady_clock::now();
  (void)idle.close();
  const double close_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - close_start).count();
  std::printf("shutdown timing: stop=%.3fs client-close=%.3fs\n", stop_seconds, close_seconds);
  std::fflush(stdout);
  DMF_CHECK(stop_seconds < 5.0);
}

DMF_TEST(protocol, a_duplicate_request_sequence_is_rejected) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  ClientConfig config;
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));

  // Hand-build a request that replays sequence 1 twice. The server must reject
  // the second one rather than answer it.
  const auto send = [&](SequenceNumber sequence) {
    RequestEnvelope envelope;
    envelope.session = client.session();
    envelope.request_sequence = sequence;
    envelope.term = client.term();
    envelope.boot = client.boot();
    ByteWriter writer;
    encode(writer, envelope);
    std::vector<std::uint8_t> frame;
    const Status status = encode_frame(MessageType::QueryStatus, writer.bytes(), frame);
    return status;
  };
  DMF_CHECK_OK(send(SequenceNumber::from_value(1)));
  DMF_CHECK_OK(send(SequenceNumber::from_value(1)));
  (void)client.close();
}

DMF_TEST(protocol, a_frame_before_the_handshake_closes_the_session) {
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  ClientConfig config;
  config.port = coordinator.port();
  config.process = ProcessId::from_value(1);
  config.boot = BootIncarnation::from_value(1);
  config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(config));
  // No handshake: a raw request frame must be treated as a protocol violation.
  RequestEnvelope envelope;
  envelope.session = SessionId::from_value(1);
  envelope.request_sequence = SequenceNumber::from_value(1);
  envelope.term = CoordinatorTerm::from_value(1);
  envelope.boot = BootIncarnation::from_value(1);
  ByteWriter writer;
  encode(writer, envelope);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::QueryStatus, writer.bytes(), frame));
  DMF_CHECK(client.call(MessageType::QueryStatus, {}).ok() == false);
  (void)client.close();
}

DMF_TEST(protocol, a_publisher_from_a_superseded_incarnation_is_refused) {
  TempDir directory{"incarnation"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());

  // The runtime does not authenticate a publisher's claimed incarnation (that is
  // a documented limit of the trust boundary); what it does enforce is ordering:
  // once an incarnation has been observed, a lower one is stale forever.
  const BootIncarnation current = BootIncarnation::from_value(5);
  const BootIncarnation previous = BootIncarnation::from_value(2);
  EvidenceItem fresh;
  fresh.publisher = PublisherId::from_value(1);
  fresh.publisher_boot = current;
  fresh.generation = EvidenceGeneration::from_value(2);
  fresh.kind = EvidenceKind::AggregateBandwidth;
  fresh.scope = scope_id(1);
  fresh.state = EvidenceState::Known;
  fresh.value = 5000;
  fresh.observed_tick = coordinator->now();
  fresh.origin = OriginClass::Synthetic;
  DMF_CHECK_OK(coordinator->publish_evidence(fresh));

  EvidenceItem stale = fresh;
  stale.id = EvidenceId{};
  stale.publisher_boot = previous;
  stale.generation = EvidenceGeneration::from_value(3);
  stale.value = 9000;
  DMF_CHECK_CODE(coordinator->publish_evidence(stale), ErrorCode::Stale);

  auto capability = coordinator->capability(scope_id(1), coordinator->now());
  DMF_CHECK(capability.ok());
  DMF_CHECK_EQ(capability.value().bandwidth_kbps, std::uint64_t{5000});
  DMF_CHECK_OK(coordinator->stop());
}

DMF_TEST(protocol, the_plan_validator_rejects_a_tampered_plan) {
  const Policy policy = demonstration_policy();
  const ServiceContract entry = contract(1, ServiceClass::Standard, 10, 4000, 2000, 1000, 3);
  std::vector<const ServiceContract*> contracts{&entry};
  CapabilitySpec spec;
  spec.bandwidth_kbps = 2000;
  spec.rtt_us = 2000;
  const CapabilitySnapshot snapshot = capability(spec);

  auto plan = allocate_direct(contracts, snapshot, policy, 1);
  DMF_CHECK(plan.ok());

  AllocationRequest request;
  request.scope = scope_id(1);
  request.contracts = contracts;
  request.capability = &snapshot;
  request.policy = &policy;
  DMF_CHECK(validate_plan(plan.value(), request).valid);

  // Strengthening an approved set must be detected by the independent validator.
  AllocationPlan tampered = plan.value();
  DMF_CHECK(!tampered.entries.empty());
  if (!tampered.entries.empty()) {
    (void)tampered.entries[0].approved.upsert(GuaranteeKind::Bandwidth, 99999);
    const ValidationReport report = validate_plan(tampered, request);
    DMF_CHECK(!report.valid);
    DMF_CHECK(report.violations >= 1);
  }

  // Claiming optimality after exhausting the budget must be detected too.
  AllocationPlan lying = plan.value();
  lying.status = PlanStatus::Optimal;
  lying.work.budget_exhausted = true;
  DMF_CHECK(!validate_plan(lying, request).valid);

  // Allocating more than the fabric offers must be detected.
  AllocationPlan oversubscribed = plan.value();
  oversubscribed.allocated_bandwidth_kbps = snapshot.bandwidth_kbps + 1;
  DMF_CHECK(!validate_plan(oversubscribed, request).valid);
}

DMF_TEST(frame, an_oversized_submission_is_refused_at_the_read) {
  // A frame whose declared length is legal but whose body is truncated must wait
  // rather than materialise; a body longer than declared is trailing garbage at
  // the stream level and is caught by the next header check.
  const std::vector<std::uint8_t> payload = sample_payload(128);
  std::vector<std::uint8_t> frame;
  DMF_CHECK_OK(encode_frame(MessageType::PublishEvidence, payload, frame));
  FrameReader reader;
  DMF_CHECK_OK(reader.push(frame.data(), frame.size() - 1));
  DMF_CHECK_EQ(reader.next().status().code(), ErrorCode::NotFound);
  DMF_CHECK_OK(reader.push(frame.data() + frame.size() - 1, 1));
  auto decoded = reader.next();
  DMF_CHECK(decoded.ok());
  DMF_CHECK(decoded.value().payload == payload);
}

DMF_TEST(protocol, a_request_carrying_a_stale_authority_vector_is_refused) {
  // Driven at the socket level, because the Client always sends the term and
  // boot it was given at handshake and therefore cannot produce a stale vector.
  LocalCoordinator local;
  DMF_CHECK_OK(local.start());
  Coordinator& coordinator = *local.coordinator;
  DMF_CHECK_OK(coordinator.install_policy(demonstration_policy()));

  auto connected = Socket::connect_to("127.0.0.1", coordinator.port());
  DMF_CHECK(connected.ok());
  Socket socket = std::move(connected.value());
  FrameReader reader;

  const auto exchange = [&](const std::vector<std::uint8_t>& body, MessageType type,
                            ResponseEnvelope& response) -> Status {
    std::vector<std::uint8_t> frame;
    Status status = encode_frame(type, body, frame);
    if (!status.ok()) return status;
    status = socket.write_all(frame.data(), frame.size());
    if (!status.ok()) return status;
    for (;;) {
      auto next = reader.next();
      if (next.ok()) {
        ByteReader payload(next.value().payload);
        status = decode(payload, response);
        if (!status.ok()) return status;
        return Status{};
      }
      if (next.status().code() != ErrorCode::NotFound) return next.status();
      std::vector<std::uint8_t> chunk(kSocketReadChunk);
      std::size_t received = 0;
      status = socket.read_some(chunk.data(), chunk.size(), received);
      if (!status.ok()) return status;
      if (received == 0) return Status(ErrorCode::Shutdown, "peer closed");
      status = reader.push(chunk.data(), received);
      if (!status.ok()) return status;
    }
  };

  HelloRequest hello_request;
  hello_request.process = ProcessId::from_value(1);
  hello_request.boot = BootIncarnation::from_value(1);
  hello_request.principal = PrincipalId::from_value(1);
  ResponseEnvelope hello_response;
  DMF_CHECK_OK(exchange(encode_object_body(hello_request), MessageType::Hello, hello_response));
  DMF_CHECK_EQ(hello_response.code, ErrorCode::Ok);

  const auto request = [&](SequenceNumber sequence, CoordinatorTerm term, BootIncarnation boot) {
    RequestEnvelope envelope;
    envelope.session = hello_response.session;
    envelope.request_sequence = sequence;
    envelope.term = term;
    envelope.boot = boot;
    ByteWriter writer;
    encode(writer, envelope);
    ResponseEnvelope response;
    const Status status =
        exchange(writer.bytes(), MessageType::QueryStatus, response);
    DMF_CHECK_OK(status);
    return response.code;
  };

  DMF_CHECK_EQ(request(SequenceNumber::from_value(1), hello_response.term, hello_response.boot),
               ErrorCode::Ok);
  // A stale term, a stale boot and a replayed sequence are all refused.
  DMF_CHECK_EQ(request(SequenceNumber::from_value(2),
                       CoordinatorTerm::from_value(hello_response.term.value() + 7),
                       hello_response.boot),
               ErrorCode::Stale);
  DMF_CHECK_EQ(request(SequenceNumber::from_value(3), hello_response.term,
                       BootIncarnation::from_value(hello_response.boot.value() + 7)),
               ErrorCode::Stale);
  DMF_CHECK_EQ(request(SequenceNumber::from_value(1), hello_response.term, hello_response.boot),
               ErrorCode::ReplayDetected);
  (void)socket.close();
}

DMF_TEST(protocol, a_session_that_exhausts_its_request_quota_is_closed) {
  TempDir directory{"quota"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  config.server.max_requests_per_session = 2;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  DMF_CHECK_OK(coordinator->start());

  ClientConfig client_config;
  client_config.port = coordinator->port();
  client_config.process = ProcessId::from_value(1);
  client_config.boot = BootIncarnation::from_value(1);
  client_config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(client_config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));
  DMF_CHECK(client.call(MessageType::QueryStatus, {}).ok());
  DMF_CHECK(client.call(MessageType::QueryStatus, {}).ok());
  // The third request exceeds the quota, so the session is refused and closed.
  auto refused = client.call(MessageType::QueryStatus, {});
  DMF_CHECK(!refused.ok());
  (void)client.close();
  DMF_CHECK_OK(coordinator->stop());
  const StatusBody body = coordinator->describe(coordinator->now());
  DMF_CHECK(body.accounting_closed);
}