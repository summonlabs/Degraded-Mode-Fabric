# Degraded Mode Fabric - Protocol

This document specifies the framed control protocol implemented in
`src/net/frame.cpp`, `src/net/messages.cpp`, `src/net/socket.cpp`,
`src/net/server.cpp` and `src/net/client.cpp`, with declarations in
`include/dmf/net.hpp`. Byte layouts are exhaustive for the framing and for the
envelope and small bodies; the large bodies are defined by the canonical codec in
`src/codec/codec.cpp` and are summarised with their field order.

## 1. Framing

One frame is exactly `20 + payload_length + 4` bytes. All integers are
little-endian and fixed width.

```
offset  size  field
------  ----  ---------------------------------------------------------------
0       4     magic, ASCII "DMFF" (0x44 0x4D 0x46 0x46)
4       2     version, u16, must equal kWireProtocolVersion (1)
6       2     type, u16, must be a defined MessageType
8       2     flags, u16; the encoder refuses a non-zero value, the decoder
              rejects one
10      2     reserved, u16; must be 0, a non-zero value is Corrupt
12      4     payload_length, u32
16      4     header CRC: CRC-32C over bytes [0, 16)
20      N     payload (N = payload_length)
20+N    4     payload CRC: CRC-32C over the payload bytes [20, 20+N)
```

Constants (`include/dmf/net.hpp`):

```
kFrameHeaderSize   20
kFrameTrailerSize  4
kMaxFramePayload   1 << 20   (1,048,576 bytes), the default and the hard ceiling
kSocketReadChunk   1 << 16   (65,536 bytes)
```

Encoder rules (`encode_frame`):

* the message type must be defined, otherwise `ErrorCode::InvalidEnum`;
* `payload.size() > max_payload` is `ErrorCode::CapacityExceeded`;
* a non-zero `flags` argument is `ErrorCode::InvalidArgument`;
* the writer itself is bounded by header + payload + trailer; overflowing it is
  `CapacityExceeded`.

Decoder rules (`FrameReader`) - the decoder is total (every byte string yields
frames or a typed error) and sticky (after the first error every later call fails
and no further byte is interpreted):

| Condition | Error |
| --- | --- |
| fewer than 20 bytes buffered | `NotFound` (need more) |
| insufficient for the declared payload and trailer | `NotFound` (need more) |
| magic mismatch | `Corrupt` |
| header CRC mismatch | `IntegrityFailure` |
| version mismatch | `VersionUnsupported` |
| undefined message type | `InvalidEnum` |
| non-zero flags | `Invalid` |
| non-zero reserved field | `Corrupt` |
| declared payload above the negotiated bound | `CapacityExceeded` |
| length arithmetic overflow | `Overflow` |
| payload CRC mismatch | `IntegrityFailure` |
| buffered bytes above `max_payload + 20 + 4 + kSocketReadChunk` | `CapacityExceeded` |

`FrameReader::reset()` clears the buffer and the sticky error; `Client::connect`
calls it. A `max_payload` of 0 or above `kMaxFramePayload` is clamped to
`kMaxFramePayload`.

## 2. Message types

The numbering is part of the wire contract. Requests are the even members of each
ten-block and acknowledgements are the request number plus one; `Hello`/`HelloAck`
are 1/2.

| Number | Name | Direction | Required role | Request body |
| --- | --- | --- | --- | --- |
| 1 | `HELLO` | client to server | none (handshake) | `HelloRequest` |
| 2 | `HELLO_ACK` | server to client | - | `ResponseEnvelope` + `HelloResponse` |
| 10 | `PUBLISH_EVIDENCE` | client | Operator | `EvidenceItem` |
| 11 | `PUBLISH_EVIDENCE_ACK` | server | - | envelope only |
| 20 | `REGISTER_CONTRACT` | client | Administrator | `ServiceContract` |
| 21 | `REGISTER_CONTRACT_ACK` | server | - | envelope only |
| 30 | `INSTALL_POLICY` | client | Administrator | `Policy` |
| 31 | `INSTALL_POLICY_ACK` | server | - | envelope only |
| 40 | `EVALUATE_CONTRACT` | client | Observer | `u64` contract id |
| 41 | `EVALUATE_CONTRACT_ACK` | server | - | `Decision` + `AllocationPlan` |
| 50 | `ACQUIRE_AUTHORITY` | client | Operator | `u64` contract id |
| 51 | `ACQUIRE_AUTHORITY_ACK` | server | - | `Grant` |
| 60 | `ACKNOWLEDGE_GRANT` | client | Operator | `u64` grant, `u64` attempt |
| 61 | `ACKNOWLEDGE_GRANT_ACK` | server | - | `Grant` |
| 70 | `REPORT_APPLIED` | client | Operator | `u64` grant, `u64` attempt |
| 71 | `REPORT_APPLIED_ACK` | server | - | `Grant` |
| 80 | `REQUEST_RESTORATION` | client | Operator | `u64` grant id |
| 81 | `REQUEST_RESTORATION_ACK` | server | - | `RestorationEvaluation` |
| 90 | `FENCE_GRANT` | client | Operator | `u64` grant, `u16` FenceReason |
| 91 | `FENCE_GRANT_ACK` | server | - | envelope only |
| 100 | `QUERY_STATUS` | client | Observer | empty (a non-empty body is `InvalidArgument`) |
| 101 | `QUERY_STATUS_ACK` | server | - | `StatusBody` |
| 110 | `REVALIDATE` | client | Observer | empty |
| 111 | `REVALIDATE_ACK` | server | - | `evaluated, fenced, expired` (3 x u64) |
| 120 | `GOODBYE` | client | Observer | empty (a non-empty body is `InvalidArgument`) |
| 121 | `GOODBYE_ACK` | server | - | envelope only |

