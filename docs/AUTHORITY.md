# Degraded Mode Fabric - Authority

This document describes how DMF represents identity, generation, binding, the
authority ladder, fencing, expiry and restart semantics. It is derived from
`include/dmf/authority.hpp`, `include/dmf/ids.hpp`, `include/dmf/grant.hpp`,
`include/dmf/decision.hpp`, `src/domain/authority.cpp`, `src/domain/grant.cpp`,
`src/engine/evaluator.cpp`, `src/runtime/coordinator.cpp` and
`src/store/state_store.cpp`.

## 1. Identity versus generation

Identity answers "which object". Generation answers "which version of the
dependency that made this legal". They are different types with different tags
and are never interchangeable:

```
StrongUInt<Tag>            value 0 is UNSET and never valid
  identity domains:        Service Contract Scope Node Policy Rule Grant Decision
                           Fence Evidence Publisher Session Attempt Subject Plan
                           Principal Envelope
  generation domains:      CoordinatorTerm Boot Process FabricGeneration
                           CapacityGeneration PolicyGeneration ContractGeneration
                           EvidenceGeneration SubjectGeneration Sequence
```

Rules the type system and the codecs enforce:

* `StrongUInt<Tag>::from_value` is the only constructor from an integer and each
  tag names a distinct domain, so a `ContractId` cannot be passed where a
  `ScopeId` is expected.
* `valid()` is `value != 0`. Every decoder reads a bare `u64` and then validates
  the surrounding object, so an unset identity is rejected rather than treated as
  a wildcard.
* `next()` returns `std::nullopt` at the top of the domain instead of wrapping.
  `IdAllocator<Tag>::allocate()` returns `nullopt` when exhausted, and every
  allocation site converts that into `ErrorCode::Exhausted` rather than reusing an
  identity. `IdAllocator::observe()` restores an allocator past a durable
  high-water mark after a restart and never moves it backwards.

Matching identity is not matching generation. Concretely, in this code base:

* A `ServiceContract` carries `id` **and** `generation`; an `EvidenceItem`
  carries `publisher` **and** `publisher_boot`; a `Grant` carries a full
  `AuthorityBinding`, which binds identity and generation for the contract, the
  subject and the scope, plus six runtime generations.
* `AuthorityBinding::classify()` compares every component and returns an
  `AuthorityDelta`. Contract and subject identity differences are reported as
  separate flags from their generation differences
  (`AuthorityDelta::Contract` vs `ContractGeneration`,
  `AuthorityDelta::Subject` vs `SubjectGeneration`).
* `Grant::validate()` refuses a grant whose degraded set is not a weakening of
  its original set and refuses a protected obligation that carries concessions.
* `Decision::validate()` refuses an authorising decision that carries no
  recommendation (its `max_authority` must be at least
  `AuthorityLevel::Recommended`), a full-service decision that rests on
  non-authoritative evidence, and a non-authorising decision that carries
  concessions.
* `Config`/`Incarnation` separation: `Incarnation { process, boot, boot_tick }`
  identifies one running process incarnation. `Coordinator::create()` advances
  the boot incarnation on every restart, so a record written by an earlier
  incarnation of the same process identity is not current authority.

## 2. AuthorityVector

```
AuthorityVector
  coordinator_term   CoordinatorTerm     monotonic coordinator term
  boot               BootIncarnation     process incarnation
  fabric             FabricGeneration    fabric topology generation
  capacity           CapacityGeneration  observed capability generation
  policy             PolicyGeneration    installed policy generation
  evidence           EvidenceGeneration  evidence frontier generation
```

* `valid()` requires all six components to be set. `valid()` is a structural
  check; it does not compare against anything.
* On the wire and on disk the vector is exactly six little-endian `u64` values in
  the order above (`src/codec/codec.cpp`), 48 bytes.
* `digest()` folds the six values through FNV-1a 64; `render()` produces the
  canonical `term=.. boot=.. fabric=.. capacity=.. policy=.. evidence=..` line.
* `current_authority(state)` in `src/runtime/coordinator.cpp` is the only place
  the vector is assembled for live use: term and boot come from the durable state,
  the fabric/capacity/evidence generations from the durable state, and the policy
  generation from the installed policy. A handshake reports it as it stands,
  including components that are still unset because no policy has been installed.

```
AuthorityBinding
  vector                AuthorityVector
  contract              ContractId
  contract_generation   ContractGeneration
  subject               SubjectId
  subject_generation    SubjectGeneration
  scope                 ScopeId
```

