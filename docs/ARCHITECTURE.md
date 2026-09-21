# Degraded Mode Fabric - Architecture

Degraded Mode Fabric (DMF) version 1.0.0, Summon Software Labs, Apache-2.0.
This document describes the systems boundary, the build layering, the module map,
the allocator problem class, and the end-to-end data flow. It describes only
behaviour that exists in the repository at the paths cited.

## 1. Systems boundary

DMF is a policy and authority runtime for degraded-mode service. It decides which
subset of a service's contractual obligations remains legally supportable under
observed fabric capability, records that decision, and issues, bounds and fences
the authority to act on it.

The runtime does not:

* measure the fabric (there is no code that opens a NIC, switch, RDMA device or
  DPU, and no code that samples bandwidth, latency or topology);
* create capacity (bandwidth is modelled as an offered quantity supplied by an
  observer);
* route or shape traffic (an issued `Grant` is an authority record, not a
  forwarding instruction);
* invent policy (an uninstalled or non-matching policy yields UNSUPPORTED, never
  an implicit concession);
* authenticate cryptographically (see `docs/PROTOCOL.md` and
  `docs/PERSISTENCE.md` for the two trust boundaries).

Capability enters the runtime only as `EvidenceItem` values published by a
caller or by a remote publisher over the control protocol. Evidence is
observation, never authority (`include/dmf/evidence.hpp`). Absence of evidence
is UNKNOWN, contradictory evidence is CONFLICT, and neither is converted into a
successful outcome.

Every fixture that ships with the repository is synthetic. Where a decision rests
on synthetic or unclassified capability, the evaluator attaches
`ReasonCode::HardwareNotExercised` to the decision
(`src/engine/evaluator.cpp`). `OriginClass::Real` is a labelling value that a
configuration or tool flag can set; setting it does not create a measurement.

## 2. Layering and exported targets

```
CMakeLists.txt
  |
  |-- dmf_core        (STATIC, 16 sources)  ->  DegradedModeFabric::dmf_core
  |     pure value types, bounded codecs, guarantee algebra, engine
  |     no threads, no sockets, no files, no clocks, no Threads dependency
  |
  |-- dmf_runtime     (STATIC, 13 sources)  ->  DegradedModeFabric::dmf_runtime
  |     durable store, framed transport, accounting, coordinator
  |     PUBLIC link: dmf_core, Threads::Threads, ws2_32 (WIN32 only)
  |     PRIVATE include: src/ (internal store headers, never installed)
  |
  |-- DegradedModeFabric::dmf    (ALIAS of dmf_runtime, build tree only)
  |
  |-- tools:     dmf_coordinator, dmf_publisher, dmf_agent, dmf_cli, dmf_selftest
  |-- example:   dmf_example_degradation (not installed)
  |-- tests:     dmf_testkit (STATIC) + eight suites (tests/CMakeLists.txt)
```

Facts enforced by `CMakeLists.txt`:

* `cmake_minimum_required(VERSION 3.20)`, project version 1.0.0, C++20 with
  extensions off, position-independent code on, default build type Release when
  neither `CMAKE_BUILD_TYPE` nor `CMAKE_CONFIGURATION_TYPES` is set.
* `find_package(Threads REQUIRED)`; only `dmf_runtime` links threads.
* The three alias targets are `DegradedModeFabric::dmf_core`,
  `DegradedModeFabric::dmf_runtime` and the convenience alias
  `DegradedModeFabric::dmf` for `dmf_runtime`. Only the first two are exported;
  `DegradedModeFabric::dmf` exists in the build tree only.
* Install/export: the five tool binaries install into
  `${CMAKE_INSTALL_BINDIR}`; `dmf_core` and `dmf_runtime` are exported into
  `DegradedModeFabricTargets.cmake` under the `DegradedModeFabric::` namespace;
  `cmake/DegradedModeFabricConfig.cmake.in` re-finds `Threads` only (`ws2_32`
  propagates as a plain interface link library) and the version file uses
  `SameMajorVersion`. `dmf_example_degradation` and `dmf_testkit` are never
  installed. Only `include/` is installed as headers; the internal
  `src/store/*.hpp` headers are reachable through a PRIVATE include directory.
* Options: `DMF_BUILD_TESTS`, `DMF_BUILD_TOOLS`, `DMF_BUILD_EXAMPLES`
  (all ON), `DMF_WARNINGS_AS_ERRORS` (ON), `DMF_ENABLE_ASAN`,
  `DMF_ENABLE_UBSAN` (both OFF), `DMF_DISCOVER_SANITIZER_SUPPORT` (ON).
