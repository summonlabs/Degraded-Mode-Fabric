# Degraded Mode Fabric

Degraded Mode Fabric (DMF) is a portable C++20 runtime that answers one question
deterministically:

> Given reduced fabric connectivity or capability and the current service
> obligations, which degraded service remains legally supportable right now,
> which guarantees must be withdrawn or weakened explicitly, and when must
> degraded authority be fenced, escalated, or restored?

It is infrastructure, not a simulator, a router, or a policy editor. It owns
*explicit degraded-mode policy and authority* and nothing else.

---

## 1. Exact product boundary

DMF **owns**:

* the degradation policy: which concession is legal, for which service class, on
  which scope, and on whose authority;
* the decision: for one subject and scope, which obligations remain supportable
  and at exactly what level, with every weakening explained;
* the authority: committed degraded grants, their expiry, their fencing, their
  restoration, and the generation set that made each of them legal;
* the durable lineage: definitions, policy, committed outcomes, fences, and the
  accounting that closes over them.

DMF **does not**:

* measure the fabric. Capability arrives as evidence published by an adjacent
  observer. DMF has no NIC, switch, RDMA, DPU or NVLink adapter;
* create capacity, compute routes, repair components, or enforce rate limits;
* authenticate cryptographically. The transport is plaintext loopback with a
  bearer-token check; see section 8;
* coordinate across machines. There is no distributed consensus, no leader
  election and no cross-host replication.

Integration is through explicit typed inputs and outputs: contracts, evidence
items, policy, an authority vector, decisions, grants, fences and restoration
evaluations. No adjacent runtime responsibility is absorbed.

---

## 2. Build, install and use

Requirements: CMake 3.20+, a C++20 compiler, and nothing else at runtime. The
library has no third-party dependency; on Windows it links only `ws2_32`.

```powershell
# Configure, build and test. The script finds MSVC through vswhere.
./scripts/build.ps1 -Config Release

# Debug, and the address sanitizer build.
./scripts/build.ps1 -Config Debug
./scripts/build.ps1 -Config Release -Sanitizer asan

# Install and validate an independent consumer against the installed prefix.
./scripts/closure.ps1 -Stage install
```

Equivalent direct commands:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
cmake --install build/release --prefix /tmp/dmf
```

Downstream consumption uses the exported package:

```cmake
find_package(DegradedModeFabric 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE DegradedModeFabric::dmf_runtime)
```

Two targets are exported:

| Target | Contents | Threads |
|---|---|---|
| `DegradedModeFabric::dmf_core` | identities, domain model, canonical codec, degradation engine, durable store | none |
| `DegradedModeFabric::dmf_runtime` | framed protocol, sockets, server, client, coordinator runtime | yes |

`examples/consumer` is a complete program that links only against the installed
package and fails loudly if any invariant breaks.

### Tools

| Tool | Purpose |
|---|---|
| `dmf_coordinator` | the runtime service: owns epoch, generations, policy, evidence, grants and fences |
| `dmf_publisher` | stands in for an adjacent fabric observer and publishes a capability fixture |
| `dmf_agent` | a service agent: acquires, acknowledges, reports effect, requests restoration |
| `dmf_cli` | operator client plus an offline, read-only `verify` of a durable store |
| `dmf_selftest` | in-process closure proof of the product-defining proposition |

Every tool prints deterministic `key=value` lines and flushes after each, so a
supervising process can block on a barrier instead of guessing with a timer.

---

## 3. Authority model

### Identity is not generation

Every authority-bearing dependency carries its own strongly typed generation.
A matching identity with a stale generation is a *different* authority and is
treated as such: `AuthorityBinding::classify` returns a flag per component that
differs, and any non-empty difference invalidates the binding.

```
AuthorityVector  = coordinator_term, boot, fabric, capacity, policy, evidence
AuthorityBinding = AuthorityVector + contract, contract_generation,
                   subject, subject_generation, scope
