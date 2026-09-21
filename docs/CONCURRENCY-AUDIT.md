# Degraded Mode Fabric - Concurrency Audit

This is a documented audit of the concurrency, ownership and teardown behaviour
of the runtime. Each item states the mechanism, the evidence in the tree, and the
verdict: SAFE (the mechanism rules the hazard out), FIXED (a concrete change in
the tree removes the hazard) or RESIDUAL (a real hazard that is not fixed; the
required action is stated).

Basis: a full read of `src/net/server.cpp`, `src/net/socket.cpp`,
`src/net/frame.cpp`, `src/net/client.cpp`, `src/runtime/coordinator.cpp`,
`src/runtime/accounting.cpp`, `src/runtime/evidence_store.cpp`,
`src/runtime/grant_table.cpp`, `src/store/*.cpp` and the matching headers, plus a
repository-wide search for every lock, atomic and thread. The working tree has no
commits yet, so mechanisms are cited by file and function rather than by line
number. Items 6, 9, 11, 13, 14 and 18 record repairs that were made in the tree;
the defects they close are listed in `docs/TESTING.md`.

Lock inventory (the complete set of thread locks):

```
Coordinator::Impl::mutex      std::mutex, non-recursive, mutable   one per coordinator
Server::Impl::mutex           std::mutex, non-recursive, mutable   one per server
Accounting::mutex_            std::mutex, non-recursive, mutable   one per accounting facade
Socket::lifecycle_mutex_      std::mutex, non-recursive, mutable   one per socket
atomics                       Socket and Listener descriptor and wakeup channel,
                              Server stopping/running/next_session/active_sessions,
                              testkit temp counter
threads                       one accept thread + one thread per session in Server
```

There is no `std::shared_mutex`, no `std::recursive_mutex`, no condition
variable, no future/promise, and no detached thread anywhere in the repository.
Separately from these thread locks, a writable `StateStore` holds an
operating-system file lock on `<root>/store.lock` for its whole lifetime (item
18); it is not a C++ mutex and never nests with one.

---

## 1. Read-lock then write-lock re-entry on the same mutex

**Mechanism.** There is no shared/reader lock in the code base at all. The
coordinator serialises every public entry point with one non-recursive
`std::mutex` taken through `std::lock_guard`; `Coordinator::start()` is the only
site that uses `std::unique_lock`, and only to release the lock around
`Server::start()` before re-acquiring it.

**Evidence.** `src/runtime/coordinator.cpp` (`struct Coordinator::Impl`,
`Coordinator::install_policy` and the other public methods, `Coordinator::start`),
`src/runtime/accounting.cpp`, `src/net/server.cpp`, `src/net/socket.cpp`.

**Verdict: SAFE.** A read-then-write upgrade cannot occur because no read lock
exists; a re-entrant lock cannot occur because no lock is recursive and no
public method is reachable from inside a locked region (see item 2).

## 2. Write lock held across helper and callback paths that re-enter state

**Mechanism.** Every public coordinator method takes the mutex once and delegates
to a `*_locked` helper (`evaluate_locked`, `acquire_locked`,
`publish_evidence_locked`, `install_policy_locked`, `register_contract_locked`,
`acknowledge_locked`, `report_applied_locked`, `request_restoration_locked`,
`sweep_locked`, `fence_grant_locked`, `describe_locked`). Nested calls stay
inside that family - for example `acquire_locked` calls `evaluate_locked`, and
`install_policy_locked` calls `sweep_locked`, which calls
`fence_grant_locked` - so the lock is taken exactly once per external call.
No `*_locked` helper calls a public method of `Coordinator`.

**Evidence.** `src/runtime/coordinator.cpp`, and the locking contract comment at
the top of that file.

**Verdict: SAFE.** One deliberate cost is recorded: the lock is held across
store I/O, because every committed mutation goes through
`append_record` -> `StateStore::append` -> `write` + `flush` before the caller
sees a result. A slow disk therefore blocks every other coordinator entry point.
That is the price of "a committed mutation is durable before it is reported"; it
is a design decision, not a defect.

## 3. Event, log and callback invocation beneath internal locks