* Warning policy is centralised in `cmake/DmfWarnings.cmake` and applied per
  target, privately: on MSVC `/W4 /permissive- /utf-8 /Zc:__cplusplus
  /Zc:preprocessor /EHsc /MP`, elsewhere `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
  -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2`.
  `DMF_WARNINGS_AS_ERRORS` appends `/WX` or `-Werror` to the same targets.
* Sanitizer policy is centralised in `cmake/DmfSanitizers.cmake`. It probes the
  toolchain with `check_cxx_source_runs` and records
  `DMF_ASAN_STATUS`/`DMF_UBSAN_STATUS` as one of `NOT_REQUESTED`,
  `UNPROBED`, `SUPPORTED`, `UNSUPPORTED`, printing
  `DMF sanitizer probe: ASan=<status> UBSan=<status>` at configure time.
  Requesting a sanitizer the toolchain cannot run is a configure-time
  `FATAL_ERROR`, so a build is never silently uninstrumented. On MSVC the ASan
  build also adds `/Zi` (ASan requires debug information); the UBSan flag
  `/fsanitize=undefined` has no MSVC runtime, so a UBSan request on MSVC fails
  the probe and stops configuration.
* A `.clang-tidy` at the repository root enables `clang-analyzer-*`,
  `bugprone-*`, `performance-*` and `portability-*` (everything else off) with
  three documented disables: `clang-analyzer-optin.core.EnumCastOutOfRange`
  (bit-flag enums), `bugprone-easily-swappable-parameters` (strongly typed
  identities) and `bugprone-branch-clone` (decoders sharing a body).

The dependency direction is strict: nothing in `dmf_core` includes anything from
`dmf_runtime`, and no core type performs I/O or takes a lock.

## 3. Module map

| Module | Public header | Implementation | Responsibility | Must not |
| --- | --- | --- | --- | --- |
| core | `include/dmf/core.hpp` | `src/core/core.cpp` | Hard global bounds, CRC-32C, FNV-1a 64 digests, checked arithmetic, `Status`/`Result`, `Tick`/`TickSource`, bounded collections, canonical byte reader/writer, text safety | Allocate on an unvalidated count; treat a wrapped integer as a value; reserve capacity on every write (see `ByteWriter::ensure`) |
| ids | `include/dmf/ids.hpp` | `src/core/ids.cpp` | Strongly typed identities (`StrongUInt<Tag>`), the generation domains, monotone `IdAllocator`, `OriginClass` | Convert an identity to a raw integer implicitly; reuse an exhausted identity (exhaustion returns `nullopt`) |
| reason | `include/dmf/reason.hpp` | `src/core/reason.cpp` | `DecisionOutcome`, `SupportTri`, `PlanStatus`, `RestorationOutcome`, `ReasonCode`, `ProvenanceClass` | Map UNKNOWN/STALE/CONFLICT/INVALID/UNSUPPORTED onto success |
| guarantee | `include/dmf/guarantee.hpp` | `src/domain/guarantee.cpp` | Guarantee kinds, comparators, canonical sets, the weakening relation, concession bounds, envelopes, explicit withdrawals | Approve a set that strengthens or adds a guarantee; drop a guarantee without materialising a withdrawal |
| contract | `include/dmf/contract.hpp` | `src/domain/contract.cpp` | The service obligation set for one subject on one scope, its envelope, protection marker, priority, bandwidth demand and floor | Create obligations or capacity |
| evidence | `include/dmf/evidence.hpp` | `src/domain/evidence.cpp` | Evidence items and their aggregation lattice, freshness, capability derivation, support predicates and shortfall reasons | Substitute an optimistic value for a missing group |
| authority | `include/dmf/authority.hpp` | `src/domain/authority.cpp` | `Incarnation`, `AuthorityVector`, `AuthorityBinding`, `AuthorityDelta`, the authority ladder, grant lifecycle states and `FenceReason` | Compare identity without generation; treat an unexpired grant as valid after a generation change |
| policy | `include/dmf/policy.hpp` | `src/domain/policy.cpp` | Ordered rule table, per-class profiles, evidence-state actions, TTL/dwell/search budgets, envelope composition | Authorise anything the rules do not explicitly authorise |
| decision | `include/dmf/decision.hpp` | `src/domain/decision.cpp` | The externally visible per-contract answer, allocation entries, work counters, infeasibility certificate, explanations | Report an authorising outcome for a refused contract |
| grant | `include/dmf/grant.hpp` | `src/domain/grant.cpp` | Committed degraded authority, restoration preconditions and their evaluation, restoration outcome | Grant authority above `Authorized` before a subject acknowledges, or `Applied` without a verified report |
| codec | `include/dmf/codec.hpp` | `src/codec/codec.cpp` | One canonical encoding for both the durable store and the wire, count-before-allocate decoding, `decode_finish`; decoders are not `noexcept` so an allocation failure propagates instead of terminating | Accept trailing bytes or an out-of-domain enum |
| engine | `include/dmf/engine.hpp` | `src/engine/{evaluator,allocator,reference_solver,validator,restoration}.cpp` | Single-contract evaluation, per-scope allocation, independent plan validation, exhaustive reference optimum, restoration evaluation | Claim optimality after exhausting its budget; claim infeasibility without an arithmetic certificate |
| store | `include/dmf/store.hpp` | `src/store/{file_io,journal,snapshot,state_store}.cpp` + internal headers | Append-only journal, transactional snapshots, recovery, durable record vocabulary, bounded retention, crash injection, a cross-process writer lock, a read-only inspection mode and the single accounting facade | Repair anything other than a reported torn tail; resurrect liveness; accept a second writer on one root |
| net | `include/dmf/net.hpp` | `src/net/{frame,socket,messages,server,client}.cpp` | Framed protocol, loopback transport with an explicit wakeup channel, threaded server, single-connection client, request/response bodies | Provide confidentiality, authentication or non-loopback binding |
| runtime | `include/dmf/runtime.hpp` | `src/runtime/{accounting,evidence_store,grant_table,coordinator}.cpp` | The coordinator: single authoritative view, live evidence frontier, grant index, exact accounting and closure, bearer-token session roles | Invoke a callback or a socket write while holding its state lock |

