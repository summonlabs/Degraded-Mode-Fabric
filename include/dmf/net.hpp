// Degraded Mode Fabric - bounded framed protocol and loopback transport.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Trust boundary
// --------------
// The transport carries no confidentiality and no cryptographic authentication.
// A session is bound to a principal by a bearer token comparison performed by a
// pluggable authenticator; an operator who needs real authentication must supply
// one. Integrity checks here detect corruption and torn frames, not forgery.
// See docs/PROTOCOL.md.
#ifndef DMF_NET_HPP
#define DMF_NET_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

inline constexpr std::size_t kFrameHeaderSize = 20;
inline constexpr std::size_t kFrameTrailerSize = 4;
inline constexpr std::uint32_t kMaxFramePayload = 1U << 20;
inline constexpr std::size_t kSocketReadChunk = 1U << 16;
inline constexpr std::size_t kMaxTokenBytes = 256;
inline constexpr std::size_t kMaxRoleBytes = 64;

/// Every protocol message type. The numbering is part of the wire contract.
enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  PublishEvidence = 10,
  PublishEvidenceAck = 11,
  RegisterContract = 20,
  RegisterContractAck = 21,
  InstallPolicy = 30,
  InstallPolicyAck = 31,
  EvaluateContract = 40,
  EvaluateContractAck = 41,
  AcquireAuthority = 50,
  AcquireAuthorityAck = 51,
  AcknowledgeGrant = 60,
  AcknowledgeGrantAck = 61,
  ReportApplied = 70,
  ReportAppliedAck = 71,
  RequestRestoration = 80,
  RequestRestorationAck = 81,
  FenceGrant = 90,
  FenceGrantAck = 91,
  QueryStatus = 100,
  QueryStatusAck = 101,
  Revalidate = 110,
  RevalidateAck = 111,
  Goodbye = 120,
  GoodbyeAck = 121,
};

bool is_valid(MessageType value) noexcept;
std::string_view to_string(MessageType value) noexcept;
/// True for the acknowledgement half of a request/response pair. This is an
/// explicit table, never a numbering trick: every request type has exactly one
/// acknowledgement and every acknowledgement exactly one request.
bool is_acknowledgement(MessageType value) noexcept;
/// True for the request half of a request/response pair.
bool is_request(MessageType value) noexcept;
/// Sentinel returned when a message type has no counterpart. It is not a valid
/// message type, so a caller that ignores the check fails at the next validation
/// rather than acting on a wrong type.
inline constexpr MessageType kNoMessageType = static_cast<MessageType>(0);

/// The acknowledgement that answers \p request. Returns kNoMessageType for an
/// acknowledgement or unknown input, which the caller must reject.
MessageType acknowledgement_for(MessageType request) noexcept;
/// The request that \p acknowledgement answers, or an invalid value.
MessageType request_for(MessageType acknowledgement) noexcept;

enum class FrameDecodeState : std::uint16_t {
  NeedMore = 1,
  Complete = 2,
  Error = 3,
};

struct FrameHeader {
  std::uint16_t version = kWireProtocolVersion;
  MessageType type = MessageType::Hello;
  std::uint16_t flags = 0;
  std::uint32_t payload_length = 0;
};

struct Frame {
  FrameHeader header{};
  std::vector<std::uint8_t> payload{};
};

/// Encodes one frame: magic, version, type, flags, reserved, length, header CRC,
/// payload, payload CRC. Refuses a payload above the frame bound.
Status encode_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                    std::vector<std::uint8_t>& out, std::uint16_t flags = 0,
                    std::uint32_t max_payload = kMaxFramePayload);

/// Incremental frame decoder. Total: every byte string either yields complete
/// frames or a typed error. Sticky: after the first error every call fails and
/// no further byte is interpreted.
class FrameReader {
 public:
  explicit FrameReader(std::uint32_t max_payload = kMaxFramePayload);
  ~FrameReader();
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;