Pairing is explicit, not arithmetic: `is_acknowledgement(type)` is a switch over
the thirteen acknowledgement types, `is_request(type)` is
`is_valid(type) && !is_acknowledgement(type)`, `acknowledgement_for(request)`
maps a request to its acknowledgement and `request_for(acknowledgement)` maps
back. `Server::response_type` delegates to `acknowledgement_for`;
`Client::call` refuses a non-request type and verifies that the reply is exactly
`acknowledgement_for(request)`. An unknown or acknowledgement input to
`acknowledgement_for`/`request_for` returns `kNoMessageType` (value 0), which
is not a valid message type, so a caller that ignores the check fails at the next
validation rather than acting on a wrong type.

### Small fixed bodies

```
HelloRequest        u32 protocol_version   (must equal 1)
                    u64 process            (ProcessId, must be set)
                    u64 boot               (BootIncarnation, must be set)
                    u64 principal          (PrincipalId, must be set)
                    blob token             (u32 length + bytes, <= 256 bytes)
                    blob role              (u32 length + bytes, <= 64 bytes)
                    token and role must contain no NUL, no control characters
                    other than tab, and no DEL

HelloResponse       u64 session            (must be set)
                    u64 term               (must be set)
                    u64 boot               (must be set)
                    AuthorityVector        6 x u64 (term, boot, fabric, capacity,
                                           policy, evidence)
                    blob detail

RequestEnvelope     4 x u64: session, request_sequence, term, boot   (32 bytes)
ResponseEnvelope    4 x u64 as above
                    u16 code               (ErrorCode, domain checked on decode)
                    blob detail

id body             u64                                              (8 bytes)
attempt body        u64 grant, u64 attempt                          (16 bytes)
fence body          u64 grant, u16 FenceReason                       (10 bytes)
RevalidateBody      u64 evaluated, u64 fenced, u64 expired           (24 bytes)
```

`HelloResponse::validate()` deliberately does not require the authority vector to
be complete: an unset component (for example an unset policy generation because
no policy is installed) is reported truthfully, and the client must treat it as
"not established" rather than as a matching generation. `ResponseEnvelope`
decodes its code with `read_enum<ErrorCode>`, so an undefined code poisons the
reader with `InvalidEnum` instead of being reinterpreted as a legal error.

### Large bodies

```
GrantBody           Grant
DecisionBody        Decision, then AllocationPlan
RestorationBody     RestorationEvaluation
StatusBody          AuthorityVector
                    12 x u64: boot, term, evidence_generation,
                              capacity_generation, fabric_generation,
                              live_grants, retained_grants, retained_fences,
                              retained_decisions, retained_contracts,
                              journal_records, journal_bytes_in_segment
                    2 x u8:   accounting_closed, policy_installed
                    3 blobs:  policy_digest, recovery, recovery_detail
```

`Decision`, `AllocationPlan`, `Grant`, `RestorationEvaluation`,
`StatusBody`'s nested codecs and every other object use the canonical codec:

* every field is written in declaration order as little-endian fixed width;
* an enum is written as `u16` and validated against its domain on decode; an
  invalid value poisons the reader with `InvalidEnum` and is never reinterpreted;
* a string is a `u32` length followed by that many bytes, bounded by
  `kMaxBlobBytes` (1 MiB) on both sides;
* a collection is a `u32` count validated against its per-collection bound
  (`kMaxGuaranteesPerSet` 16, `kMaxEnvelopeBounds` 16, `kMaxDeltaEntries` 32,
  `kMaxEvidenceItems` 65536, `kMaxReasonsPerDecision` 32,
  `kMaxSupportEntries` 16, `kMaxPreconditions` 32, `kMaxPlanEntries` 1,048,576)
  *before* the elements are allocated;