## 4. The allocator problem class and objective

Stated in `include/dmf/engine.hpp` and implemented in `src/engine/allocator.cpp`:

* Instance: one scope, one `CapabilitySnapshot`, N contracts registered for that
  scope.
* Bandwidth is the only additive, contended fabric resource. Every other
  guarantee is a support predicate evaluated against the snapshot:
  `supports()` in `src/domain/evidence.cpp` maps capability fields onto
  guarantee kinds (availability uses reachable ppm, bandwidth uses aggregate
  kbps, latency uses RTT microseconds with an AtMost comparator, path diversity
  uses the path count, durability maps the synchronous-durability flag onto
  level 4 or 0, reachability uses node coverage ppm).
* A contract is rigid when it is a protected obligation (service class
  Protected or the explicit `non_degradable` marker) or carries a frozen
  envelope (`max_concessions() == 0`). A rigid contract is either served at its
  full original obligations or explicitly refused; it is never weakened.
* Flexible contracts absorb degradation.

Objective, strictly lexicographic:

1. maximise the number of rigid contracts served in full;
2. maximise the sum of their priorities;
3. serve flexible contracts in the deterministic order and first-fit against the
   remaining capacity, refusing any contract that cannot reach its policy floor.

Implementation mechanics that make the objective match the claim:

* `assess()` resolves every non-bandwidth guarantee, choosing the strongest
  value the snapshot supports inside the composed envelope
  (`Policy::effective_envelope` = stricter of contract envelope and class
  ceiling, capped by `max_concessions_per_contract`). Exceeding the envelope
  yields UNSUPPORTED with `ReasonCode::PolicyEnvelopeExceeded`, never a silent
  clamp.
* A non-Known capability state is handled by the policy's per-state action
  (`unknown/stale/conflict/invalid/unsupported_evidence_action`, all defaulting
  to Refuse). `Degrade` on unproven capability yields INDETERMINATE plus
  escalation rather than an invented floor.
* Rigid selection is an exact branch-and-bound knapsack over the rigid set. Item
  value is `BIG + priority` where `BIG = sum(priorities) + 1`, so maximising
  value maximises count first and priority second. The fractional (LP)
  relaxation over a density-sorted suffix is the upper bound; the incumbent is
  always a feasible selection, so aborting on the node budget still yields a
  legal plan. Ties are broken by the lexicographically smallest selection
  vector, which makes the result independent of insertion order.
* `Policy::search_node_budget` bounds the search. On exhaustion the plan status
  is SEARCH_LIMIT_REACHED and optimality is never claimed.