  /// Appends received bytes. Refuses to buffer beyond the declared bound.
  Status push(const std::uint8_t* data, std::size_t size);
  /// Extracts the next complete frame. Returns NotFound while more bytes are
  /// needed, and the sticky decode error once the stream is poisoned.
  Result<Frame> next();

  [[nodiscard]] bool failed() const noexcept { return error_ != ErrorCode::Ok; }
  [[nodiscard]] ErrorCode error() const noexcept { return error_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - offset_; }
  [[nodiscard]] std::uint32_t max_payload() const noexcept { return max_payload_; }
  void reset() noexcept;

 private:
  std::vector<std::uint8_t> buffer_{};
  std::size_t offset_ = 0;
  std::uint32_t max_payload_;
  ErrorCode error_ = ErrorCode::Ok;
};

/// Blocking socket wrapper.
///
/// A socket that owns a wakeup channel waits for readability in select() rather
/// than blocking inside recv(); release_blocked_io() sends one datagram to that
/// channel, which is the documented way to release a blocked read from another
/// thread. The descriptor and the wakeup descriptor are both exchanged
/// atomically under one lifecycle lock, so a release can never touch a
/// descriptor that was already closed and recycled.
class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  /// One-time process-wide transport initialisation. Idempotent.
  static Status startup();
  static void teardown();

  static Result<Socket> connect_to(const std::string& host, std::uint16_t port);

  [[nodiscard]] bool valid() const noexcept { return descriptor_.load() >= 0; }
  Status write_all(const std::uint8_t* data, std::size_t size);
  /// Reads up to \p size bytes. A zero read means the peer closed cleanly.
  Status read_some(std::uint8_t* data, std::size_t size, std::size_t& read_out);
  /// Creates the wakeup channel this socket's blocking reads wait on. Must be
  /// called before the socket is read from another thread.
  Status open_wakeup_channel();
  /// Releases any blocked read immediately. Safe from any thread, and mutually
  /// exclusive with close(): the two share a lock, so a release can never touch
  /// a descriptor that was already closed and recycled.
  Status release_blocked_io() noexcept;
  Status close();
  /// Closes without constructing a Status, for destructors: a destructor must
  /// not throw and has nothing to report a failure to.
  void close_quietly() noexcept;
  [[nodiscard]] bool closed_by_owner() const noexcept;

 private:
  friend class Listener;
  explicit Socket(std::intptr_t descriptor) noexcept : descriptor_(descriptor) {}
  Status wait_readable(bool& woken);

  std::atomic<std::intptr_t> descriptor_{-1};
  std::atomic<std::intptr_t> wakeup_{-1};
  std::uint8_t wakeup_address_[16] = {};
  int wakeup_address_length_ = 0;
  mutable std::mutex lifecycle_mutex_{};
};

/// Blocking loopback listener.
///
/// The accept path never blocks inside accept() itself. It waits in select() on
/// two descriptors: the listening socket and a loopback datagram socket that
/// exists only to be woken. release_blocked_io() sends one datagram, which makes
/// the wait return immediately on every supported platform. Closing a listening
/// socket does *not* reliably wake a blocked accept, so relying on close() alone
/// would leave a shutdown waiting on the operating system.
class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  static Result<Listener> bind_loopback(std::uint16_t port, int backlog = 32);
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_.load() >= 0; }
  /// Waits for a pending connection, then accepts it. Returns Shutdown when the
  /// listener was released instead of a connection arriving.
  Status accept(Socket& out);
  /// Wakes a blocked accept immediately and releases any pending socket I/O.
  Status release_blocked_io() noexcept;
  Status close();

 private:
  Status wait_readable(bool& woken);

  std::atomic<std::intptr_t> descriptor_{-1};
  std::atomic<std::intptr_t> wakeup_{-1};
  std::uint8_t wakeup_address_[16] = {};
  int wakeup_address_length_ = 0;
  std::uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// Protocol bodies
// ---------------------------------------------------------------------------