* a boolean is one byte and must be 0 or 1;
* `decode_finish()` requires the reader to be at the end, so a body with trailing
  bytes is `TrailingGarbage`.

Decoders are not `noexcept`. They materialise values, so an allocation failure
propagates as an exception rather than terminating the process; every other
failure mode is a returned `Status`. Encoders keep their `noexcept` guarantee
because the writer checks every bound before it writes.

The store and the wire share this codec, so a durable record payload and a
protocol body are byte-identical for equal values. See `docs/PERSISTENCE.md` for
the framing around record payloads.

## 3. Session binding, transport release and replay rules

* The server mints the session identity (`next_session` starts at 1 and is
  allocated atomically); the handler is told the identity and cannot choose it.
  The handshake response always carries the server-minted value, overwriting
  anything the handler filled in.
* A connection must complete `HELLO` before any other frame. Any frame before a
  completed handshake closes the session with no response
  (`requests_rejected` is incremented).
* A second `HELLO` on an established session is `InvalidState`, is answered with
  the error code and closes the session.
* Every request envelope must carry the bound session identity
  (`envelope.session == state->id`); a mismatch is `Unauthorized` and fatal: the
  error is returned and the session closes.
* The request sequence must strictly increase
  (`envelope.request_sequence <= last_sequence` is `ReplayDetected`, fatal).
  The check is per session and in memory; it is not durable, and the sequence is
  not used to deduplicate across reconnects.
* A per-session request quota (`ServerConfig::max_requests_per_session`,
  default 1,048,576) yields `QuotaExceeded` and closes the session.
* The envelope must carry the current coordinator term and boot incarnation;
  `Coordinator::on_request` returns `ErrorCode::Stale` on either mismatch. This
  is a per-request check, so a client that reconnects after a restart must
  re-handshake.
* The handler must return OK for a request to be answered with the request's own
  error code; a handler status is copied into `ResponseEnvelope::code` and
  `detail`. A failed handler does not by itself close the session, except for the
  fatal conditions listed above and for `GOODBYE`.
* `Client::call` validates that the reply type is the expected acknowledgement,
  that the reply session equals the bound session (`Unauthorized`), that the reply
  sequence equals the request sequence (`SequenceRegression`), and then returns
  the bytes after the envelope as the body.
* `Client::close` attempts a `GOODBYE` call and ignores its status before closing
  the socket.

### Releasing blocked I/O

Closing a socket does not reliably wake a blocked `accept` or `recv` on every
supported platform, and a blocked `recv` released with `shutdown()` delayed
thread termination by 120 seconds on Windows. Both `Listener` and `Socket`
therefore carry an explicit loopback-datagram wakeup channel:

* `Listener::bind_loopback` binds a loopback UDP socket to an ephemeral port
  alongside the listening socket; `Listener::accept` waits in `select()` on the
  listening descriptor and the wakeup descriptor, and only then calls `accept`.
* `Socket::open_wakeup_channel()` creates the same channel for a session socket.
  `Server`'s accept loop calls it for every accepted connection before the
  session thread is created; a socket without a channel reads with a blocking
  `recv` (which is what the single-threaded `Client` uses).
* `release_blocked_io()` sends exactly one datagram, which makes the wait return
  immediately. `Socket::release_blocked_io()` no longer calls `shutdown`;
  `Listener::release_blocked_io()` sends the datagram and also shuts the
  listening socket down.
* `Server::stop()` publishes `stopping`, drains the session registry under its
  registry lock, releases the listener, releases and joins every session, closes
  the listener, joins the accept thread, and drains and joins once more to catch a
  session that registered while the first drain was running. No thread is joined
  while it waits on a resource the joiner controls.

## 4. Role model

```
PrincipalRole   Observer = 1   Operator = 2   Administrator = 3   (ordered)
```

A session's role is a property of the bearer token it presented, never of a claim
it made. On `HELLO`, `Coordinator::on_hello` resolves the role:

* if `CoordinatorConfig::allow_anonymous_sessions` is true, the session is
  admitted as Administrator and the token is not checked (documented as a
  single-process test facility, never a deployment setting);
* otherwise the presented token is compared with every entry of
  `CoordinatorConfig::principals` (token string to `PrincipalRole`) using
  `constant_time_equals`, and every configured token is compared so the
  comparison cost does not reveal which token, if any, matched; the matching
  entry supplies the role;
* an empty `principals` map refuses every handshake (`Unauthorized`,
  "coordinator has no principal configured"), and a token that matches no entry is
  refused (`Unauthorized`, "bearer token was rejected").