```

### The authority ladder

| Level | Established by | Is it authority? |
|---|---|---|
| `Observed` | an evaluation whose outcome does not authorise service | no |
| `Eligible` | a degradable contract with Known evidence that is not currently served | no |
| `Recommended` | an evaluation that authorises service, before anything is committed | **no** |
| `Authorized` | a committed grant | yes |
| `Acknowledged` | the subject reports it accepted the grant | yes, but not effect |
| `Applied` | the subject reports the effect under the current attempt identity | yes, verified effect |

Observation is not authority. Eligibility is not authorisation. Authorisation is
not application. Acknowledgement is not verified effect.

### Fencing and revalidation

A live grant is fenced when:

* it expires;
* its coordinator term, boot incarnation, policy generation, contract, contract
  generation, subject, subject generation or scope changes — those are changes to
  the *authority*, and the grant dies with them;
* the observed capability changes such that the level it promised can no longer
  be proven serviceable. A capability change alone does not fence: the grant is
  re-evaluated, and it survives only when the runtime can still prove that what
  it promised is supportable, in which case it is rebound to the new generation
  set through a durable `GRANT_REBOUND` record.

Restoration is a transition, not a state. It requires positive proof for every
recorded precondition — current Known evidence, a capability that supports the
*original* obligations, no outstanding fence, unchanged contract and generation,
a satisfied dwell requirement, and a policy that still permits full service.
UNKNOWN evidence can never produce `PROVEN`.

---

## 4. Major invariants

1. **Degraded output never exceeds original authority.** Every approved
   guarantee set is checked against the original with `weakens_or_equals`.
   Adding a guarantee, strengthening a shared one, or turning a soft guarantee
   hard are all rejected. The check runs in the evaluator, in the plan
   validator, in `Decision::validate`, in `Grant::validate`, on every codec
   decode, and on every snapshot load.
2. **A rigid obligation is whole or refused.** A protected obligation, or any
   contract with a frozen envelope, is never partially served: it is served at
   its full original obligations or explicitly refused/escalated.
3. **Weaker traffic absorbs degradation first.** The allocator maximises the
   number of rigid contracts served in full, then their priority sum, and only
   then serves flexible contracts. An exhaustive reference solver differentially
   tests this over thousands of seeded instances.
4. **Absence of proof is never proof of support.** Missing, stale, conflicting,
   invalid or unsupported evidence yields `Indeterminate`, never `Supported`.
   The default evidence actions are all `Refuse`.
5. **Every concession is explicit.** A decision or grant carries the original
   set, the approved set, and the complete delta of concessions and withdrawals.
   A non-authorising decision carries no approved set at all.
6. **Every decision is revocable.** A decision binds the full generation set
   that made it legal, and any change to an authority component fences it.
7. **Determinism.** Identical inputs produce byte-identical plans, decisions and
   digests, independent of container, hash, insertion or discovery order.
8. **Exact accounting closure.** `decisions_total` equals the sum over
   outcomes; `grants_issued` equals live plus retained terminated plus pruned;
   `fences_total` equals retained plus pruned. Lifecycle counters are derived
   from the journal, so closure holds across a hard kill.
9. **Persistence is not liveness.** A restart never resurrects a grant,
   observation or acknowledgement. A closed store is a closed store.

---

## 5. Lifecycle and restart semantics

```
evidence --derive--> capability --allocate--> plan --evaluate--> decision
   --acquire--> grant --acknowledge--> --apply--> --fence/expire/revoke--> restored
