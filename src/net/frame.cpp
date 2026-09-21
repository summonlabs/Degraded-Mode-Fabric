// Degraded Mode Fabric - frame codec.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/net.hpp"

namespace dmf {

namespace {

constexpr std::uint8_t kFrameMagic[4] = {'D', 'M', 'F', 'F'};
constexpr std::size_t kFrameHeaderCrcOffset = kFrameHeaderSize - 4;
constexpr std::size_t kFrameHeaderCovered = kFrameHeaderSize - 4;

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    (static_cast<std::uint16_t>(bytes[1]) << 8));
}

/// A frame may never make the decoder buffer hold more than one maximum frame
/// plus one socket read chunk.
std::size_t max_buffered(std::uint32_t max_payload) noexcept {
  return static_cast<std::size_t>(max_payload) + kFrameHeaderSize + kFrameTrailerSize +
         kSocketReadChunk;
}

}  // namespace

bool is_valid(MessageType value) noexcept {
  switch (value) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::PublishEvidence:
    case MessageType::PublishEvidenceAck:
    case MessageType::RegisterContract:
    case MessageType::RegisterContractAck:
    case MessageType::InstallPolicy:
    case MessageType::InstallPolicyAck:
    case MessageType::EvaluateContract:
    case MessageType::EvaluateContractAck:
    case MessageType::AcquireAuthority:
    case MessageType::AcquireAuthorityAck:
    case MessageType::AcknowledgeGrant:
    case MessageType::AcknowledgeGrantAck:
    case MessageType::ReportApplied:
    case MessageType::ReportAppliedAck:
    case MessageType::RequestRestoration:
    case MessageType::RequestRestorationAck:
    case MessageType::FenceGrant:
    case MessageType::FenceGrantAck:
    case MessageType::QueryStatus:
    case MessageType::QueryStatusAck:
    case MessageType::Revalidate:
    case MessageType::RevalidateAck:
    case MessageType::Goodbye:
    case MessageType::GoodbyeAck:
      return true;
  }
  return false;
}

std::string_view to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::PublishEvidence: return "PUBLISH_EVIDENCE";
    case MessageType::PublishEvidenceAck: return "PUBLISH_EVIDENCE_ACK";
    case MessageType::RegisterContract: return "REGISTER_CONTRACT";
    case MessageType::RegisterContractAck: return "REGISTER_CONTRACT_ACK";
    case MessageType::InstallPolicy: return "INSTALL_POLICY";
    case MessageType::InstallPolicyAck: return "INSTALL_POLICY_ACK";
    case MessageType::EvaluateContract: return "EVALUATE_CONTRACT";
    case MessageType::EvaluateContractAck: return "EVALUATE_CONTRACT_ACK";
    case MessageType::AcquireAuthority: return "ACQUIRE_AUTHORITY";
    case MessageType::AcquireAuthorityAck: return "ACQUIRE_AUTHORITY_ACK";
    case MessageType::AcknowledgeGrant: return "ACKNOWLEDGE_GRANT";
    case MessageType::AcknowledgeGrantAck: return "ACKNOWLEDGE_GRANT_ACK";
    case MessageType::ReportApplied: return "REPORT_APPLIED";
    case MessageType::ReportAppliedAck: return "REPORT_APPLIED_ACK";
    case MessageType::RequestRestoration: return "REQUEST_RESTORATION";
    case MessageType::RequestRestorationAck: return "REQUEST_RESTORATION_ACK";
    case MessageType::FenceGrant: return "FENCE_GRANT";
    case MessageType::FenceGrantAck: return "FENCE_GRANT_ACK";
    case MessageType::QueryStatus: return "QUERY_STATUS";
    case MessageType::QueryStatusAck: return "QUERY_STATUS_ACK";
    case MessageType::Revalidate: return "REVALIDATE";
    case MessageType::RevalidateAck: return "REVALIDATE_ACK";
    case MessageType::Goodbye: return "GOODBYE";
    case MessageType::GoodbyeAck: return "GOODBYE_ACK";
  }
  return "UNRECOGNISED";
}