A binding is what a decision or grant commits to. `valid()` requires every
component to be set; `digest()` and the codec include all eleven values.

## 3. AuthorityDelta

```
AuthorityDelta (bit flags, u32)
  None                 0
  CoordinatorTerm      1 << 0
  Boot                 1 << 1
  Fabric               1 << 2
  Capacity             1 << 3
  Policy               1 << 4
  Evidence             1 << 5
  Contract             1 << 6
  ContractGeneration   1 << 7
  Subject              1 << 8
  SubjectGeneration    1 << 9
  Scope                1 << 10
```

`invalidates_authority(delta)` is true for every value other than None. The
reason that must be recorded when a grant is fenced for an authority change is
chosen by `fence_reason_for()` in this fixed order of precedence:

| Delta flag present | FenceReason |
| --- | --- |
| `Boot` | `BootAdvance` |
| `CoordinatorTerm` | `CoordinatorTermAdvance` |
| `Fabric` | `TopologyChange` |
| `Capacity` | `CapacityChange` |
| `Policy` | `PolicyChange` |
| `Contract`, `ContractGeneration`, `Subject`, `SubjectGeneration` or `Scope` | `ContractChange` |
| `Evidence` | `AuthorityUnprovable` |
| nothing matched (with a non-None delta) | `Manual` |

`to_string(AuthorityDelta)` renders a pipe-separated flag list, or `NONE`.

## 4. The authority ladder

```
AuthorityLevel
  None = 0
  Observed = 1       we hold an observation; not authority
  Eligible = 2       the subject could be degraded; not authorisation
  Recommended = 3    policy recommends; not authorisation
  Authorized = 4     committed degraded authority exists
  Acknowledged = 5   the subject claims to have accepted it; not effect
  Applied = 6        effect verified against evidence
```

`is_authority(level)` is true at Authorized and above. `is_verified_effect(level)`
is true only at Applied. The ordering is numeric, so a comparison is a ladder
position comparison.

What the code actually assigns:

| Producer | Condition | Level |
| --- | --- | --- |
| `evaluate_contract` | authorising outcome (Full or Degraded) | `Recommended` |
| `evaluate_contract` | outcome is Invalid | `None` |
| `evaluate_contract` | non-authorising, contract is not rigid, capability is Known | `Eligible` |
| `evaluate_contract` | every other non-authorising outcome | `Observed` |
| `Grant::authority_level` | `GrantState::Issued` | `Authorized` |
| `Grant::authority_level` | `GrantState::Acknowledged` | `Acknowledged` |
| `Grant::authority_level` | `GrantState::Applied` | `Applied` |
| `Grant::authority_level` | Fenced, Expired or Revoked | `None` |

The consequence is the separation the ladder exists for: an evaluation never
recommends authority it does not hold. A decision that supports its contract says
`Recommended`; a decision for a contract that is inside the degradation regime
but not authorised says `Eligible`; a decision that proves nothing says
`Observed`. Only a committed grant reaches `Authorized`, and only an accepted,
verified effect reaches `Acknowledged` and `Applied`.

## 5. Grant lifecycle

```
GrantState
  Issued = 1  Acknowledged = 2  Applied = 3   (live)
  Fenced = 4  Expired = 5  Revoked = 6        (terminal)
```

`grant_transition_allowed(from, to)` permits exactly these forward moves:

| From | To |
| --- | --- |
| Issued | Acknowledged, Applied, Fenced, Expired, Revoked |
| Acknowledged | Applied, Fenced, Expired, Revoked |
| Applied | Fenced, Expired, Revoked |
| Fenced, Expired, Revoked | nothing |

A transition to the same state is rejected, and no terminal state has an outgoing
transition. The same table is applied on replay, so an illegal transition in a
journal record fails recovery with `ErrorCode::InvalidState` instead of being
tolerated.

A grant also carries a monotonic `sequence` and a `last_attempt` `AttemptId`.
`acknowledge()` and `report_applied()` reject any attempt identity that is not the
current one with `ErrorCode::ReplayDetected`, and each accepted step rotates both
values, so an old acknowledgement or effect report cannot be replayed after the
state has moved on.

## 6. Fencing

`fence_grant_locked()` in `src/runtime/coordinator.cpp` is the single fence path
for a running coordinator. It:

1. finds the grant in the durable grant table and refuses a missing
   (`NotFound`) or already-terminal (`InvalidState`) grant;
2. allocates a `FenceId`;
3. records `prior` = the grant's existing binding and `current` = the same
   binding with `vector` replaced by the current authority vector, so the record
   shows exactly which generations differed;
