// Degraded Mode Fabric - protocol bodies.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/net.hpp"

namespace dmf {

Status HelloRequest::validate() const {
  if (protocol_version != kWireProtocolVersion) {
    return Status(ErrorCode::VersionUnsupported, "client protocol version is not supported");
  }
  if (!process.valid()) return Status(ErrorCode::InvalidArgument, "hello carries no process identity");
  if (!boot.valid()) return Status(ErrorCode::InvalidArgument, "hello carries no boot incarnation");
  if (!principal.valid()) return Status(ErrorCode::InvalidArgument, "hello carries no principal");
  if (token.size() > kMaxTokenBytes) return Status(ErrorCode::CapacityExceeded, "token is too long");
  if (role.size() > kMaxRoleBytes) return Status(ErrorCode::CapacityExceeded, "role is too long");
  if (!is_text_safe(token) || !is_text_safe(role)) {
    return Status(ErrorCode::InvalidArgument, "hello carries unsafe text");
  }
  return Status{};
}

Status HelloResponse::validate() const {
  if (!session.valid()) return Status(ErrorCode::InvalidArgument, "session identity is unset");
  if (!term.valid()) return Status(ErrorCode::InvalidArgument, "coordinator term is unset");
  if (!boot.valid()) return Status(ErrorCode::InvalidArgument, "boot incarnation is unset");
  // The authority vector is reported truthfully, including components that are
  // still unset because the corresponding subsystem has not been configured
  // (an unset policy generation means no policy is installed). Requiring it to
  // be complete here would force the handshake to lie.
  return Status{};
}

Status RequestEnvelope::validate() const {
  if (!session.valid()) return Status(ErrorCode::InvalidArgument, "request carries no session");
  if (!request_sequence.valid()) {
    return Status(ErrorCode::InvalidArgument, "request carries no sequence");
  }
  if (!term.valid()) return Status(ErrorCode::InvalidArgument, "request carries no term");
  if (!boot.valid()) return Status(ErrorCode::InvalidArgument, "request carries no boot leaf");
  return Status{};
}

void encode(ByteWriter& writer, const HelloRequest& value) {
  writer.u32(value.protocol_version);
  writer.u64(value.process.value());
  writer.u64(value.boot.value());
  writer.u64(value.principal.value());
  writer.blob(value.token);
  writer.blob(value.role);
}

Status decode(ByteReader& reader, HelloRequest& value) {
  HelloRequest candidate;
  candidate.protocol_version = reader.u32();
  candidate.process = ProcessId::from_value(reader.u64());
  candidate.boot = BootIncarnation::from_value(reader.u64());
  candidate.principal = PrincipalId::from_value(reader.u64());
  candidate.token = std::string(reader.blob());
  candidate.role = std::string(reader.blob());
  if (!reader.ok()) return Status(reader.error());
  Status status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const HelloResponse& value) {
  writer.u64(value.session.value());
  writer.u64(value.term.value());
  writer.u64(value.boot.value());
  encode(writer, value.authority);
  writer.blob(value.detail);
}

Status decode(ByteReader& reader, HelloResponse& value) {
  HelloResponse candidate;
  candidate.session = SessionId::from_value(reader.u64());
  candidate.term = CoordinatorTerm::from_value(reader.u64());
  candidate.boot = BootIncarnation::from_value(reader.u64());
  Status status = decode(reader, candidate.authority);
  if (!status.ok()) return status;
  candidate.detail = std::string(reader.blob());
  if (!reader.ok()) return Status(reader.error());
  status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const RequestEnvelope& value) {
  writer.u64(value.session.value());
  writer.u64(value.request_sequence.value());
  writer.u64(value.term.value());
  writer.u64(value.boot.value());
}

Status decode(ByteReader& reader, RequestEnvelope& value) {
  RequestEnvelope candidate;
  candidate.session = SessionId::from_value(reader.u64());
  candidate.request_sequence = SequenceNumber::from_value(reader.u64());
  candidate.term = CoordinatorTerm::from_value(reader.u64());
  candidate.boot = BootIncarnation::from_value(reader.u64());
  if (!reader.ok()) return Status(reader.error());
  Status status = candidate.validate();
  if (!status.ok()) return status;
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const ResponseEnvelope& value) {
  writer.u64(value.session.value());
  writer.u64(value.request_sequence.value());
  writer.u64(value.term.value());
  writer.u64(value.boot.value());
  writer.u16(static_cast<std::uint16_t>(value.code));
  writer.blob(value.detail);
}

Status decode(ByteReader& reader, ResponseEnvelope& value) {
  ResponseEnvelope candidate;
  candidate.session = SessionId::from_value(reader.u64());
  candidate.request_sequence = SequenceNumber::from_value(reader.u64());
  candidate.term = CoordinatorTerm::from_value(reader.u64());
  candidate.boot = BootIncarnation::from_value(reader.u64());
  candidate.code = read_enum<ErrorCode>(reader, ErrorCode::IoError);
  candidate.detail = std::string(reader.blob());
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const GrantBody& value) { encode(writer, value.grant); }

Status decode(ByteReader& reader, GrantBody& value) { return decode(reader, value.grant); }

void encode(ByteWriter& writer, const DecisionBody& value) {
  encode(writer, value.decision);
  encode(writer, value.plan);
}

Status decode(ByteReader& reader, DecisionBody& value) {
  Status status = decode(reader, value.decision);
  if (!status.ok()) return status;
  return decode(reader, value.plan);
}

void encode(ByteWriter& writer, const StatusBody& value) {
  encode(writer, value.authority);
  writer.u64(value.boot.value());
  writer.u64(value.term.value());
  writer.u64(value.evidence_generation.value());
  writer.u64(value.capacity_generation.value());
  writer.u64(value.fabric_generation.value());
  writer.u64(value.live_grants);
  writer.u64(value.retained_grants);
  writer.u64(value.retained_fences);
  writer.u64(value.retained_decisions);
  writer.u64(value.retained_contracts);
  writer.u64(value.journal_records);
  writer.u64(value.journal_bytes_in_segment);
  writer.boolean(value.accounting_closed);
  writer.boolean(value.policy_installed);
  writer.blob(value.policy_digest);
  writer.blob(value.recovery);
  writer.blob(value.recovery_detail);
}

Status decode(ByteReader& reader, StatusBody& value) {
  StatusBody candidate;
  Status status = decode(reader, candidate.authority);
  if (!status.ok()) return status;
  candidate.boot = BootIncarnation::from_value(reader.u64());
  candidate.term = CoordinatorTerm::from_value(reader.u64());
  candidate.evidence_generation = EvidenceGeneration::from_value(reader.u64());
  candidate.capacity_generation = CapacityGeneration::from_value(reader.u64());
  candidate.fabric_generation = FabricGeneration::from_value(reader.u64());
  candidate.live_grants = reader.u64();
  candidate.retained_grants = reader.u64();
  candidate.retained_fences = reader.u64();
  candidate.retained_decisions = reader.u64();
  candidate.retained_contracts = reader.u64();
  candidate.journal_records = reader.u64();
  candidate.journal_bytes_in_segment = reader.u64();
  candidate.accounting_closed = reader.boolean();
  candidate.policy_installed = reader.boolean();
  candidate.policy_digest = std::string(reader.blob());
  candidate.recovery = std::string(reader.blob());
  candidate.recovery_detail = std::string(reader.blob());
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

void encode(ByteWriter& writer, const RestorationBody& value) {
  encode(writer, value.evaluation);
}

Status decode(ByteReader& reader, RestorationBody& value) {
  return decode(reader, value.evaluation);
}

void encode(ByteWriter& writer, const RevalidateBody& value) {
  writer.u64(value.evaluated);
  writer.u64(value.fenced);
  writer.u64(value.expired);
}

Status decode(ByteReader& reader, RevalidateBody& value) {
  RevalidateBody candidate;
  candidate.evaluated = reader.u64();
  candidate.fenced = reader.u64();
  candidate.expired = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  value = candidate;
  return Status{};
}

std::vector<std::uint8_t> encode_id_body(std::uint64_t id) {
  ByteWriter writer(16);
  writer.u64(id);
  return writer.bytes();
}

Status decode_id_body(const std::vector<std::uint8_t>& body, std::uint64_t& id) {
  ByteReader reader(body);
  id = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  return decode_finish(reader);
}

std::vector<std::uint8_t> encode_attempt_body(std::uint64_t grant, std::uint64_t attempt) {
  ByteWriter writer(32);
  writer.u64(grant);
  writer.u64(attempt);
  return writer.bytes();
}

Status decode_attempt_body(const std::vector<std::uint8_t>& body, std::uint64_t& grant,
                           std::uint64_t& attempt) {
  ByteReader reader(body);
  grant = reader.u64();
  attempt = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  return decode_finish(reader);
}

std::vector<std::uint8_t> encode_fence_body(std::uint64_t grant, FenceReason reason) {
  ByteWriter writer(32);
  writer.u64(grant);
  write_enum(writer, reason);
  return writer.bytes();
}

Status decode_fence_body(const std::vector<std::uint8_t>& body, std::uint64_t& grant,
                         FenceReason& reason) {
  ByteReader reader(body);
  grant = reader.u64();
  reason = read_enum<FenceReason>(reader, FenceReason::Manual);
  if (!reader.ok()) return Status(reader.error());
  return decode_finish(reader);
}

}  // namespace dmf
