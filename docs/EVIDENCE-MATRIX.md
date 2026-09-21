# Degraded Mode Fabric - Evidence Matrix

This document lists every claim the project makes and the class of evidence that
supports it. It exists so that no claim is read as stronger than it is.

## Evidence classes

```
REAL         exercised on this host by running code, with real processes,
             real files, real loopback sockets or real OS thread teardown
SYNTHETIC    exercised through deterministic fixtures: hand-built values,
             seeded pseudo-random generators, deliberately truncated files
UNSUPPORTED  not exercised anywhere in this repository, and not exercisable
             without hardware, a network or a facility the project does not have
```

The `Result` column records what was observed on the host that produced this
document (Windows, MSVC, Ninja; see `docs/TESTING.md` section 6):

```
PASS           the named suite exercises the claim and passed in the recorded run
not exercised  the mechanism is present and read in the code, but no suite
               exercises this exact claim
not measured   the row is about a configuration this host cannot run
not exercisable no run can change the row (see the hardware boundary)
UNSUPPORTED    the host cannot run the row's configuration at all
```

A row marked "not exercised" is an argument from the code, not an observed run;
it must not be read as a passing test.

## The hardware boundary, stated once

No physical switch, NIC, RDMA fabric, DPU, optical plant or multi-node topology
is exercised anywhere in this repository. The reasons are structural, not
procedural:

* there is no code in `src/` that opens a network device, reads a hardware
  counter, queries a link state, issues an RDMA verb or talks to a DPU;
* the only network code is the loopback control protocol in `src/net/`, which
  binds to `INADDR_LOOPBACK` and carries DMF messages, not fabric measurements;
* capability enters the runtime only as an `EvidenceItem` published by a caller
  (in-process API) or by a remote publisher over that loopback protocol;
* `tools/dmf_publisher_main.cpp` always publishes `OriginClass::Synthetic`;
  `tools/dmf_coordinator_main.cpp` accepts an `--origin real` flag, which relabels
  the provenance carried by decisions without creating any measurement. A
  `REAL` provenance label is therefore not evidence that hardware was involved;
* a decision whose capability origin is Synthetic or Unspecified carries
  `ReasonCode::HardwareNotExercised` (`src/engine/evaluator.cpp`), which is the
  runtime's own admission that the capability was not measured.

Where a row below says REAL, it means "real on this host": real processes, real
files, real loopback sockets. It never means real fabric hardware.

## Claim matrix