```

On open, a writable store advances the boot incarnation and the coordinator
term, fences every grant the previous incarnation committed with
`BOOT_ADVANCE`, clears the live evidence frontier, and rebuilds its indexes.
Definitions, policy, lineage, fences, completed outcomes and counters survive;
nothing dynamic does.

The durable format is versioned and integrity-checked. Every header carries its
own CRC-32C so a torn payload can be told apart from a corrupted header. A
genuine torn tail — either too short to hold a record header, or a header whose
own integrity check passed but whose declared payload is absent — is truncated
to the last verified record boundary and reported through the recovery outcome
and a durable `TORN_TAIL_OBSERVED` record. Nothing else is ever truncated:
corrupt checksums, sequence regression, unsupported versions, impossible lengths
and trailing garbage are refused and the bytes are left exactly as found.

Snapshots are written to a staging file, flushed, renamed and the directory
synced. `dmf-cli verify` opens a store read-only and cannot create, append,
truncate or remove anything.

---

## 6. Protocol

Frames are fixed-header, length-prefixed, checksummed and strictly bounded:

```
offset  size  field
0       4     magic  'D','M','F','F'
4       2     protocol version
6       2     message type
8       2     flags (must be zero)
10      2     reserved (must be zero)
12      4     payload length  (refused above 1 MiB before allocation)
16      4     CRC-32C of bytes [0,16)
20      N     payload
20+N    4     CRC-32C of the payload
```

The decoder is total and sticky-failing: after any error every further call
fails and no byte is reinterpreted. Every request is bound to the session the
server minted, with a strictly increasing request sequence; a replayed or
regressed sequence is rejected and the connection is closed. Sessions carry a
role derived from the bearer token they presented — never from a role string they
claim.

---

## 7. Testing

```
ctest --test-dir build/release --output-on-failure
```

| Suite | Scope |
|---|---|
| `dmf_test_unit_domain` | guarantee algebra, envelope composition, policy selection, evidence aggregation, authority deltas, grant lifecycle, codec round-trips and malformed decodes |
| `dmf_test_protocol_codec` | every truncated frame prefix, oversized and impossible lengths, corrupted integrity fields, invalid enumerations, sticky failure, session binding, replay rejection, role enforcement, prompt shutdown |
| `dmf_test_persistence` | corrupt headers and payloads, every truncation length, unsupported versions, sequence regression, trailing garbage, torn-tail recovery, read-only opens, retention bounds, snapshot round-trip, accounting closure |
| `dmf_test_integration` | contention and priority, refusal and escalation, the authority ladder, duplicate and late attempts, expiry, capability and policy revalidation, restoration, restart lineage, proven infeasibility, restart determinism |
| `dmf_test_property_engine` | 400 seeded invariant instances, 200 order-independence instances, 3000 differential instances against an exhaustive reference solver, monotonicity, adversarial anti-greedy instances, bounded-search honesty, certificate soundness |
| `dmf_test_concurrency` | latch-coordinated multithreaded evaluation, concurrent grant lifecycles, shutdown releasing blocked sessions, interleaved revalidation, close during shutdown |
| `dmf_test_multiprocess` | real child processes over real loopback sockets: full lifecycle, hard kill after a durable commit, crash injection at three durable boundaries, single-writer lock, publisher death |
| `dmf_test_scale` | 500/1000/2000/4000 contracts, bounded retention, snapshot round-trip, growth ratios |

No test uses a timeout, a retry, or a sleep. The harness blocks indefinitely on
every wait, because a hang is a defect to diagnose rather than to abandon.

---

## 8. Evidence: REAL, SYNTHETIC, UNSUPPORTED

Every fixture used by the examples, the tools' default flow and the test suites
is **SYNTHETIC**. The runtime never measures the fabric.

| Claim | Evidence |
|---|---|
| Degradation never exceeds original authority | **REAL** — enforced in six independent places and exercised by every suite |
| Rigid obligations are never weakened | **REAL** — property suite plus adversarial instances |
| Weaker traffic absorbs degradation first | **REAL** — optimality proven against an exhaustive reference solver over 3000 seeded instances |
| UNKNOWN/STALE/CONFLICT/INVALID can never authorise | **REAL** — unit and integration suites |
| Durable lineage survives a restart, live authority does not | **REAL** — real process termination in `dmf_test_multiprocess` |
| Crash at a durable boundary is conservative | **REAL** — three injected boundaries, real processes |
| Multiprocess death and fresh-boot fencing | **REAL** — real OS processes over loopback sockets |
| Framed transport refuses malformed input | **REAL** — exhaustive prefix and corruption tests |
| Bounded retention and accounting closure at scale | **REAL** — 4000 contracts |
| Deterministic canonical serialisation | **REAL** — digest equality across shuffled inputs |
| Physical switch / NIC / RDMA / DPU / NVLink / multi-node behaviour | **UNSUPPORTED** — no such hardware is exercised, and no code path claims it |
| Cryptographically secure transport or authentication | **UNSUPPORTED** — deliberately out of scope; see the trust boundary below |
| UndefinedBehaviorSanitizer coverage | **UNSUPPORTED on this host** — MSVC provides no UBSan runtime; see section 9 |
| Cross-host coordination or consensus | **UNSUPPORTED** — not implemented by design |

---

## 9. Trust boundary and genuine limitations

* **The transport is plaintext loopback.** There is no confidentiality, no
  encryption and no cryptographic authentication. A bearer token is compared
  against a configured principal table; that is a capability check, not a
  security protocol. Do not expose the port off-host.
* **CRC-32C is integrity, not security.** It detects torn writes, accidental
  corruption and unsophisticated tampering. It does not authenticate a writer,
  cannot detect a deliberate rewrite, and provides no rollback protection.
* **One writer per store root.** A lock file makes a second coordinator refuse
  to open the same root, but there is no distributed coordination.
* **The allocator's optimality claim is bounded.** It proves the rigid selection
  optimal for the instance when the branch-and-bound search completes inside the
  policy node budget. When the budget is exhausted the plan is reported as
  `SEARCH_LIMIT_REACHED` and optimality is never claimed. `PROVEN_INFEASIBLE`
  is emitted only with an arithmetic certificate over the smallest demands.
* **Capability is only as good as its evidence.** DMF does not verify that a
  publisher's observation is true; it verifies that it is well formed, current,
  superseding, and internally consistent.
* **No hardware validation of any kind.** Nothing in this repository was run
  against a physical switch, NIC, RDMA fabric, DPU, SmartNIC or multi-node
  cluster, and no result should be read as if it were.
* **UndefinedBehaviorSanitizer** could not be exercised on this host: MSVC ships
  no UBSan runtime and no clang-cl compiler is installed. The CMake sanitizer
  probe reports the exact status at configure time.

---

## 10. Documentation

| Document | Contents |
|---|---|
| `docs/ARCHITECTURE.md` | systems boundary, layering, module map, allocator problem class |
| `docs/AUTHORITY.md` | identities, generations, bindings, the ladder, fencing, restart |
| `docs/PROTOCOL.md` | byte-level framing, message table, session rules, trust boundary |
| `docs/PERSISTENCE.md` | on-disk layout, recovery rules, crash points, CRC trust boundary |
| `docs/CONCURRENCY-AUDIT.md` | the ownership and call-path audit, item by item |
| `docs/EVIDENCE-MATRIX.md` | every claim classified REAL / SYNTHETIC / UNSUPPORTED |
| `docs/TESTING.md` | the harness, the no-timeout rule, and how to add a suite |

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
