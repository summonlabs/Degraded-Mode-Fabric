// Degraded Mode Fabric - threaded framed-protocol server.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Ownership and locking rules enforced here (see docs/CONCURRENCY-AUDIT.md):
//   * every session owns exactly one socket and exactly one thread;
//   * the handler is invoked on the session thread and never while the server
//     registry mutex is held;
//   * shutdown releases blocked accept and read operations before any join, so
//     no thread is ever joined while it waits on a resource the joiner holds;
//   * the registry mutex is never held across socket I/O or thread joins;
//   * a session is registered before its thread exists, so shutdown can never
//     miss a live session.
#include <algorithm>

#include "dmf/net.hpp"

namespace dmf {

struct SessionState {
  SessionId id{};
  Socket socket{};
  std::thread thread{};
  HelloRequest hello{};
};

struct Server::Impl {
  ServerConfig config{};
  SessionHandler* handler = nullptr;
  Accounting* accounting = nullptr;
  Listener listener{};
  std::thread accept_thread{};
  mutable std::mutex mutex{};
  std::vector<std::shared_ptr<SessionState>> sessions{};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> next_session{1};
  std::atomic<std::size_t> active_sessions{0};
};

namespace {

Status send_body(Socket& socket, MessageType type, const std::vector<std::uint8_t>& body,
                 std::uint32_t max_payload, Accounting* accounting) {
  std::vector<std::uint8_t> frame;
  Status status = encode_frame(type, body, frame, 0, max_payload);
  if (!status.ok()) return status;
  if (accounting != nullptr) (void)accounting->record_frame_sent();
  return socket.write_all(frame.data(), frame.size());
}

Status send_response(Socket& socket, MessageType type, const ResponseEnvelope& envelope,
                     const std::vector<std::uint8_t>& body, std::uint32_t max_payload,
                     Accounting* accounting) {
  ByteWriter writer;
  encode(writer, envelope);
  writer.raw(body.data(), body.size());
  if (!writer.ok()) return Status(ErrorCode::CapacityExceeded, "response exceeds the encoder bound");
  return send_body(socket, type, writer.bytes(), max_payload, accounting);
}

MessageType response_type(MessageType request) noexcept { return acknowledgement_for(request); }

void run_session(Server::Impl& impl, const std::shared_ptr<SessionState>& state) {
  Socket& socket = state->socket;
  FrameReader reader(impl.config.max_frame_payload);
  std::vector<std::uint8_t> chunk(kSocketReadChunk);
  bool handshaken = false;
  SequenceNumber last_sequence{};
  std::size_t requests = 0;

  for (;;) {
    if (impl.stopping.load()) break;
    Result<Frame> frame = reader.next();
    if (!frame.ok()) {
      if (frame.status().code() != ErrorCode::NotFound) {
        if (impl.accounting != nullptr) {
          (void)impl.accounting->record_frame_rejected(frame.status().code() ==
                                                       ErrorCode::CapacityExceeded);
        }
        break;
      }
      std::size_t received = 0;
      Status status = socket.read_some(chunk.data(), chunk.size(), received);
      if (!status.ok()) break;
      if (received == 0) break;
      status = reader.push(chunk.data(), received);
      if (!status.ok()) {
        if (impl.accounting != nullptr) {
          (void)impl.accounting->record_frame_rejected(status.code() == ErrorCode::CapacityExceeded);
        }
        break;
      }
      continue;
    }
    if (impl.accounting != nullptr) (void)impl.accounting->record_frame_received();
    const Frame current = std::move(frame.value());
    const MessageType type = current.header.type;

    if (type == MessageType::Hello) {
      HelloResponse hello_response;
      HelloRequest hello_request;
      Status status = [&]() {
        ByteReader hello_reader(current.payload);
        Status inner = decode(hello_reader, hello_request);
        if (!inner.ok()) return inner;
        return decode_finish(hello_reader);
      }();
      if (status.ok() && handshaken) {
        status = Status(ErrorCode::InvalidState, "session completed a handshake twice");
      }
      if (status.ok()) status = impl.handler->on_hello(state->id, hello_request, hello_response);
      ResponseEnvelope envelope;
      std::vector<std::uint8_t> body;
      if (!status.ok()) {
        envelope.code = status.code();
        envelope.detail = status.detail();
      } else {
        // The session identity is minted by the server, never by the handler:
        // a session can only ever act under the identity it was bound to.
        hello_response.session = state->id;
        envelope.session = state->id;
        envelope.term = hello_response.term;
        envelope.boot = hello_response.boot;
        body = encode_object_body(hello_response);
        handshaken = true;
        state->hello = hello_request;
      }
      if (!send_response(socket, MessageType::HelloAck, envelope, body,
                         impl.config.max_frame_payload, impl.accounting)
               .ok()) {
        break;
      }
      if (!status.ok()) break;
      continue;
    }

    if (!handshaken) {
      // Any frame before a completed handshake is a protocol violation.
      if (impl.accounting != nullptr) (void)impl.accounting->record_request_rejected();
      break;
    }

    RequestEnvelope envelope;
    ByteReader request_reader(current.payload);
    Status status = decode(request_reader, envelope);
    const std::size_t prefix = request_reader.consumed();
    std::vector<std::uint8_t> body_bytes;
    if (status.ok()) {
      body_bytes.assign(current.payload.begin() + static_cast<std::ptrdiff_t>(prefix),
                        current.payload.end());
    }
    ResponseEnvelope response;
    response.session = state->id;
    response.request_sequence = envelope.request_sequence;
    std::vector<std::uint8_t> response_body;

    bool fatal = false;
    if (!status.ok()) {
      response.code = status.code();
      response.detail = status.detail();
      fatal = true;
    } else if (envelope.session != state->id) {
      response.code = ErrorCode::Unauthorized;
      response.detail = "request identity does not match the bound session";
      fatal = true;
    } else if (last_sequence.valid() && envelope.request_sequence <= last_sequence) {
      response.code = ErrorCode::ReplayDetected;
      response.detail = "request sequence did not advance";
      fatal = true;
    } else if (++requests > impl.config.max_requests_per_session) {
      response.code = ErrorCode::QuotaExceeded;
      response.detail = "session request quota exhausted";
      fatal = true;
    } else {
      last_sequence = envelope.request_sequence;
      status = impl.handler->on_request(state->id, envelope, type, body_bytes, response,
                                        response_body);
      if (!status.ok()) {
        response.code = status.code();
        response.detail = status.detail();
      }
    }

    if (!send_response(socket, response_type(type), response, response_body,
                       impl.config.max_frame_payload, impl.accounting)
             .ok()) {
      break;
    }
    if (type == MessageType::Goodbye || fatal) break;
  }
}

void session_loop(Server::Impl& impl, const std::shared_ptr<SessionState>& state) {
  try {
    run_session(impl, state);
  } catch (...) {
    // A handler must never unwind through a session thread: terminate() would
    // take the whole runtime down with it. The session is ended and the
    // rejection is accounted for rather than silently swallowed.
    impl.active_sessions.fetch_add(0);
    if (impl.accounting != nullptr) (void)impl.accounting->record_request_rejected();
  }
  // Teardown runs on this thread only: it is the sole owner of the descriptor.
  (void)state->socket.close();
  if (impl.accounting != nullptr) (void)impl.accounting->record_session_closed();
  impl.handler->on_session_closed(state->id);
  impl.active_sessions.fetch_sub(1);
}

void accept_loop(Server::Impl& impl) {
  for (;;) {
    Socket socket;
    Status status = impl.listener.accept(socket);
    if (!status.ok()) break;
    if (impl.stopping.load()) {
      (void)socket.close();
      break;
    }
    if (impl.active_sessions.load() >= impl.config.max_sessions) {
      if (impl.accounting != nullptr) (void)impl.accounting->record_session_rejected();
      (void)socket.close();
      continue;
    }
    const std::uint64_t raw = impl.next_session.fetch_add(1);
    if (raw == 0) {
      (void)socket.close();
      break;
    }
    auto state = std::make_shared<SessionState>();
    state->id = SessionId::from_value(raw);
    state->socket = std::move(socket);
    const Status channel = state->socket.open_wakeup_channel();
    if (!channel.ok()) {
      (void)state->socket.close();
      continue;
    }
    impl.active_sessions.fetch_add(1);
    if (impl.accounting != nullptr) (void)impl.accounting->record_session_opened();
    {
      // The state is registered before its thread exists, so shutdown can never
      // miss a live session.
      const std::lock_guard<std::mutex> guard(impl.mutex);
      impl.sessions.push_back(state);
    }
    state->thread = std::thread([&impl, state]() { session_loop(impl, state); });
  }
}

void drain_sessions(Server::Impl& impl, std::vector<std::shared_ptr<SessionState>>& out) {
  const std::lock_guard<std::mutex> guard(impl.mutex);
  out.swap(impl.sessions);
}

void release_and_join(std::vector<std::shared_ptr<SessionState>>& drained) {
  for (const std::shared_ptr<SessionState>& state : drained) {
    (void)state->socket.release_blocked_io();
  }
  for (const std::shared_ptr<SessionState>& state : drained) {
    if (state->thread.joinable()) state->thread.join();
  }
  drained.clear();
}

}  // namespace

Server::Server(ServerConfig config, SessionHandler& handler, Accounting* accounting)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = config;
  impl_->handler = &handler;
  impl_->accounting = accounting;
}