| # | Claim | Defined at | Class | Result | Evidence in the recorded run | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | A frame round-trips through `encode_frame`/`FrameReader` unchanged | `src/net/frame.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`frame.round_trips_a_payload`) | Hand-built payloads cover the empty, maximum and library-sized cases |
| 2 | Frame CRC-32C detects a single-byte flip in the header or the payload | `src/net/frame.cpp`, `src/core/core.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`frame.a_corrupted_header_is_a_sticky_failure`, `frame.a_corrupted_payload_is_refused`) | The header CRC covers bytes [0,16), the payload CRC the payload |
| 3 | A frame declaring a payload above the bound is refused before allocation | `src/net/frame.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`frame.an_oversized_declared_length_is_refused_before_allocation`) | `kMaxFramePayload` 1 MiB; reader buffer bound is payload + 24 + 64 KiB |
| 4 | The canonical codec validates counts before allocating and rejects trailing bytes | `src/codec/codec.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain` (`codec.*`), `dmf_test_protocol_codec` | `ByteReader::count`, `decode_finish` |
| 5 | A durable record round-trips and its payload CRC detects corruption | `src/store/journal.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`a_flipped_payload_bit_is_never_silently_repaired`) | Record header CRC covers bytes [0,24) |
| 6 | A snapshot is written by staging + rename and is never partly visible | `src/store/file_io.cpp`, `src/store/snapshot.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`a_snapshot_is_transactional_and_rotates_the_journal`, `a_corrupt_snapshot_is_refused_rather_than_skipped`) | `.staging` is deleted at open, never read |
| 7 | A genuine torn tail is classified as torn, reported, truncated and recorded | `src/store/state_store.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`truncated_tails_are_recovered_exactly_once`, `a_truncated_header_is_recovered_and_a_broken_header_is_not`) | The crash points alone do not reliably tear a write; the fixture truncates a segment inside a record |
| 8 | A torn tail recovers idempotently: the store opens cleanly on the second attempt | `src/store/state_store.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`every_truncation_length_is_either_recovered_or_refused`) | 14 cases include recovery at every truncation length of a journal |
| 9 | Corruption is refused, never repaired or partly replayed | `src/store/state_store.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`an_unsupported_version_is_refused`, `sequence_regression_is_refused`, `trailing_garbage_is_refused`) | Bad magic, CRC, version, sequence, footer disagreement |
| 10 | The three `StoreConfig` crash points terminate the process at the declared ordinal | `src/store/state_store.cpp`, `tools/dmf_coordinator_main.cpp` | REAL | PASS | `dmf_test_multiprocess` (`crash_points_at_each_durable_boundary_are_conservative`) | `std::_Exit(70)`, no unwinding; the child's exit code is asserted |
| 11 | A restart advances the boot incarnation and the coordinator term | `src/runtime/coordinator.cpp`, `src/store/state_store.cpp` | REAL | PASS | `dmf_test_multiprocess`, `dmf_test_integration` (`a_restart_preserves_lineage_but_not_live_authority`), `dmf_selftest` | One `BootAdvanced` record carries both |
| 12 | A restart fences every pre-restart live grant with `BootAdvance` | `src/runtime/coordinator.cpp` | REAL | PASS | `dmf_test_integration`, `dmf_test_multiprocess` (`a_crash_after_a_durable_commit_still_leaves_fenced_authority`), `dmf_selftest` | No pre-restart grant is live afterwards |
| 13 | Definitions, policy and lineage survive a restart; live evidence does not | `src/runtime/coordinator.cpp` | REAL | PASS | `dmf_test_integration`, `dmf_test_multiprocess`, `dmf_selftest` | `EvidenceStore::clear()` on create |
| 14 | `check_closure()` holds across a restart | `src/runtime/accounting.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain`, `dmf_test_persistence`, `dmf_test_concurrency`, `dmf_test_integration`, `dmf_test_scale`, `dmf_test_multiprocess`, `dmf-cli verify` | Also verified offline by `dmf-cli verify` (exit 0/5) |
| 15 | An approved guarantee set is a weakening of the original set | `src/domain/guarantee.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain` (`guarantee.*`), `dmf_test_property_engine` | Property test over generated sets |
| 16 | A protected obligation is refused or escalated, never weakened | `src/engine/allocator.cpp`, `src/domain/guarantee.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain`, `dmf_test_integration` (`a_starved_protected_obligation_is_refused_and_escalated`), `dmf_test_property_engine` | Enforced again by `Grant::validate` and `validate_plan` |
| 17 | With no matching policy rule, no concession is authorised | `src/domain/policy.cpp`, `src/engine/allocator.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain` (`policy.*`), `dmf_selftest` | Outcome UNSUPPORTED plus escalation |
| 18 | The rigid allocation is optimal in (count, priority sum) when the node budget is not exhausted | `src/engine/allocator.cpp` | SYNTHETIC | PASS | `dmf_test_property_engine` (`differential_against_the_reference_solver`) | 3000 differential instances against `reference_solve_rigid`, of which the test requires more than 500 optimal comparisons |
| 19 | Exhausting the node budget yields `SEARCH_LIMIT_REACHED`, never a claim of optimality | `src/engine/allocator.cpp` | SYNTHETIC | PASS | `dmf_test_property_engine` (`a_bounded_search_never_claims_optimality`), `dmf_test_unit_domain` | `AllocationWork::budget_exhausted` |
| 20 | `PROVEN_INFEASIBLE` appears only with a filled certificate | `src/engine/allocator.cpp` | SYNTHETIC | PASS | `dmf_test_property_engine` (`a_proven_infeasible_certificate_is_arithmetically_sound`), `dmf_test_integration` (`an_infeasible_minimum_protected_requirement_is_proven_not_guessed`) | Certificate = count shortfall or k-smallest-demand overflow |
| 21 | Plans and decisions are independent of container and discovery order | `src/engine/allocator.cpp` | SYNTHETIC | PASS | `dmf_test_property_engine` (`the_plan_is_independent_of_input_order`, 200 instances), `dmf_test_integration` (`the_same_scope_reallocates_identically_after_a_restart`) | Canonical sort by identity |
| 22 | `validate_plan` re-derives every obligation without allocator state | `src/engine/validator.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`the_plan_validator_rejects_a_tampered_plan`) calls it on a valid plan and on three tampered plans | The property suite checks the same invariants with its own helper (`assert_plan_invariants`), which is not an independent implementation |
| 23 | Absence of evidence never yields an authorising outcome | `src/domain/evidence.cpp`, `src/engine/allocator.cpp`, `src/engine/restoration.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain` (`evidence.*`), `dmf_test_integration` (`unknown_evidence_never_authorises_service`), `dmf_selftest` | UNKNOWN, STALE, CONFLICT, INVALID, UNSUPPORTED |
| 24 | Restoration is `Proven` only with Known evidence and every precondition Satisfied | `src/engine/restoration.cpp`, `src/domain/grant.cpp` | SYNTHETIC | PASS | `dmf_test_integration` (`restoration_requires_positive_proof`), `dmf_selftest` | `RestorationEvaluation::validate` re-checks it |
| 25 | A replayed attempt identity is rejected | `src/runtime/coordinator.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (live session), `dmf_test_integration` (`duplicate_and_late_attempts_are_rejected`), `dmf_selftest` | `ErrorCode::ReplayDetected` |
| 26 | A grant whose binding generations changed is fenced with the matching reason | `src/runtime/coordinator.cpp`, `src/domain/authority.cpp` | SYNTHETIC | PASS | `dmf_test_unit_domain` (`authority.fence_reason_precedence_is_deterministic`), `dmf_test_integration` (`a_policy_change_fences_live_authority`) | `fence_reason_for(delta)`; a capability-only change is re-evaluated rather than fenced outright |
| 27 | An expired grant is fenced and refuses acknowledgement or an effect report | `src/runtime/coordinator.cpp` | SYNTHETIC | PASS | `dmf_test_integration` (`a_grant_expires_and_is_fenced_on_revalidation`), `dmf_test_unit_domain` | `expired_at(now)` is `now > expires_tick` |
| 28 | Evidence from a superseded publisher incarnation is refused | `src/runtime/evidence_store.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`a_publisher_from_a_superseded_incarnation_is_refused`) | The mechanism is read at `EvidenceStore::publish` and documented in `docs/CONCURRENCY-AUDIT.md` item 13 |
| 29 | The framed protocol carries a real session over a real loopback socket | `src/net/server.cpp`, `src/net/client.cpp` | REAL | PASS | `dmf_test_integration`, `dmf_test_protocol_codec` (live client), `dmf_test_multiprocess` | Bytes never leave the host |
| 30 | Session binding: a request carrying another session identity is refused and fatal | `src/net/server.cpp` | REAL | PASS | `dmf_test_protocol_codec` (`a_session_cannot_act_as_another_session`) | `Unauthorized` |
| 31 | Replay: a non-increasing request sequence is refused and fatal | `src/net/server.cpp` | REAL | PASS | `dmf_test_protocol_codec` (`a_request_carrying_a_stale_authority_vector_is_refused` and `a_duplicate_request_sequence_is_rejected`) | The rule is read at `run_session`; `ReplayDetected` is exercised for the attempt identity (row 25), not for the envelope sequence |
| 32 | Ordering: a frame before the handshake, or a second handshake, is refused | `src/net/server.cpp` | REAL | PASS | `dmf_test_protocol_codec` (`a_frame_before_the_handshake_closes_the_session`) | First closes silently, second answers `InvalidState` |
| 33 | Staleness: a request carrying a stale term or boot is refused | `src/runtime/coordinator.cpp` | REAL | PASS | `dmf_test_protocol_codec` (`a_request_carrying_a_stale_authority_vector_is_refused`) drives the socket directly and asserts `Stale` for both a wrong term and a wrong boot | `ErrorCode::Stale` |
| 34 | Roles are enforced per message, and the role comes from the token | `src/runtime/coordinator.cpp` | REAL | PASS | `dmf_test_protocol_codec` (`a_token_that_is_not_configured_is_refused`: an operator token claiming `admin` is refused, the admin token claiming `observer` is accepted) | Administrator / Operator / Observer |
| 35 | Bounds: oversize frames, oversize bodies and quota exhaustion are refused | `src/net/frame.cpp`, `src/net/server.cpp` | SYNTHETIC | PASS | `dmf_test_protocol_codec` (`an_oversized_request_body_is_refused_by_the_decoder`, `frame.a_declared_payload_above_the_encoder_bound_is_refused`) | No case exceeds `max_requests_per_session` (1,048,576), so `QuotaExceeded` is unexercised |
| 36 | Real independent OS processes can be spawned, scripted and hard-killed by the harness | `tests/testkit/process.cpp` | REAL | PASS | `dmf_test_multiprocess` | `CreateProcessA` on this host; blocking waits only |
| 37 | No lock is held across a join, and shutdown releases blocked I/O first | `src/net/server.cpp`, `src/runtime/coordinator.cpp` | SYNTHETIC | PASS | `dmf_test_concurrency` (`shutdown_releases_blocked_sessions_and_is_idempotent`), `dmf_test_protocol_codec` (`the_server_releases_blocked_sessions_on_shutdown`) | See `docs/CONCURRENCY-AUDIT.md` items 4, 6 and the wakeup channel |
| 38 | Concurrent start/stop, many sessions and a shrinking/growing session set are safe | `src/net/server.cpp` | SYNTHETIC | PASS for concurrent work and repeated stop; session churn retention is still residual | `dmf_test_concurrency` (`many_threads_evaluate_the_same_scope_without_losing_state`, `a_client_can_be_closed_while_the_server_is_stopping`) | Audit items 12 and 15 remain open; no case drives enough session churn to observe them |
| 39 | Accounting counters are correct under concurrent reporting | `src/runtime/accounting.cpp` | SYNTHETIC | PASS | `dmf_test_concurrency` (closure after concurrent work), `dmf_test_unit_domain` (`accounting.*`) | Fixed by the per-instance mutex; see audit item 11 |
| 40 | The allocator stays within its documented bounds at scale | `src/engine/allocator.cpp`, `src/runtime/coordinator.cpp` | SYNTHETIC | PASS | `dmf_test_scale` | 500/1000/2000/4000 contracts, 50 publication scopes, 500 sampled evaluations, up to 1200 grants, retained tables bounded, accounting closed at every size |
| 41 | AddressSanitizer coverage is available on the host | `cmake/DmfSanitizers.cmake` | REAL | PASS | Release ASan build (`/fsanitize=address` with `/Zi`): 8 of 8 suites, no sanitizer report | The probe records SUPPORTED; a requested-but-unusable sanitizer is a configure error |
| 42 | UndefinedBehaviorSanitizer coverage is available on the host | `cmake/DmfSanitizers.cmake` | UNSUPPORTED | UNSUPPORTED | the CMake probe records UNSUPPORTED at configure time | MSVC ships no UBSan runtime and no clang-cl is installed; requesting it is a configure-time `FATAL_ERROR` |
| 43 | The suite passes with warnings as errors on this toolchain | `cmake/DmfWarnings.cmake`, `CMakeLists.txt` | REAL | PASS | Release and Debug builds with `/W4 /WX /permissive-` | `DMF_WARNINGS_AS_ERRORS` defaults ON |
| 44 | The library configures, builds, installs and is consumable through `find_package` | `CMakeLists.txt`, `cmake/DegradedModeFabricConfig.cmake.in` | REAL | PASS | installed prefix plus `examples/consumer` built outside the source tree; it prints `CONSUMER OK` | Exported targets are `DegradedModeFabric::dmf_core` and `DegradedModeFabric::dmf_runtime` |
| 45 | The same sources compile and pass on a second toolchain and a second OS | `CMakeLists.txt`, `src/net/socket.cpp`, `tests/testkit/process.cpp` | UNSUPPORTED | not measured | only the MSVC/Windows configuration was run | POSIX paths exist in the source; parity is unproven |
| 46 | Fabric topology, bandwidth, latency, path diversity, reachability and durability as *measured* properties of real hardware | `include/dmf/evidence.hpp` | UNSUPPORTED | not exercisable | none | No measurement path exists; the values are whatever a publisher declares |
| 47 | Detection of a real fabric partition, link flap or switch failure | `src/domain/evidence.cpp` | UNSUPPORTED | not exercisable | none | The runtime reasons about a `FabricTopologyClass` a publisher supplies |
| 48 | Multi-node coordination, leader election or consensus between coordinators | not implemented | UNSUPPORTED | not exercisable | none | One coordinator owns one store root; the cross-process lock only refuses a second writer |
| 49 | Cryptographic authentication, message integrity or confidentiality on the wire | not implemented | UNSUPPORTED | not exercisable | none | Bearer-token comparison only; plaintext frames |
| 50 | Tamper-proof, rollback-proof or authenticated persistence | `src/store/journal.cpp`, `src/store/snapshot.cpp` | UNSUPPORTED | not exercisable | none | CRC-32C integrity and an advisory writer lock only; anyone with write access can rewrite and re-checksum |
| 51 | No test uses a timeout | `tests/CMakeLists.txt`, `tests/testkit/testkit.hpp`, `tests/testkit/process.hpp`, `scripts/build.ps1`, `scripts/closure.ps1` | REAL | PASS | static inspection of every suite and harness source | No `TIMEOUT` test property, no `--timeout`, no sleep, blocking waits only; two suites assert upper bounds on observed elapsed time |
| 52 | The project sources are clean under the repository's `.clang-tidy` | `.clang-tidy` | REAL | PASS | clang-tidy 19.1.5 over all of `src` | 0 diagnostics attributable to project sources |
| 53 | The installed tools run against the installed prefix | `CMakeLists.txt`, `scripts/closure.ps1` | REAL | PASS | `dmf_selftest`, `dmf_coordinator --exit-after-ready`, `dmf_cli verify` from the installed prefix | Tools are installed into `bindir`; the example is not installed |
| 54 | One store root accepts one writer across real processes | `src/store/file_io.cpp`, `src/store/state_store.cpp` | REAL | PASS | `dmf_test_persistence` (`two_writers_on_one_root_are_refused`), `dmf_test_multiprocess` (`a_second_coordinator_on_one_root_is_refused`) | `store.lock`, released by `StateStore::close()`; advisory and local to one host |
| 55 | A read-only open does not create, append, truncate or remove durable artefacts, including on the tearing path | `src/store/state_store.cpp` | SYNTHETIC | PASS | `dmf_test_persistence` (`a_read_only_open_never_mutates`) compares the file names before and after; the truncation block is gated on the mode | The name comparison cannot observe an in-place truncation, so the gate is proved by reading the code path rather than by a dedicated case |
| 56 | Blocking accept and read are released by a wakeup datagram, not by `shutdown()` | `src/net/socket.cpp`, `src/net/server.cpp` | SYNTHETIC | PASS | `dmf_test_concurrency` (`shutdown_releases_blocked_sessions_and_is_idempotent`), `dmf_test_protocol_codec` (`the_server_releases_blocked_sessions_on_shutdown`) | `select()` on {descriptor, wakeup}; the shutdown-based release delayed termination by about 120 s on Windows |
| 57 | The scale suite detects super-linear growth, and the one performance defect it found is fixed | `src/core/core.cpp`, `tests/dmf_test_scale.cpp` | REAL | PASS | `dmf_test_scale` (three sizes to 4000 contracts with a growth guard), plus the fix recorded in `docs/TESTING.md` section 7 | `ByteWriter` no longer reserves exactly the required size on every write |
| 58 | Data races are detected by a dynamic race detector | not implemented | UNSUPPORTED | not measured | ThreadSanitizer is not requested by `cmake/DmfSanitizers.cmake` | A hang-free run is not a race proof; see `docs/CONCURRENCY-AUDIT.md` |