4. selects the durable record type and terminal state: `Expiry` becomes
   `GrantExpired`/`Expired`, `Revocation` and `Restored` become
   `GrantRevoked`/`Revoked`, everything else becomes `GrantFenced`/`Fenced`;
5. appends the `FenceRecord` to the journal and only then moves the in-memory
   grant state and the grant-table index;
6. updates accounting through `apply_record` (`fences_total` plus the per-outcome
   counter).

`Coordinator::create()` writes its own pre-restart fences rather than calling
that helper: one `GrantFenced` record per grant that was live when the previous
incarnation stopped, with `FenceReason::BootAdvance`, `AuthorityDelta::Boot`
and the grant's own binding in both `prior` and `current`; the grant index is
rebuilt from the durable table afterwards.

Fences are triggered by:

* `revalidate()` (and the automatic sweeps): every live grant is checked for
  expiry first, then its binding is re-classified against the current authority
  vector;
* `install_policy()`: a policy generation change sweeps the live grants;
* `publish_evidence()`: a material capability change (the derived capability
  digest for the scope changed) advances the fabric, capacity and evidence
  generations and then sweeps;
* `Coordinator::create()`: every grant that was live when the process stopped is
  fenced with `FenceReason::BootAdvance` and `AuthorityDelta::Boot`;
* the explicit `fence()` API, the `FENCE_GRANT` protocol message, and
  `request_restoration()` when restoration is Proven
  (`FenceReason::Restored`).

An expired grant is also fenced lazily: `acknowledge()` and `report_applied()`
check `expired_at(now)` first and, when it is true, commit an expiry fence and
return `ErrorCode::Stale`.

## 7. Revalidation semantics

A revalidation sweep (`sweep_locked`) classifies each live grant against the
current authority vector and takes exactly one of four actions:

| Condition | Action |
| --- | --- |
| `grant.expired_at(now)` | fence with `FenceReason::Expiry` |
| a change to boot, term, policy, contract, contract generation, subject, subject generation or scope | fence with `fence_reason_for(delta)` |
| only the observed capability changed, and the re-evaluated decision is not authorising, or its approved set does not satisfy `weakens_or_equals(decision.approved, grant.degraded)` | fence with `fence_reason_for(delta)` |
| only the observed capability changed, and the re-evaluated approved set does satisfy that relation | append a durable `GrantRebound` record (record type 21) that rebinds the grant to the current generation set; the grant stays live |

The capability branch is what makes restoration reachable. A capability change is
not by itself a reason to deny authority: the runtime re-evaluates the registered
contract the grant names (its contract generation is unchanged, or the earlier
branch would have fenced the grant) and keeps the grant alive exactly when what it
promised is still serviceable. Because the fabric, capacity and evidence
generations advance on every material capability change, a surviving grant is
rebound to those generations, and the rebind is durable, so a restart cannot
resurrect the old binding. A rebind that cannot be committed fences the grant
with `FenceReason::AuthorityUnprovable` rather than leaving it bound to a stale
generation set.

Two consequences follow. A favourable capability change does not fence, so a
degraded grant can survive long enough for `request_restoration()` to prove it
and fence it with `FenceReason::Restored`. And a grant can never be carried
across a change on the strength of its identity alone: the survival test is a
fresh evaluation compared against the exact degraded set the grant promised.

The sweep is also the only place a grant is checked against the clock outside
`acknowledge()` and `report_applied()`. `RevalidateReport` counts
`evaluated`, `fenced` and `expired`; a rebound grant is counted in
`evaluated` only, because it neither fenced nor expired.

## 8. Expiry

* `Grant::expired_at(now)` is `now > expires_tick`. A grant is valid exactly at
  its expiry tick and invalid one tick later.
* `Decision::expires_tick` is set only for authorising outcomes:
  `now + max(policy.default_ttl_ticks, policy.minimum_ttl_ticks)` with checked
  addition (saturating at the top of the tick domain). A non-authorising decision
  expires at `now`.
* `Policy::validate()` requires both TTLs to be non-zero and
  `minimum_ttl_ticks <= default_ttl_ticks`, so "no expiry" is not expressible.
* A grant is checked against the clock at: acknowledgement, effect report, and
  every revalidation sweep listed above.
* Revalidation is also explicit: `Coordinator::revalidate(now)` and the
  `REVALIDATE` message return `{evaluated, fenced, expired}`. The sweep takes a
  snapshot of the live id list first, so a state transition during the sweep
  cannot invalidate the iteration.
