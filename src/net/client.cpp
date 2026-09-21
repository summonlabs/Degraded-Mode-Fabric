// Degraded Mode Fabric - framed-protocol client.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <exception>

#include "dmf/net.hpp"

namespace dmf {

Client::~Client() {
  // The graceful Goodbye is deliberately not attempted here: it performs I/O and
  // can throw, neither of which is acceptable in a destructor. The transport is
  // released directly; a caller that wants a clean handshake calls close().
  socket_.close_quietly();
  session_ = SessionId{};
}

Status Client::connect(const ClientConfig& config) {
  if (connected()) return Status(ErrorCode::InvalidState, "client is already connected");
  config_ = config;
  Status status = Socket::startup();
  if (!status.ok()) return status;
  auto socket = Socket::connect_to(config.host, config.port);
  if (!socket.ok()) return socket.status();
  socket_ = std::move(socket.value());
  reader_.reset();
  session_ = SessionId{};
  term_ = CoordinatorTerm{};
  boot_ = BootIncarnation{};
  request_sequence_ = SequenceNumber{};
  return Status{};
}

namespace {

Result<Frame> read_frame_from(Socket& socket, FrameReader& reader) {
  std::vector<std::uint8_t> chunk(kSocketReadChunk);
  for (;;) {
    Result<Frame> frame = reader.next();
    if (frame.ok()) return frame;
    if (frame.status().code() != ErrorCode::NotFound) return frame;
    std::size_t received = 0;
    Status status = socket.read_some(chunk.data(), chunk.size(), received);
    if (!status.ok()) return status;
    if (received == 0) {
      return Status(ErrorCode::Shutdown, "peer closed the connection");
    }
    status = reader.push(chunk.data(), received);
    if (!status.ok()) return status;
  }
}

}  // namespace

Status Client::handshake(HelloResponse& response) {
  if (!connected()) return Status(ErrorCode::InvalidState, "client is not connected");
  HelloRequest request;
  request.process = config_.process;
  request.boot = config_.boot;
  request.principal = config_.principal;
  request.token = config_.token;
  request.role = config_.role;
  Status status = request.validate();
  if (!status.ok()) return status;
  std::vector<std::uint8_t> frame;
  status = encode_frame(MessageType::Hello, encode_object_body(request), frame, 0,
                        config_.max_frame_payload);
  if (!status.ok()) return status;
  status = socket_.write_all(frame.data(), frame.size());
  if (!status.ok()) return status;

  Result<Frame> reply = read_frame_from(socket_, reader_);
  if (!reply.ok()) return reply.status();
  if (reply.value().header.type != MessageType::HelloAck) {
    return Status(ErrorCode::InvalidState, "expected a handshake acknowledgement");
  }
  ByteReader reader(reply.value().payload);
  ResponseEnvelope envelope;
  status = decode(reader, envelope);
  if (!status.ok()) return status;
  if (envelope.code != ErrorCode::Ok) {
    return Status(envelope.code, envelope.detail);
  }
  status = decode(reader, response);
  if (!status.ok()) return status;
  status = decode_finish(reader);
  if (!status.ok()) return status;
  session_ = response.session;
  term_ = response.term;
  boot_ = response.boot;
  return Status{};
}

Result<std::vector<std::uint8_t>> Client::call(MessageType type,
                                               const std::vector<std::uint8_t>& body) {
  if (!connected()) return Status(ErrorCode::InvalidState, "client is not connected");
  if (!session_.valid()) return Status(ErrorCode::InvalidState, "client has not handshaken");
  if (!is_request(type)) {
    return Status(ErrorCode::InvalidArgument, "call requires a request message type");
  }
  const auto next = request_sequence_.next();
  if (!next.has_value()) return Status(ErrorCode::Overflow, "request sequence exhausted");
  RequestEnvelope envelope;
  envelope.session = session_;
  envelope.request_sequence = *next;
  envelope.term = term_;
  envelope.boot = boot_;
  ByteWriter writer;
  encode(writer, envelope);
  writer.raw(body.data(), body.size());
  if (!writer.ok()) return Status(ErrorCode::CapacityExceeded, "request exceeds the encoder bound");
  std::vector<std::uint8_t> frame;
  Status status = encode_frame(type, writer.bytes(), frame, 0, config_.max_frame_payload);
  if (!status.ok()) return status;
  status = socket_.write_all(frame.data(), frame.size());
  if (!status.ok()) return status;
  request_sequence_ = *next;

  Result<Frame> reply = read_frame_from(socket_, reader_);
  if (!reply.ok()) return reply.status();
  const MessageType expected = acknowledgement_for(type);
  if (reply.value().header.type != expected) {
    return Status(ErrorCode::InvalidState, "response type does not match the request");
  }
  ByteReader reader(reply.value().payload);
  ResponseEnvelope response;
  status = decode(reader, response);
  if (!status.ok()) return status;
  if (response.session != session_) {
    return Status(ErrorCode::Unauthorized, "response belongs to a different session");
  }
  if (response.request_sequence != envelope.request_sequence) {
    return Status(ErrorCode::SequenceRegression, "response sequence does not match the request");
  }
  if (response.code != ErrorCode::Ok) {
    return Status(response.code, response.detail);
  }
  std::vector<std::uint8_t> result;
  const std::size_t remaining = reader.remaining();
  if (remaining > 0) {
    const std::string_view tail = reader.raw(remaining);
    result.assign(tail.begin(), tail.end());
  }
  return result;
}

Status Client::close() {
  if (!socket_.valid()) return Status{};
  if (session_.valid()) {
    (void)call(MessageType::Goodbye, {});
  }
  const Status status = socket_.close();
  session_ = SessionId{};
  return status;
}

}  // namespace dmf