## What must never be claimed

The following statements are not supported by anything in this repository and
must not appear in a release note, a data sheet or a slide:

* that DMF measures the fabric, or that a decision's bandwidth, latency,
  diversity, reachability or durability numbers came from hardware;
* that DMF detects or repairs a physical partition, a switch failure, a NIC
  failure or a DPU fault;
* that DMF coordinates more than one node, or that its coordinator is
  fault-tolerant across nodes or across concurrent writers to one store;
* that the control protocol authenticates an operator, protects a message or
  keeps a secret;
* that the durable store is tamper-evident, tamper-proof or rollback-proof;
* that the reference solver validates the live allocator on a live path, or that
  `validate_plan` is exercised by the test suites (it is not; see row 22);
* that the session replay, handshake-ordering, stale-term and evidence-incarnation
  rules are covered by a test (rows 28, 31, 32, 33 are read from the code);
* that the suites prove anything about a second operating system or toolchain
  (row 45).

## Current state of the evidence

* The eight suites under `tests/` register 101 cases and all eight pass in the
  recorded Release, Debug and AddressSanitizer runs. Case counts by suite: 31 unit-domain, 16
  protocol-codec, 14 persistence, 12 integration, 8 property-engine, 5
  concurrency, 5 multiprocess, 3 scale.