The `role` string in `HelloRequest` is a label only. It is validated for length
(`kMaxRoleBytes` = 64) and text safety and is then ignored for authorisation: no
code path derives a `PrincipalRole` from it. The `principal` identity is
likewise carried and validated but not used for authorisation in this revision.

`required_role(type)`:

| Role | Messages |
| --- | --- |
| Administrator | `REGISTER_CONTRACT`, `INSTALL_POLICY` |
| Operator | `PUBLISH_EVIDENCE`, `ACQUIRE_AUTHORITY`, `ACKNOWLEDGE_GRANT`, `REPORT_APPLIED`, `REQUEST_RESTORATION`, `FENCE_GRANT` |
| Observer | `EVALUATE_CONTRACT`, `QUERY_STATUS`, `REVALIDATE`, `GOODBYE`, and every type not listed above |

The role is recorded per session at handshake and compared numerically; a request
whose session has no recorded role is `Unauthorized`.

## 5. Trust boundary

The transport provides neither confidentiality nor cryptographic authentication:

* Frames are plaintext on a TCP connection. There is no TLS, no session key, no
  message authentication code and no signature anywhere in `src/net/`.
* The bearer token is a capability check, not an authentication protocol. It
  gates admission, and it now also selects the role, so authorisation is only as
  strong as the secrecy of the configured token: anyone who knows the
  administrator token is an administrator.
* The server always binds to loopback: `Listener::bind_loopback` sets
  `sin_addr.s_addr = htonl(INADDR_LOOPBACK)`. `ServerConfig` has no host field,
  so there is no configuration in the public surface that binds elsewhere. The
  protocol is a local control channel by construction; it is not hardened for
  exposure.
* Frame CRCs detect corruption, not forgery: an attacker who can write to the
  connection can compute valid CRCs.
* On Windows the transport performs `WSAStartup(MAKEWORD(2,2))` once; on POSIX it
  ignores `SIGPIPE` once and uses `MSG_NOSIGNAL` on send so a vanished peer
  surfaces as an error rather than a signal.

## 6. Bounds and refusal rules

```
ServerConfig
  port                   0 means an ephemeral port, reported through port()
  max_sessions           64
  max_frame_payload      kMaxFramePayload (1 MiB)
  max_requests_per_session 1,048,576
  backlog                32, passed to listen()

ClientConfig
  host                   127.0.0.1
  port
  process, boot, principal, token
  role                   "operator" (a label; see section 4)
  max_frame_payload      kMaxFramePayload
```

`ServerConfig` has no bind-host field: loopback-only is a design decision, not a
default. `backlog` is used, not vestigial: `Server::start` passes it to
`Listener::bind_loopback`, which calls `listen(socket, backlog > 0 ? backlog : 1)`.

Refusals and their codes:

| Situation | Code |
| --- | --- |
| hello carries a wrong protocol version | `VersionUnsupported` |
| hello missing process, boot or principal; token > 256 bytes; role > 64 bytes; unsafe text | `InvalidArgument` / `CapacityExceeded` |
| token mismatch, or no principal configured | `Unauthorized` |
| frame before handshake | session closed, no response |
| second handshake | `InvalidState` |
| session identity mismatch | `Unauthorized` (fatal) |
| non-increasing request sequence | `ReplayDetected` (fatal) |
| request quota exhausted | `QuotaExceeded` (fatal) |
| stale term or boot | `Stale` |
| role below the required role, or no role recorded | `Unauthorized` |
| body that does not decode, or trailing bytes | the decoder's code (`Truncated`, `InvalidEnum`, `TrailingGarbage`, ...) |
| body for `QUERY_STATUS` or `GOODBYE` that is not empty | `InvalidArgument` |
| frame decode failure | session closed; counted as rejected (oversize separately) |
| message type valid but not handled by the coordinator | `Unsupported` |
| oversize frame or frame buffer | `CapacityExceeded` |
| more live sessions than `max_sessions` | connection closed, `sessions_rejected` incremented, no response |

## 7. Numbering invariants

The numbering is not algorithmic, and the code does not assume that it is:

* requests and acknowledgements are paired by an explicit table
  (`acknowledgement_for`/`request_for`), not by arithmetic on the type value;
* `is_acknowledgement(type)` is an explicit switch over the thirteen
  acknowledgement types, so a new message type must be added to the enum,
  `is_valid`, `is_acknowledgement`, `acknowledgement_for` and `request_for`;
* a message type that is valid but carries no handler in
  `Coordinator::on_request` is answered with `Unsupported`;
* the request/acknowledgement spacing (even request, odd acknowledgement, in
  blocks of ten, with `HELLO`/`HELLO_ACK` at 1/2) is documentation only. Nothing
  in the protocol derives a message number from another message number.