**Mechanism.** The server invokes the `SessionHandler` (`on_hello`,
`on_request`, `on_session_closed`) from the session's own thread, in
`run_session`/`session_loop`, where no server lock is held. `Server::Impl::mutex`
is taken in exactly two places: the registration push in `accept_loop` and the
`swap` in `drain_sessions`. Neither calls the handler. `session_loop` wraps the
whole session body in a `try/catch(...)`: a handler that throws ends its session
and is counted as a rejected request instead of unwinding through the thread and
calling `std::terminate`. The coordinator has no callback surface at all: the
only virtual methods it implements are the handler methods, and nothing inside a
locked region invokes user code. No logging or `std::cout` appears inside a
locked region.

**Evidence.** `src/net/server.cpp` (`accept_loop`, `drain_sessions`,
`run_session`, `session_loop`), `include/dmf/net.hpp`
("Every method is invoked on the session's own thread, never while a
server-internal lock is held"), `src/runtime/coordinator.cpp`.

**Verdict: SAFE.**

## 4. Joining workers while holding state they need

**Mechanism.** `Coordinator::stop()` sets `impl_->stopped` under the state mutex,
**releases it**, and only then calls `Server::stop()`. A session thread finishing
its last request runs `session_loop`, which calls
`handler->on_session_closed(id)` -> `Coordinator::on_session_closed`, which takes
that same state mutex. Because the joiner does not hold it, the join cannot
deadlock. The comment in `Coordinator::stop()` states this explicitly.
`Server::stop()` itself never holds `Server::Impl::mutex` while joining: it
drains the registry into a local vector under the lock, then releases, then
releases blocked I/O and joins.

**Evidence.** `src/runtime/coordinator.cpp` (`Coordinator::stop`),
`src/net/server.cpp` (`drain_sessions`, `release_and_join`, `Server::stop`,
`session_loop`).

**Verdict: SAFE (fixed by design).** Any future code that takes the coordinator
lock around `server->stop()` reintroduces the deadlock.

## 5. Cancellation and shutdown with reversed lock ordering

**Mechanism.** The lock graph has one internal edge: a thread holding
`Coordinator::Impl::mutex` may take `Accounting::mutex_` (every
`record_*` call, including from the server threads, which report frame and
session activity through the same facade, and from `describe_locked` ->
`check_closure`). `Accounting::mutex_` never acquires another lock, so it is a
leaf. Session and accept threads take the same leaf without holding anything.
`Socket::lifecycle_mutex_` is also a leaf (taken by `close`,
`release_blocked_io` and the move operations). `Server::Impl::mutex` is taken
alone around vector operations. No path takes two of the four mutexes in opposite
orders. The `LockFile` in `StateStore` adds no edge at all: it is an
operating-system lock acquired once in `open` and released in `close`, with no
C++ mutex involved.

**Evidence.** Lock sites in `src/runtime/coordinator.cpp`,
`src/runtime/accounting.cpp`, `src/net/server.cpp`, `src/net/socket.cpp`,
`src/store/state_store.cpp`; shutdown paths in `Coordinator::stop` and
`Server::stop`.

**Verdict: SAFE**, conditional on `Accounting` remaining a leaf. If an accounting
hook ever calls back into the coordinator or the server, this becomes a genuine
inversion; keep the accounting callbacks out.

## 6. Blocked socket and thread teardown

**Mechanism.** Closing a descriptor does not reliably wake a blocked `accept`,
and releasing a blocked `recv` with `shutdown()` left the Windows per-thread
socket state in a condition that delayed thread termination by about 120 seconds.
Both blocking wait paths therefore wait in `select()` on two descriptors: the
I/O descriptor and a loopback datagram wakeup channel
(`Listener::bind_loopback` creates one beside the listening socket;
`Socket::open_wakeup_channel` is called by the server's accept loop for every
accepted connection before its thread is created). `release_blocked_io()` sends
exactly one datagram, which makes the wait return immediately on every supported
platform. A `Socket` without a wakeup channel (the single-threaded `Client`)
keeps a plain blocking `recv`.

`Server::stop()` orders teardown so that no thread is ever joined while it waits
on a resource the joiner controls:

1. publish `stopping` (an atomic) and clear `running`;
2. swap the session registry out under the registry lock (no I/O under the lock);
3. release the listener's blocked `accept` with `Listener::release_blocked_io()`
   (a wakeup datagram plus a `shutdown`), which makes the accept loop exit;
4. release every session's blocked read/write with
   `Socket::release_blocked_io()`, then join each session thread;
5. close the listener and join the accept thread;
6. drain and join once more, to catch a session that registered while the first
   drain was running.

Each session thread closes its own socket in `session_loop` ("teardown runs on
this thread only: it is the sole owner of the descriptor"), so `stop()` releases
but never closes a session descriptor. `Socket::release_blocked_io()` and
`Socket::close()` share `lifecycle_mutex_`, which is what makes the release
safe against a concurrent owner close.

**Evidence.** `src/net/server.cpp` (`Server::stop`, `release_and_join`,
`accept_loop`, `session_loop`), `src/net/socket.cpp` (`open_wakeup_channel`,
`wait_readable`, `release_blocked_io`, `close`, `lifecycle_mutex_`),
`include/dmf/net.hpp`.

**Verdict: FIXED.** The descriptor is exchanged atomically in `close()` so a
double close is impossible, the shared `lifecycle_mutex_` closes the window in
which a release could touch a recycled descriptor number, and the wakeup channel
removes the shutdown-based stall: the concurrency suite's shutdown case and the
protocol suite's release case complete within their explicit bound rather than
after a multi-minute delay.

**Residual:** `Listener` has no equivalent lifecycle mutex; its
`release_blocked_io()` and `close()` use `descriptor_.load()` and
`descriptor_.exchange(-1)` without sharing a lock. A double close is still
impossible (the exchange makes the second call a no-op), and the
shutdown-after-close window can only be hit if two threads call `Server::stop()`
concurrently on the same `Server`.

**Header-comment note:** the class comment above `Socket` in
`include/dmf/net.hpp` still says that "shutdown() is safe to call from another
thread and is the documented way to release a blocked read or write". That is a
stale comment: there is no public `shutdown()` method, and
`Socket::release_blocked_io()` now sends only the wakeup datagram. The behaviour
described in this item is the behaviour in the code.

## 7. Cross-object mutex order inversion

**Mechanism.** The only objects that own thread locks are `Coordinator::Impl`,
`Server::Impl`, `Accounting` and `Socket`. `Server` holds a pointer to the
coordinator as its handler and to the accounting facade, but never calls either
while holding `Server::Impl::mutex`. The coordinator calls the accounting facade
under its own lock (item 5). The coordinator calls `Server::stop()` only with its
lock released (item 4). Therefore the reachable order is
`Coordinator::mutex -> Accounting::mutex_` and nothing else, and the reverse
order does not exist.

**Evidence.** As in items 4-6.

**Verdict: SAFE.**

## 8. Moved-from handle ownership

**Mechanism.** `detail::FileHandle` (internal), `LockFile` (internal) and
`Socket`/`Listener` (public) are move-only RAII handles whose move constructor
and move assignment reset the source to the invalid value; the destructor closes
only a valid handle; move assignment closes the target's existing handle first.
`Socket` and `Listener` hold their descriptors in
`std::atomic<std::intptr_t>` and move by `exchange(kInvalidDescriptor)`;
`Socket` also moves its wakeup descriptor and address under
`lifecycle_mutex_`, so a moved-from handle is inert rather than aliasing.
`tests/testkit/process.hpp`'s `ChildProcess` follows the same rule for its
native handles.

**Evidence.** `src/store/file_io.cpp` (`~FileHandle`, move constructor, move
assignment; `LockFile` equivalents), `src/net/socket.cpp`
(`Socket::~Socket`, `Socket::Socket(Socket&&)`, `Socket::operator=`, the
`Listener` equivalents), `include/dmf/store.hpp`/`include/dmf/net.hpp`
(deleted copy operations), `tests/testkit/process.cpp`.

**Verdict: SAFE.** Move assignment is guarded by a self-assignment check; the
moved-from object is `valid() == false` and every operation on it returns
`InvalidState` or `Shutdown` rather than touching a stale descriptor.

## 9. Close and shutdown races, double close

**Mechanism.** `Socket::close()` and `Socket::~Socket()` are the only closers.
`close()` performs `descriptor_.exchange(kInvalidDescriptor)` and closes the
value it won, so exactly one thread can ever close a given descriptor; a second
call sees `-1` and returns OK. `release_blocked_io()` deliberately does not
invalidate or touch the I/O descriptor at all - it sends one datagram on the
wakeup channel - and it holds `lifecycle_mutex_` for that send, mutually
exclusive with `close()`. `close()` also exchanges and closes the wakeup
descriptor under the same lock. `Listener` uses the same exchange discipline for
`close()` and the destructor.

**Evidence.** `src/net/socket.cpp`, `include/dmf/net.hpp`.

**Verdict: FIXED for `Socket`** (shared `lifecycle_mutex_`, atomic exchange, and a
release path that never touches the I/O descriptor).
**Residual for `Listener`:** no shared lock between `release_blocked_io()` and
`close()`; harmless for a serialised `Server::stop()`, racy for two concurrent
stops. Required action: give `Listener` the same lifecycle mutex, or make
`Server::stop()` single-entry with a compare-exchange flag.

## 10. Callbacks retaining references to mutable state beyond the lock lifetime

**Mechanism.** The handler interface passes the request body as
`const std::vector<std::uint8_t>&` and the session identity by value; the
coordinator copies everything it needs into `ResponseEnvelope` and
`response_body` before releasing the lock, and the server writes only those
buffers afterwards. Values returned to callers (`Decision`, `Grant`,
`FenceRecord`, `RestorationEvaluation`, `StatusBody`, `AccountingCounters`)
are returned by value, so no interior pointer escapes.

**Verdict: FIXED for one accessor, RESIDUAL for another.**

* `Coordinator::state()` returns `const DurableState&` with no lock held, and the
  header now carries the contract explicitly: it "is the one accessor that takes
  no lock: it is safe only when the caller is the sole thread using the
  coordinator (a single-threaded harness, or after `stop()`). Concurrent callers
  must use `describe()`, `evaluate()` or the query surface instead." Internal
  code never uses it - it uses `impl.state()` under the lock - and every caller
  in the tools and the self test is single-threaded. The hazard is closed by a
  documented precondition rather than by a snapshot return.
* `Coordinator::recovery()` is `noexcept` and reads the store's recovery outcome
  without the lock, so the header's phrase "the one accessor that takes no lock"
  is narrower than the code: two accessors are lock-free. The read is of a small
  enum that is written once during `create()`, so it is benign in practice, but it is not synchronised by
  construction and the header states no restriction. Required action: document
  immutability after construction, or take the lock.

**Evidence.** `include/dmf/runtime.hpp` (`state()`, `recovery()`),
`include/dmf/net.hpp` (`SessionHandler`), `src/net/server.cpp` (`run_session`),
`src/runtime/coordinator.cpp` (`on_request`, `counters`).

## 11. Accounting counters mutated from several threads (FIXED)

**Hazard.** The server is constructed with a pointer to the coordinator's
accounting facade, and accept/session threads called
`record_frame_sent`, `record_frame_received`, `record_frame_rejected`,
`record_session_opened`, `record_session_closed`, `record_session_rejected`
and `record_request_rejected` while the coordinator mutated the same
non-atomic counter block under its own mutex. That is a data race with lost
updates, and it could make `check_closure()` report violations that never
happened.

**Fix in the tree.** `Accounting` owns a non-recursive mutex; every
`record_*` entry point, `replace()`, `counters()` and `check_closure()` take it,
`counters()` returns the block by value ("a reference is never handed out"), and
the class deletes copy and move so it cannot be duplicated. The facade stays a
leaf lock (item 5). There is exactly one counter block: it lives in
`DurableState` and is exposed by `StateStore::accounting()`, so the store,
the runtime and the protocol server all reach the same instance.

**Evidence.** `include/dmf/accounting.hpp` (`mutex_`, `counters()`,
`replace()`), `src/runtime/accounting.cpp` (`bump`, `counters`, `replace`,
every `record_*`), `src/store/state_store.cpp` (`Impl::accounting`).

**Verdict: FIXED.** Residual cost: `AccountingCounters` remains a plain value
type, so a caller that copies the block once and then reasons about it holds a
snapshot, not a live view; that is the intended semantic.

## 12. Session registry retention (RESIDUAL)

**Mechanism.** `accept_loop` pushes every accepted session into
`Server::Impl::sessions` and never removes it when the session ends; only
`drain_sessions` (called from `Server::stop()`) swaps the vector out. A finished
session therefore keeps its `SessionState` alive, including a `std::thread`
object that remains joinable until `stop()` joins it, and a closed `Socket`.

**Evidence.** `src/net/server.cpp` (`accept_loop`, `drain_sessions`,
`release_and_join`).

**Verdict: RESIDUAL.** With `max_sessions` bounding concurrent sessions but not
completed ones, a long-lived server that sees many connect/disconnect cycles
accumulates OS thread handles and per-session allocations until shutdown.
Required action: remove the state from the registry when the session thread
finishes (for example by marking an index and reaping on the next accept), or
join finished sessions opportunistically.

## 13. Evidence incarnation ordering (FIXED)

**Hazard.** `EvidenceStore::publish` superseded live evidence on
(scope, kind, publisher) and never compared `publisher_boot`, so an item
published under an older incarnation could replace a newer one even though
`include/dmf/evidence.hpp` states that evidence from a previous incarnation of
the same publisher is never current.

**Fix in the tree.** The store keeps `publisher_incarnation_` (publisher ->
highest boot incarnation seen). An item from an older incarnation is refused with
`ErrorCode::Stale`; an item from a newer incarnation is marked as superseding the
publisher's previous contributions and the map is advanced.

**Evidence.** `src/runtime/evidence_store.cpp` (`publish`),
`include/dmf/runtime.hpp`.

**Verdict: FIXED.** Residual notes: the `replaced` predicate in the rebuild loop
contains a second disjunct that is implied by the first, so the "a fresh
incarnation replaces everything the old one contributed" comment describes more
than the code does (replacement still requires a matching scope and kind); and
`clear()` resets the live vector but not the incarnation map, which is the safe
direction (an older incarnation stays rejected) but should be stated.

## 14. Request/acknowledgement pairing (FIXED)

**Hazard.** `is_acknowledgement()` was implemented as `value % 2 == 0`. Because
the numbering puts every request except `Hello` on an even number and every
acknowledgement on request + 1, the predicate classified every real request as an
acknowledgement, and `Client::call` - which rejected any type for which the
predicate was true - refused `PUBLISH_EVIDENCE`, `EVALUATE_CONTRACT`,
`ACQUIRE_AUTHORITY`, `QUERY_STATUS` and every other client request with
`InvalidArgument`. Only the handshake worked.

**Fix in the tree.** `is_acknowledgement()` is now an explicit switch over the
thirteen acknowledgement types; `is_request()` is
`is_valid(value) && !is_acknowledgement(value)`; `acknowledgement_for()` maps a
request to its acknowledgement and `request_for()` maps back. `Client::call`
guards on `is_request` and verifies the reply with `acknowledgement_for`;
`Server::response_type` delegates to `acknowledgement_for`.

**Evidence.** `src/net/frame.cpp`, `src/net/client.cpp`, `src/net/server.cpp`,
`include/dmf/net.hpp`.

**Verdict: FIXED.**

## 15. Session lifetime and registration order (SAFE)

**Mechanism.** The session state is a `std::shared_ptr<SessionState>` held by
both the registry and the session thread's lambda capture. The state is pushed
into the registry **before** `std::thread` is constructed, so a concurrent
`Server::stop()` can never miss a live session; the thread's own shared_ptr keeps
the state alive after the registry is drained and after the thread function
returns. `drain_sessions` moves the vector out under the lock and the joins
happen outside it.

**Evidence.** `src/net/server.cpp` (`accept_loop`, `session_loop`,
`drain_sessions`, `release_and_join`).

**Verdict: SAFE.** This is the mechanism behind the comment "a session is
registered before its thread exists, so shutdown can never miss a live session".

**Residual:** if `std::thread` construction throws (thread creation failure),
the state is already registered with a non-joinable thread and the exception
escapes `accept_loop`, whose thread function has no handler, so the process
terminates. Required action: catch the failure, unregister the state and either
continue or stop the server cleanly.

## 16. Per-session buffer ownership (SAFE)

**Mechanism.** Each session thread owns its own `FrameReader` and reads through
its own `Socket`; the `FrameReader` is never shared. The client is documented as
single-connection, one call at a time, and holds one `Socket` and one
`FrameReader`.

**Evidence.** `src/net/server.cpp` (`run_session`), `src/net/client.cpp`,
`include/dmf/net.hpp` ("Single-connection client. Not thread safe: one call at a
time.").

**Verdict: SAFE.**

## 17. Ordered start of the server inside the coordinator (SAFE)

**Mechanism.** `Coordinator::start()` takes the state lock, refuses a second
start (`InvalidState`), constructs the `Server` with `this` and the accounting
facade, releases the lock, and only then calls `Server::start()`. If binding
fails, it re-acquires the lock and destroys the server. The server therefore
begins accepting with `impl_->server` already published and the accounting
facade already constructed.

**Evidence.** `src/runtime/coordinator.cpp` (`Coordinator::start`).

**Verdict: SAFE.**

## 18. Cross-process writer lock on one store root (SAFE)

**Mechanism.** A writable `StateStore::open` acquires an exclusive
operating-system lock on `<root>/store.lock` (`_locking(_LK_NBLCK, 1)` on
Windows, `flock(LOCK_EX | LOCK_NB)` elsewhere) before it reads or writes any
journal. A second process on the same root is refused with
`ErrorCode::AlreadyExists` instead of interleaving appends and corrupting the
store. `StateStore::close()` releases the lock before returning, and
`~StateStore` calls `close()`, so the lock is released on every normal exit
path. The lock is an OS file lock, not a C++ mutex: it is acquired once, never
nested, and never held while acquiring a thread lock.

**Evidence.** `src/store/file_io.hpp`/`src/store/file_io.cpp` (`LockFile`),
`src/store/state_store.cpp` (`open`, `close`), `include/dmf/store.hpp`
(`StoreConfig::read_only`).

**Verdict: SAFE.** The multiprocess suite's single-writer case exercises it with
real processes. The lock is advisory and local to one host's file system; it is
not an access control and it does not coordinate separate roots.

## 19. Exception boundary on the session thread (SAFE)

**Mechanism.** `session_loop` catches every exception escaping `run_session`,
counts the session as a rejected request, closes the socket and calls
`on_session_closed` - all on the session's own thread. A handler that throws (a
decoder that fails to allocate, for example, since decoders are deliberately not
`noexcept`) therefore ends one session rather than terminating the process.

**Evidence.** `src/net/server.cpp` (`session_loop`).

**Verdict: SAFE.** The accept thread has no such boundary (item 15 residual), and
the boundary is deliberately per-session rather than per-server, so it cannot be
used to keep a broken session alive.

---

## Required actions

| Priority | Action | Item |
| --- | --- | --- |
| High | Reap finished sessions so their thread handles are joined and their state released | 12 |
| Medium | Make `Server::stop()` single-entry (atomic flag), or give `Listener` a lifecycle mutex | 6, 9 |
| Medium | Handle `std::thread` construction failure in `accept_loop` | 15 |
| Low | Take the lock in `Coordinator::recovery()`, or document it as immutable after construction | 10 |
| Low | Correct the stale `shutdown()` sentence in the `Socket` class comment in `include/dmf/net.hpp` | 6 |
| Low | Compare `bytes_in_segment` against the truncated length after tail recovery | `docs/PERSISTENCE.md` item 1 |
| Low | Remove the redundant disjunct in the evidence replacement predicate and document the `clear()` behaviour | 13 |

## Audit method and limits

* Every lock, atomic and thread in the repository was located by search and read
  in context; the four thread mutexes in the inventory are the complete set, and
  the store's OS file lock is listed separately.
* The storage, protocol, integration, property, concurrency, multiprocess and
  scale suites now exist and pass, so several mechanisms above are corroborated
  by an observed run rather than by argument alone: item 6 by the shutdown and
  release cases, item 11 by the accounting-under-concurrency cases, item 12 by
  the session-churn case (which exercises the accumulation, not a fix for it),
  and item 18 by the two-writer case. The items marked SAFE that no suite
  exercises directly (1, 2, 3, 5, 7, 8, 17) remain arguments from the code.
* No dynamic race detector was run: ThreadSanitizer coverage is not requested by
  the build files (`cmake/DmfSanitizers.cmake` probes AddressSanitizer and
  UndefinedBehaviorSanitizer only), and a hang-free run is not a race proof. The
  AddressSanitizer build passes (see `docs/EVIDENCE-MATRIX.md`), which covers
  memory errors but not data races.
* The tree was being modified while the audit was performed and has no commits;
  rows marked FIXED reflect the mechanism present at the end of the audit.