bool is_acknowledgement(MessageType value) noexcept {
  switch (value) {
    case MessageType::HelloAck:
    case MessageType::PublishEvidenceAck:
    case MessageType::RegisterContractAck:
    case MessageType::InstallPolicyAck:
    case MessageType::EvaluateContractAck:
    case MessageType::AcquireAuthorityAck:
    case MessageType::AcknowledgeGrantAck:
    case MessageType::ReportAppliedAck:
    case MessageType::RequestRestorationAck:
    case MessageType::FenceGrantAck:
    case MessageType::QueryStatusAck:
    case MessageType::RevalidateAck:
    case MessageType::GoodbyeAck:
      return true;
    case MessageType::Hello:
    case MessageType::PublishEvidence:
    case MessageType::RegisterContract:
    case MessageType::InstallPolicy:
    case MessageType::EvaluateContract:
    case MessageType::AcquireAuthority:
    case MessageType::AcknowledgeGrant:
    case MessageType::ReportApplied:
    case MessageType::RequestRestoration:
    case MessageType::FenceGrant:
    case MessageType::QueryStatus:
    case MessageType::Revalidate:
    case MessageType::Goodbye:
      return false;
  }
  return false;
}

bool is_request(MessageType value) noexcept { return is_valid(value) && !is_acknowledgement(value); }

MessageType acknowledgement_for(MessageType request) noexcept {
  switch (request) {
    case MessageType::Hello: return MessageType::HelloAck;
    case MessageType::PublishEvidence: return MessageType::PublishEvidenceAck;
    case MessageType::RegisterContract: return MessageType::RegisterContractAck;
    case MessageType::InstallPolicy: return MessageType::InstallPolicyAck;
    case MessageType::EvaluateContract: return MessageType::EvaluateContractAck;
    case MessageType::AcquireAuthority: return MessageType::AcquireAuthorityAck;
    case MessageType::AcknowledgeGrant: return MessageType::AcknowledgeGrantAck;
    case MessageType::ReportApplied: return MessageType::ReportAppliedAck;
    case MessageType::RequestRestoration: return MessageType::RequestRestorationAck;
    case MessageType::FenceGrant: return MessageType::FenceGrantAck;
    case MessageType::QueryStatus: return MessageType::QueryStatusAck;
    case MessageType::Revalidate: return MessageType::RevalidateAck;
    case MessageType::Goodbye: return MessageType::GoodbyeAck;
    default: return kNoMessageType;
  }
}

MessageType request_for(MessageType acknowledgement) noexcept {
  switch (acknowledgement) {
    case MessageType::HelloAck: return MessageType::Hello;
    case MessageType::PublishEvidenceAck: return MessageType::PublishEvidence;
    case MessageType::RegisterContractAck: return MessageType::RegisterContract;
    case MessageType::InstallPolicyAck: return MessageType::InstallPolicy;
    case MessageType::EvaluateContractAck: return MessageType::EvaluateContract;
    case MessageType::AcquireAuthorityAck: return MessageType::AcquireAuthority;
    case MessageType::AcknowledgeGrantAck: return MessageType::AcknowledgeGrant;
    case MessageType::ReportAppliedAck: return MessageType::ReportApplied;
    case MessageType::RequestRestorationAck: return MessageType::RequestRestoration;
    case MessageType::FenceGrantAck: return MessageType::FenceGrant;
    case MessageType::QueryStatusAck: return MessageType::QueryStatus;
    case MessageType::RevalidateAck: return MessageType::Revalidate;
    case MessageType::GoodbyeAck: return MessageType::Goodbye;
    default: return kNoMessageType;
  }
}

Status encode_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                    std::vector<std::uint8_t>& out, std::uint16_t flags, std::uint32_t max_payload) {
  if (!is_valid(type)) return Status(ErrorCode::InvalidEnum, "message type is not known");
  if (payload.size() > max_payload) {
    return Status(ErrorCode::CapacityExceeded, "frame payload exceeds the negotiated bound");
  }
  if (flags != 0) return Status(ErrorCode::InvalidArgument, "frame flags must be zero");
  ByteWriter writer(kFrameHeaderSize + payload.size() + kFrameTrailerSize);
  for (const std::uint8_t byte : kFrameMagic) writer.u8(byte);
  writer.u16(kWireProtocolVersion);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u16(flags);
  writer.u16(0);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u32(crc32c(writer.data(), kFrameHeaderCovered));
  writer.raw(payload.data(), payload.size());
  writer.u32(crc32c(payload.data(), payload.size()));
  if (!writer.ok()) return Status(ErrorCode::CapacityExceeded, "frame exceeds the encoder bound");
  out = writer.bytes();
  return Status{};
}