* PROVEN_INFEASIBLE is emitted only when `minimum_protected_served` cannot be
  met, and only with a filled `InfeasibilityCertificate`: either fewer admissible
  rigid contracts than required, or the sum of the k smallest rigid bandwidth
  demands exceeds the offered capacity.
* Flexible allocation walks `allocation_precedes` order (class rank, then
  priority, then identity - the protection key and the priority key swap places
  when `policy.protect_first` is false), allocates
  `min(demand, remaining)`, refuses below the contract's bandwidth floor with
  `ReasonCode::ResourceExhausted`, and re-checks the composed envelope after
  allocation.
* Output ordering is canonical: entries are sorted by contract identity, so
  `AllocationPlan::digest()` is stable.

Independent corroboration exists inside the library: `validate_plan()`
(`src/engine/validator.cpp`) re-derives every obligation from the plan and the
request without consulting allocator state, and `reference_solve_rigid()`
(`src/engine/reference_solver.cpp`) enumerates every rigid subset for small
instances. The reference solver refuses instances with 63 or more rigid
candidates or more than `max_subsets` subsets, and is never called on a live
path.

## 5. Data flow

```
observed fabric                     (evidence is observation, never authority)
      |
      v
EvidenceItem  --publish_evidence-->  EvidenceStore (live frontier, cleared on boot)
      |                                   |
      |                                   v
      |                        derive_capability(scope, capacity generation, freshness)
      |                        all seven evidence groups must be Known and fresh
      |                                   |
      |                                   v
      |                          CapabilitySnapshot {state, topology, bandwidth,
      |                                              rtt, paths, reachability,
      |                                              coverage, durability, origin}
      v                                   |
durable EvidencePublished (+ Fabric/Capacity/Evidence generation advances)
                                          |
                                          v
                        allocate_scope(scope, contending contracts, capability, policy)
                                          |
                                          v
                             AllocationPlan {status, entries, work, certificate}
                                          |
                                          v
                        evaluate_contract(contract, capability, policy, entry)
                                          |
                                          v
                                 Decision {outcome, original, approved, delta,
                                           reasons, support, evidence_state,
                                           plan_status, escalation_required,
                                           provenance, max_authority}
                                          |
                    durable DecisionRecorded (bounded by max_decisions)
                                          |
                                          v
                        acquire_locked -> Grant {binding, degraded, delta,
                                                 preconditions, sequence, attempt,
                                                 issued/expires ticks}
                                          |
                    durable GrantIssued
                                          |
                        acknowledge -> durable GrantAcknowledged (attempt rotates)
                        report_applied -> durable GrantApplied (attempt rotates)
                                          |
                                          v
              revalidate() sweep / publish_evidence / install_policy / explicit fence
                                          |
        +---------------------------------+----------------------------------+
        |                                 |                                  |
   expired at now                authority component changed        capability changed only
        |                                 |                                  |
        v                                 v                                  v
  GrantExpired                   GrantFenced with                    re-evaluate the contract
  (Expired)                      fence_reason_for(delta)                    |
                                                                   approved weakens-or-equals
                                                                   the grant's degraded set?
                                                                      |             |
                                                                     yes            no
                                                                      |             |
                                                                      v             v
                                                        durable GrantRebound     GrantFenced
                                                        (grant stays live,       (fence_reason_for(delta))
                                                         binding rebound to
                                                         current generations)
                                          |
                                          v
                        request_restoration -> RestorationEvaluation
                        (Proven only with Known evidence and every precondition
                         Satisfied) -> durable RestorationProven / RestorationRefused
                                          |
                        Proven also fences the grant with FenceReason::Restored
```

Generation binding is what makes the flow safe. Each `Decision` and `Grant`
stores an `AuthorityBinding`: coordinator term, boot incarnation, fabric,
capacity, policy and evidence generations, plus contract identity and
generation, subject identity and generation, and scope. A later comparison
classifies the difference into an `AuthorityDelta`; any non-None delta is a
change of state. Changes to the authority components (boot, term, policy,
contract, contract generation, subject, subject generation, scope) invalidate the
authority and map onto the `FenceReason` that must be recorded. A change to the
observed capability alone is a different case: the runtime re-evaluates the
obligation and keeps the grant only when the re-evaluated approved set still
weakens-or-equals the set the grant promised, in which case the grant is rebound
durably (`RecordType::GrantRebound`, value 21) to the current generations.
See `docs/AUTHORITY.md`.