struct HelloRequest {
  std::uint32_t protocol_version = kWireProtocolVersion;
  ProcessId process{};
  BootIncarnation boot{};
  PrincipalId principal{};
  std::string token{};
  std::string role{};

  [[nodiscard]] Status validate() const;
};

struct HelloResponse {
  SessionId session{};
  CoordinatorTerm term{};
  BootIncarnation boot{};
  /// Current generation set. A component is left unset when the subsystem that
  /// owns it has not been configured yet; the client must treat an unset
  /// component as "not established", never as a matching generation.
  AuthorityVector authority{};
  std::string detail{};

  [[nodiscard]] Status validate() const;
};

struct RequestEnvelope {
  SessionId session{};
  SequenceNumber request_sequence{};
  CoordinatorTerm term{};
  BootIncarnation boot{};

  [[nodiscard]] Status validate() const;
};

struct ResponseEnvelope {
  SessionId session{};
  SequenceNumber request_sequence{};
  CoordinatorTerm term{};
  BootIncarnation boot{};
  ErrorCode code = ErrorCode::Ok;
  std::string detail{};
};

void encode(ByteWriter& writer, const HelloRequest& value);
Status decode(ByteReader& reader, HelloRequest& value);
void encode(ByteWriter& writer, const HelloResponse& value);
Status decode(ByteReader& reader, HelloResponse& value);
void encode(ByteWriter& writer, const RequestEnvelope& value);
Status decode(ByteReader& reader, RequestEnvelope& value);
void encode(ByteWriter& writer, const ResponseEnvelope& value);
Status decode(ByteReader& reader, ResponseEnvelope& value);

/// Response bodies.
struct GrantBody {
  Grant grant{};
};
struct DecisionBody {
  Decision decision{};
  AllocationPlan plan{};
};
struct StatusBody {
  AuthorityVector authority{};
  BootIncarnation boot{};
  CoordinatorTerm term{};
  EvidenceGeneration evidence_generation{};
  CapacityGeneration capacity_generation{};
  FabricGeneration fabric_generation{};
  std::uint64_t live_grants = 0;
  std::uint64_t retained_grants = 0;
  std::uint64_t retained_fences = 0;
  std::uint64_t retained_decisions = 0;
  std::uint64_t retained_contracts = 0;
  std::uint64_t journal_records = 0;
  std::uint64_t journal_bytes_in_segment = 0;
  bool accounting_closed = false;
  bool policy_installed = false;
  std::string policy_digest{};
  std::string recovery{};
  std::string recovery_detail{};
};
struct RestorationBody {
  RestorationEvaluation evaluation{};
};
struct RevalidateBody {
  std::uint64_t evaluated = 0;
  std::uint64_t fenced = 0;
  std::uint64_t expired = 0;
};

void encode(ByteWriter& writer, const GrantBody& value);
Status decode(ByteReader& reader, GrantBody& value);
void encode(ByteWriter& writer, const DecisionBody& value);
Status decode(ByteReader& reader, DecisionBody& value);
void encode(ByteWriter& writer, const StatusBody& value);
Status decode(ByteReader& reader, StatusBody& value);
void encode(ByteWriter& writer, const RestorationBody& value);
Status decode(ByteReader& reader, RestorationBody& value);
void encode(ByteWriter& writer, const RevalidateBody& value);
Status decode(ByteReader& reader, RevalidateBody& value);

/// Small request bodies shared by the runtime and its clients.
std::vector<std::uint8_t> encode_id_body(std::uint64_t id);
Status decode_id_body(const std::vector<std::uint8_t>& body, std::uint64_t& id);
std::vector<std::uint8_t> encode_attempt_body(std::uint64_t grant, std::uint64_t attempt);
Status decode_attempt_body(const std::vector<std::uint8_t>& body, std::uint64_t& grant,
                           std::uint64_t& attempt);
std::vector<std::uint8_t> encode_fence_body(std::uint64_t grant, FenceReason reason);
Status decode_fence_body(const std::vector<std::uint8_t>& body, std::uint64_t& grant,
                         FenceReason& reason);