* Freshness of evidence is a separate budget:
  `FreshnessWindow::fresh()` rejects an observation from the future and any
  observation older than `policy.evidence_freshness_ticks`. The window is
  enforced on evaluation, capability queries and restoration; it is deliberately
  not enforced when a policy is installed or when evidence is published, because
  those paths compare capability digests rather than authorise service.

## 9. Boot incarnation and coordinator term

* A restart is an incarnation change. `Coordinator::create()` reads the durable
  state, computes `next_boot = state.boot.next()` (or 1 for a fresh store) and
  `next_term = state.term.next()` (or 1), and commits both in a single
  `RecordType::BootAdvanced` record containing `{boot, term, last_commit_tick}`.
* `apply_record` for `BootAdvanced` enforces: boot and term must be set; a
  previously set boot must strictly advance (`boot <= state.boot` is
  `SequenceRegression`); a previously set term must not regress
  (`term < state.term` is `SequenceRegression`). Boot and term therefore both
  advance on restart, and the term can only advance further.
* The standalone `RecordType::CoordinatorTermAdvanced` record requires a strict
  increase over the durable term.
* Every authenticated request must carry the current term and boot: `on_request`
  compares the envelope against the durable state and returns
  `ErrorCode::Stale` on a mismatch. This is what stops a client that survived a
  restart from continuing to act under the previous incarnation's authority.
* The handshake response returns the current term and boot, and the client stamps
  them onto every request; a client therefore has to re-handshake after a
  restart.
* `Incarnation` also carries `boot_tick`, the logical tick at which the
  incarnation began. Ticks are logical and monotonic; they are never wall-clock
  time and are never compared across incarnations.

## 10. Restoration

`request_restoration()` evaluates the grant's recorded preconditions (or
regenerates them from the contract and policy when the grant carries none) and
returns a `RestorationEvaluation`. The outcome is `Proven` only when every
precondition is Satisfied; an Unsupported or blocking NotSatisfied precondition
yields `Refused`, any other NotSatisfied precondition yields `NotProven`, and
any Indeterminate precondition yields `Indeterminate`. A terminal grant is
refused with the reason matching its state. `RestorationEvaluation::validate()`
re-checks the outcome against the precondition list.

Two limits must be stated precisely. The runtime calls
`evaluate_restoration` with `fence_active = false`, so the `NoActiveFence`
precondition is always Satisfied on the live path; the precondition exists,
is evaluated, and is currently never made to fail by the coordinator. And
`EscalationClear` is likewise evaluated as Satisfied unconditionally
(`src/engine/restoration.cpp`).

## 11. Why "matching identity is not matching generation"

The same service, subject, scope or publisher identity can be reused across
versions, restarts and reconfigurations. Authority that was derived from one
version of a dependency must not survive a change to it. The code makes the
distinction at five points:

* identity and generation are separate strongly typed fields, and
  `AuthorityBinding::classify()` reports them as separate delta flags, so an
  operator can see whether the same contract was redefined or a different
  contract now occupies that identity;
* a `Decision` and a `Grant` bind all six runtime generations plus contract and
  subject generations, and the bindings are durable, so the comparison can be
  re-made after a restart;
* a capability-only change is answered by re-evaluating the contract and
  comparing the result with the grant's own degraded set, never by assuming that
  identity implies entitlement;
* `RestorationEvaluation` re-checks `ContractUnchanged` (identity and
  generation of contract and subject) and `AuthorityUnchanged` (the six runtime
  generations) as preconditions, and reports
  `ReasonCode::ContractGenerationChanged`, `AuthorityGenerationChanged`,
  `BootIncarnationAdvanced` or `CoordinatorEpochAdvanced` rather than a generic
  failure;
* evidence carries `publisher` and `publisher_boot`, and the live evidence
  frontier enforces the ordering: `EvidenceStore` remembers the highest boot
  incarnation seen per publisher, refuses an item from an older incarnation with
  `ErrorCode::Stale` ("evidence comes from a superseded publisher incarnation"),
  and marks supersession when an item arrives from a newer one. The incarnation is
  also bound into the evidence digest and durable record, so a restart cannot
  resurrect it. See `docs/CONCURRENCY-AUDIT.md` item 13 for the two limits that
  remain: replacement of a publisher's previous items still requires a matching
  scope and kind, and `EvidenceStore::clear()` resets the live vector but not the
  incarnation map (which keeps an older incarnation rejected, the safe
  direction).