## 6. Invariants

* An approved guarantee set is always a weakening of the original: it may not
  add a guarantee kind, may not strengthen a shared one, and every dropped
  guarantee is materialised as an explicit withdrawal
  (`classify_weakening`, `compute_delta`).
* A protected obligation is never weakened; an unmet protected obligation is
  refused or escalated, never degraded.
* With no matching policy rule there is no legal concession: the outcome is
  UNSUPPORTED and escalation is requested.
* Absence of evidence is UNKNOWN; UNKNOWN can never authorise full service and
  can never produce a Proven restoration.
* A live grant is never carried across a generation change on the strength of its
  identity: an authority-component change fences it, and a capability-only change
  keeps it only with a fresh proof that the promised obligations are still
  serviceable, recorded as a durable rebind.
* Durable records and live authority are different things: a restarted runtime
  keeps definitions, policy, lineage and fences, and clears live evidence and
  pre-restart grant liveness (see `docs/PERSISTENCE.md`).
* Identity is not generation. Holders compare both (see `docs/AUTHORITY.md`).
* Every externally reachable collection is bounded before allocation by one of
  the constants in `include/dmf/core.hpp`.

## 7. Runtime shape and threading

* One `Coordinator` owns one `StateStore`, one live evidence frontier, one grant
  index and one non-recursive mutex. Public entry points take the mutex once and
  then call `*_locked` helpers. `Coordinator::state()` is documented as the one
  accessor that takes no lock, and is safe only for exclusive use (a
  single-threaded harness, or after `stop()`); `Coordinator::recovery()` is also
  lock-free and reads a small enum that is written once during `create()`.
* The store owns the single `AccountingCounters` block and exposes one
  `Accounting` facade over it (`StateStore::accounting()`). The facade is
  internally mutex-protected, `counters()` returns a copy, and it is shared by
  the coordinator and the protocol server, so no counter has two unsynchronised
  writers. `Accounting::record_plan()` and the `grants_released` counter are
  declared and serialised but no code path in this revision calls or increments
  them; plan counters therefore stay at zero.
* The optional framed server runs one accept thread plus one thread per session,
  bounded by `ServerConfig::max_sessions` (default 64). The session handler is
  invoked on the session's own thread. Every session socket gets a loopback
  datagram wakeup channel when the session is created; blocking accept and
  blocking read wait in `select()` on both the descriptor and that channel,
  and `release_blocked_io()` sends one datagram.
* A writable `StateStore` open takes an exclusive operating-system lock on
  `<root>/store.lock` and releases it in `close()`, so one store root has one
  writer. A read-only open takes no lock.
* The coordinator never reads wall-clock time. Time is a monotonic `Tick`
  advanced explicitly by `Coordinator::advance`, which is what makes every
  temporal assertion reproducible.
* Locking, ownership and teardown are audited item by item in
  `docs/CONCURRENCY-AUDIT.md`, including the findings that are not fixed.

## 8. What is not implemented

* No fabric or hardware adapter. There is no code for switches, NICs, RDMA, DPUs
  or multi-node fabrics, and no measurement path of any kind.
* No distributed coordination. A single coordinator owns a store root. There is
  no consensus protocol and no leader election between coordinators; the
  cross-process lock only refuses a second writer on the same root.
* No cryptographic authentication, confidentiality or tamper-proofing. The
  control protocol carries a bearer token compared against
  `CoordinatorConfig::principals`, and durable integrity is CRC-32C only.
* No off-host binding. `ServerConfig` has no host field and
  `Listener::bind_loopback` always binds `INADDR_LOOPBACK`; the control channel
  is local by construction.
* No routing, shaping, scheduling or traffic engineering. A grant is an authority
  record for another system to act on.
* No separate session translation unit: session handling lives in
  `src/net/server.cpp`. `src/net/` contains `frame.cpp`, `socket.cpp`,
  `messages.cpp`, `server.cpp`, `client.cpp`.
* No second toolchain or second operating system run is recorded. The POSIX paths
  in `src/net/socket.cpp`, `src/store/file_io.cpp` and
  `tests/testkit/process.cpp` exist in the source but were not exercised on the
  host that produced `docs/EVIDENCE-MATRIX.md`.
* UndefinedBehaviorSanitizer coverage is not available on the recorded host:
  MSVC ships no UBSan runtime and no clang-cl is installed, and the CMake probe
  records that at configure time.