/// Canonical single-object body helpers used by tools and tests.
template <class T>
std::vector<std::uint8_t> encode_object_body(const T& value) {
  ByteWriter writer;
  encode(writer, value);
  return writer.bytes();
}
/// Decodes a body that must contain exactly one object and nothing else.
template <class T>
Status decode_object_body(const std::vector<std::uint8_t>& body, T& value) {
  ByteReader reader(body);
  Status status = decode(reader, value);
  if (!status.ok()) return status;
  return decode_finish(reader);
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

/// Callbacks the runtime implements. Every method is invoked on the session's
/// own thread, never while a server-internal lock is held.
class SessionHandler {
 public:
  SessionHandler() = default;
  virtual ~SessionHandler() = default;
  SessionHandler(const SessionHandler&) = delete;
  SessionHandler& operator=(const SessionHandler&) = delete;

  /// Decides whether a connection becomes a session. The server has already
  /// minted \p session; the handler must not choose it. Returning a non-OK
  /// Status rejects the connection and the response carries that code.
  virtual Status on_hello(SessionId session, const HelloRequest& request,
                          HelloResponse& response) = 0;
  /// Handles one request. The server has already bound the session identity
  /// and the request sequence; the handler owns authority-vector validation.
  virtual Status on_request(SessionId session, const RequestEnvelope& envelope, MessageType type,
                            const std::vector<std::uint8_t>& body, ResponseEnvelope& response,
                            std::vector<std::uint8_t>& response_body) = 0;
  /// Called exactly once per accepted session, on the session thread.
  virtual void on_session_closed(SessionId session) = 0;
};

struct ServerConfig {
  /// Loopback only. DMF deliberately offers no way to bind elsewhere: the
  /// transport is plaintext and its authentication is a bearer-token check, so
  /// exposing it off-host would be a trust-boundary violation, not a feature.
  std::uint16_t port = 0;
  std::size_t max_sessions = 64;
  std::uint32_t max_frame_payload = kMaxFramePayload;
  std::size_t max_requests_per_session = 1U << 20;
  /// Pending-connection queue depth passed to listen().
  int backlog = 32;
};

/// Threaded framed-protocol server. One accept thread plus one thread per
/// session, bounded by max_sessions.
class Server {
 public:
  /// Opaque implementation type. Declared publicly so the session and accept
  /// loops can be free functions; its definition stays in the translation unit.
  struct Impl;

  Server(ServerConfig config, SessionHandler& handler, Accounting* accounting = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Binds and starts accepting. Returns the bound port through port().
  Status start();
  /// Releases blocked accepts and reads, then joins every thread. Idempotent.
  Status stop();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::size_t session_count() const;

 private:
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
  std::atomic<bool> running_{false};
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct ClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  ProcessId process{};
  BootIncarnation boot{};
  PrincipalId principal{};
  std::string token{};
  std::string role = "operator";
  std::uint32_t max_frame_payload = kMaxFramePayload;
};

/// Single-connection client. Not thread safe: one call at a time.
class Client {
 public:
  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Status connect(const ClientConfig& config);
  [[nodiscard]] Status handshake(HelloResponse& response);
  [[nodiscard]] Result<std::vector<std::uint8_t>> call(MessageType type,
                                                       const std::vector<std::uint8_t>& body);
  Status close();
  [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }
  [[nodiscard]] SessionId session() const noexcept { return session_; }
  [[nodiscard]] CoordinatorTerm term() const noexcept { return term_; }
  [[nodiscard]] BootIncarnation boot() const noexcept { return boot_; }

 private:
  Socket socket_{};
  FrameReader reader_{};
  ClientConfig config_{};
  SessionId session_{};
  CoordinatorTerm term_{};
  BootIncarnation boot_{};
  SequenceNumber request_sequence_{};
};

}  // namespace dmf

#endif  // DMF_NET_HPP