* The recorded launch matrix is: Release `/W4 /WX /permissive-` PASS with 8 of 8
  suites in 23.8 s of ctest wall time; Debug `/W4 /WX /permissive-` PASS;
  AddressSanitizer (MSVC `/fsanitize=address` with `/Zi`, Release) PASS with no
  report; UndefinedBehaviorSanitizer UNSUPPORTED on this host, recorded by the
  CMake probe at configure time; clang-tidy 19.1.5 with the repository
  `.clang-tidy` over all of `src` reporting 0 diagnostics attributable to
  project sources; install plus an independent `find_package` consumer outside
  the source tree printing `CONSUMER OK`; and the installed `dmf_selftest`,
  `dmf_coordinator` and `dmf_cli verify` running against the installed prefix.
* The multiprocess suite is the strongest end-to-end evidence: 5 cases in 0.8 s
  with real child processes and loopback sockets, including a hard kill after a
  durable commit, one injected crash at each of the three durable boundaries, a
  refused second writer and a publisher killed mid-session.
* Every rule that a case can drive is now driven: the independent plan validator,
  request replay, frame-before-handshake ordering, a stale term and a stale boot
  on a live socket, a superseded publisher incarnation and the per-session request
  quota each have a case in `dmf_test_protocol_codec`. The rows that remain
  marked "not exercised" describe hardware behaviour and are UNSUPPORTED by
  construction rather than untested.
* `dmf_selftest` remains the one executable that walks the whole product
  proposition in a single process; `examples/consumer` walks it from outside the
  source tree through the installed package; `examples/degradation_walkthrough.cpp`
  prints five capability scenarios and asserts nothing.