Server::~Server() {
  const Status status = stop();
  (void)status;
}

Status Server::start() {
  Status status = Socket::startup();
  if (!status.ok()) return status;
  auto listener = Listener::bind_loopback(impl_->config.port, impl_->config.backlog);
  if (!listener.ok()) return listener.status();
  impl_->listener = std::move(listener.value());
  port_ = impl_->listener.port();
  impl_->stopping.store(false);
  running_.store(true);
  impl_->accept_thread = std::thread([this]() { accept_loop(*impl_); });
  return Status{};
}

Status Server::stop() {
  if (impl_ == nullptr) return Status{};
  impl_->stopping.store(true);
  running_.store(false);
  std::vector<std::shared_ptr<SessionState>> drained;
  drain_sessions(*impl_, drained);
  // Release blocked accepts and reads before any join happens, so no thread is
  // ever joined while it waits on a resource the joiner controls.
  (void)impl_->listener.release_blocked_io();
  release_and_join(drained);
  (void)impl_->listener.close();
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
  // The accept thread has stopped, so no further session can be registered;
  // drain once more to catch a session that registered while we were joining.
  std::vector<std::shared_ptr<SessionState>> late;
  drain_sessions(*impl_, late);
  release_and_join(late);
  return Status{};
}

std::size_t Server::session_count() const { return impl_->active_sessions.load(); }

}  // namespace dmf