FrameReader::FrameReader(std::uint32_t max_payload) : max_payload_(max_payload) {
  if (max_payload_ == 0 || max_payload_ > kMaxFramePayload) max_payload_ = kMaxFramePayload;
}

FrameReader::~FrameReader() = default;

void FrameReader::reset() noexcept {
  buffer_.clear();
  offset_ = 0;
  error_ = ErrorCode::Ok;
}

Status FrameReader::push(const std::uint8_t* data, std::size_t size) {
  if (error_ != ErrorCode::Ok) return Status(error_, "frame decoder is poisoned");
  if (size == 0) return Status{};
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  } else if (offset_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  const auto total = checked_add(buffer_.size(), size);
  if (!total.has_value()) {
    error_ = ErrorCode::Overflow;
    return Status(error_, "frame buffer length overflowed");
  }
  if (*total > max_buffered(max_payload_)) {
    error_ = ErrorCode::CapacityExceeded;
    return Status(error_, "frame buffer bound exceeded; the peer is not draining");
  }
  buffer_.insert(buffer_.end(), data, data + size);
  return Status{};
}

Result<Frame> FrameReader::next() {
  if (error_ != ErrorCode::Ok) return Status(error_, "frame decoder is poisoned");
  const std::size_t available = buffer_.size() - offset_;
  if (available < kFrameHeaderSize) {
    return Status(ErrorCode::NotFound, "need more bytes to complete a frame header");
  }
  const std::uint8_t* header = buffer_.data() + offset_;
  if (header[0] != kFrameMagic[0] || header[1] != kFrameMagic[1] || header[2] != kFrameMagic[2] ||
      header[3] != kFrameMagic[3]) {
    error_ = ErrorCode::Corrupt;
    return Status(error_, "frame magic does not match");
  }
  if (read_u32(header + kFrameHeaderCrcOffset) != crc32c(header, kFrameHeaderCovered)) {
    error_ = ErrorCode::IntegrityFailure;
    return Status(error_, "frame header fails its integrity check");
  }
  const std::uint16_t version = read_u16(header + 4);
  if (version != kWireProtocolVersion) {
    error_ = ErrorCode::VersionUnsupported;
    return Status(error_, "frame protocol version is not supported");
  }
  const std::uint16_t raw_type = read_u16(header + 6);
  const MessageType type = static_cast<MessageType>(raw_type);
  if (!is_valid(type)) {
    error_ = ErrorCode::InvalidEnum;
    return Status(error_, "frame carries an unknown message type");
  }
  if (read_u16(header + 8) != 0) {
    error_ = ErrorCode::Invalid;
    return Status(error_, "frame carries unsupported flags");
  }
  if (read_u16(header + 10) != 0) {
    error_ = ErrorCode::Corrupt;
    return Status(error_, "frame reserved field is not zero");
  }
  const std::uint32_t payload_length = read_u32(header + 12);
  if (payload_length > max_payload_) {
    error_ = ErrorCode::CapacityExceeded;
    return Status(error_, "frame declares a payload above the negotiated bound");
  }
  const auto total = checked_add(
      checked_add(static_cast<std::uint64_t>(kFrameHeaderSize),
                  static_cast<std::uint64_t>(payload_length))
          .value_or(std::numeric_limits<std::uint64_t>::max()),
      static_cast<std::uint64_t>(kFrameTrailerSize));
  if (!total.has_value() || *total > std::numeric_limits<std::size_t>::max()) {
    error_ = ErrorCode::Overflow;
    return Status(error_, "frame length overflowed");
  }
  if (available < static_cast<std::size_t>(*total)) {
    return Status(ErrorCode::NotFound, "need more bytes to complete the frame payload");
  }
  const std::uint8_t* payload_bytes = header + kFrameHeaderSize;
  const std::uint32_t payload_crc = read_u32(payload_bytes + payload_length);
  if (payload_crc != crc32c(payload_bytes, payload_length)) {
    error_ = ErrorCode::IntegrityFailure;
    return Status(error_, "frame payload fails its integrity check");
  }
  Frame frame;
  frame.header.version = version;
  frame.header.type = type;
  frame.header.flags = 0;
  frame.header.payload_length = payload_length;
  frame.payload.assign(payload_bytes, payload_bytes + payload_length);
  offset_ += static_cast<std::size_t>(*total);
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  }
  return frame;
}

}  // namespace dmf
