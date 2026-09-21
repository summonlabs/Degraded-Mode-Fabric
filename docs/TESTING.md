# Degraded Mode Fabric - Testing

This document describes the test harness, every suite declared under `tests/`,
the tools that support testing, the measured results of the build matrix, and the
permanent rule that governs all of them.

## 1. The permanent rule: no test uses a timeout

No test, no suite, no harness wait and no build step in this repository is given a
timeout. A hang is a defect to be diagnosed; abandoning the step hides it.

Where the rule is enforced:

* `tests/CMakeLists.txt`: `add_test(NAME <suite> COMMAND <suite>)` is used
  without `set_tests_properties`, and no test is registered with a timeout
  property anywhere in the file. There are no ctest properties at all - no
  `WORKING_DIRECTORY`, `LABELS`, `SERIAL`, `DEPENDS` or fixtures.
* `scripts/build.ps1`: runs `ctest --test-dir <build> --output-on-failure` with
  no `--timeout` argument, and its header comment states that no step is given an
  artificial timeout. `scripts/closure.ps1` repeats the rule.
* `tests/testkit/testkit.hpp`: "Deliberately tiny: no third-party dependency, no
  timeout, no retry. A test that hangs is a defect in the runtime, so the harness
  never hides one."
* `tests/testkit/process.hpp`: "The harness blocks indefinitely on every wait: a
  hang is a defect, not something a watchdog should paper over." The
  implementation waits with `WaitForSingleObject(..., INFINITE)` on Windows and
  `waitpid(pid, &status, 0)` on POSIX.
* `tools/dmf_coordinator_main.cpp` publishes a ready-file barrier and a
  `DMF_READY` line precisely so a supervising test can block on a real event
  instead of inventing a timeout.

There are exactly two time-based assertions in the suites, and neither is a
timeout: `dmf_test_protocol_codec` asserts that coordinator shutdown takes less
than 5 seconds (`stop_seconds < 5.0`, printed as
`shutdown timing: stop=... client-close=...`), and `dmf_test_scale` asserts that
each larger instance costs no more than `previous * 6.0 + 1.0` seconds. Both are
upper bounds on observed work; neither can make a hung test pass.

Consequences that follow from the rule, and that a reviewer should apply:

* no `sleep`, `Sleep`, `usleep` or busy-wait polling loop to "let the other
  thread catch up"; use an explicit synchronisation point (a child's output line,
  a ready file, a blocking join, a `std::latch`). The concurrency suite uses
  `std::latch` barriers and atomic flags, and no suite contains a sleep;
* no retry loop around a flaky assertion;
* no test that passes only because it gave up waiting;
* if a test hangs, the correct action is to reproduce it with a debugger or a
  sanitizer run and fix the runtime, not to add a watchdog.

## 2. Harness

`tests/testkit` builds one static library, `dmf_testkit`
(`tests/testkit/testkit.cpp`, `tests/testkit/process.cpp`), linked by every
suite. It depends on `dmf_runtime` and on the repository's include root, and
`testkit.cpp` supplies `main`, so every suite links the runner's `main`.

```
Registration   DMF_TEST(suite, name) registers a case at static-init time.
Assertions     DMF_CHECK(condition)
               DMF_CHECK_EQ(actual, expected)
               DMF_CHECK_OK(expression)            expects Status::ok()
               DMF_CHECK_CODE(expression, code)    expects an exact ErrorCode
               DMF_CHECK_THROWS(expression)        (defined; no suite uses it)
Failure        dmf::test::fail() throws AssertionFailure{file:line: message}
Runner         dmf::test::run_all(argc, argv) - main() is defined in testkit.cpp
Order          tests are sorted by (suite, name) before running, so order is
               deterministic and independent of static-init order
Filter         an optional first argument is a substring match on "suite.name".
               The summary's "registered" count is the total registry, not the
               filtered selection
Reporting      "[ PASS ] suite.name" / "[ FAIL ] suite.name -> message", then
               "N passed, M failed, T registered"; exit code 1 if any failed
Scratch space  dmf::test::TempDir - creates dmf-<label>-<pid>-<n> under the
               current working directory and removes it in the destructor
Randomness     dmf::test::Rng - splitmix64 with an explicit seed; below(),
               u32(), coin(). Every property test seeds it explicitly so a
               failure reproduces exactly
Child processes dmf::test::ChildProcess - spawn(), read_line(), write_line(),
               wait(), terminate(), running(), identifier(); pipes for stdout
               (stderr is redirected to the same pipe) and stdin
Tool lookup    dmf::test::tool_path(name) resolves a tool built next to the test
               executable (its own directory, or one level up), so no absolute
               build path is compiled into a source file
```

The runner catches `AssertionFailure`, `std::exception` and any other exception per
test, records the failure and continues with the next test, so one broken case
does not hide the rest. A suite that throws during static initialisation or that
hangs still fails or hangs the run, which is the intended behaviour.

`TempDir` lives in the process working directory rather than the OS temp
directory, so the suites need a writable working directory (the test build
directory under ctest). Cleanup happens in the destructor only, so a killed test
process leaves its `dmf-<label>-<pid>-<n>` directory behind. No suite writes
outside a `TempDir`.

`ChildProcess` returns the exit code only from `wait()`/`terminate()`;
`terminate()` is the hard-kill primitive (`TerminateProcess(handle, 9)` on
Windows) and the destructor does not kill or reap a running child.

## 3. Suites

`tests/CMakeLists.txt` declares the `dmf_testkit` library and eight suites.
Every suite is registered with ctest under its target name, and the target name,
executable name, ctest name and source basename are identical.

| Target | Source | Cases | Scope |
| --- | --- | --- | --- |
| `dmf_test_unit_domain` | `tests/dmf_test_unit_domain.cpp` | 31 | Domain rules: guarantee weakening, envelopes, policy selection, evidence aggregation and capability derivation, authority deltas, fence-reason precedence, the authority ladder, grant transitions, codec round-trips and refusals, checked arithmetic, accounting closure |
| `dmf_test_protocol_codec` | `tests/dmf_test_protocol_codec.cpp` | 16 | Frame round-trip, every truncated prefix, sticky decode failure, CRC and bound refusals, request/acknowledgement pairing, and a live client against a real coordinator over loopback (lifecycle, impersonation, policy-change fencing, token refusal, blocked-session release) |
| `dmf_test_persistence` | `tests/dmf_test_persistence.cpp` | 14 | Journal and snapshot framing, writer lock, read-only open, CRC corruption refusal, torn-tail recovery and truncation to every prefix length, sequence regression, trailing garbage, retention bounds, restart semantics |
| `dmf_test_integration` | `tests/dmf_test_integration.cpp` | 12 | Full coordinator lifecycle over the framed server: degradation ordering, protected refusal and escalation, unknown evidence, the ladder, attempt replay, expiry fencing, capability and policy fencing, restoration, restart lineage, infeasibility, post-restart determinism |
| `dmf_test_property_engine` | `tests/dmf_test_property_engine.cpp` | 8 | Seeded property and differential tests: 400 invariant instances, 200 order-independence instances, 3000 differential instances against `reference_solve_rigid`, capacity monotonicity, adversarial greedy-breaking instances, bounded-search behaviour, certificate arithmetic, work-counter consistency |
| `dmf_test_concurrency` | `tests/dmf_test_concurrency.cpp` | 5 | 8 threads x 40 evaluations of one scope, six concurrent acquire/acknowledge attempt identities, shutdown releasing blocked sessions with an idempotent stop, interleaved revalidation and evaluation, close-while-stopping |
| `dmf_test_multiprocess` | `tests/dmf_test_multiprocess.cpp` | 5 | Real child processes and loopback sockets: a publisher and an agent driving a coordinator, a hard kill after a durable commit, one crash-injection boundary at each of the three ordinals, a refused second coordinator on one root, publisher death |
| `dmf_test_scale` | `tests/dmf_test_scale.cpp` | 3 | Behaviour at the documented bounds: 500/1000/2000/4000 contracts, 50 publication scopes, 500 sampled evaluations and up to 1200 grants with bounded retained tables; sustained grant pressure under retention bounds; snapshot round-trip over a large state |

The eight suites register 101 cases in total. All eight are declared in
`DMF_TEST_SUITES` in `tests/CMakeLists.txt`; the file also adds
`add_dependencies(dmf_test_multiprocess dmf_coordinator dmf_publisher dmf_agent
dmf_cli)` when the tools are built, so the child executables exist next to it.
`tool_path()` locates them relative to the test executable itself.

Measurement note: the property suite contains eight cases, including the three
seeded loops (400, 200 and 3000 instances). The protocol suite grew from 16 to 23
cases when the revalidation, replay, handshake-ordering, stale-vector,
publisher-incarnation, quota and independent-validator rules gained cases; the
current per-suite counts are the ones the built executables report, and the total
above is their sum.

## 4. Verification executables outside ctest

| Executable | Source | Role |
| --- | --- | --- |
| `dmf_selftest` | `tools/dmf_selftest_main.cpp` | In-process closure self test with no sockets and no flags. Runs the product proposition against one scratch store and then restarts against the same root, printing a PASS/FAIL line for each of 36 checks: policy refusal, full-service support, weaker traffic absorbing degradation, protected refusal and escalation, the ladder through acquire/acknowledge/apply, replay rejection, restoration, stale evidence, restart lineage, restart fencing, evidence not resurrected, closure across the restart. Ends with `SELFTEST OK failures=0` or `SELFTEST FAILED failures=<n>` and exits non-zero on any failure. |
| `dmf_example_degradation` | `examples/degradation_walkthrough.cpp` | Illustrative walkthrough: prints the decision explanation for five capability scenarios (full capability; degraded standard; degraded protected; partitioned fabric; no observation). It asserts nothing and is not a test target. |
| `dmf_consumer` | `examples/consumer/main.cpp` | Independent downstream consumer, built outside the source tree against the installed prefix with `find_package(DegradedModeFabric 1.0 CONFIG REQUIRED)`. Runs its own coordinator and prints `CONSUMER OK` or `CONSUMER FAILED`; exits 0 or 1. |
| `dmf-cli verify` | `tools/dmf_cli_main.cpp` | Offline store inspection. Opens the store through `StateStore::open` with `read_only = true` and sets no other `StoreConfig` field, prints the recovery outcome and detail, boot, term, journal records, boot count, retained counts, whether a policy is installed and whether accounting closure holds, and exits 4 when the store cannot be opened and 5 when a closure identity is violated (0 when closed). |
| `scripts/closure.ps1` | `scripts/closure.ps1` | Runs the closure matrix: release build and tests; debug build and tests; the AddressSanitizer configuration; install into `build/prefix`, build and run the consumer outside the source tree, then run the installed `dmf_selftest`, `dmf_coordinator --exit-after-ready` and `dmf_cli verify`; and optionally a fresh clone of the repository. No step is given a timeout. |

The other tools are operators of the protocol rather than test executables:
`dmf_coordinator` (server, `DMF_READY port=<decimal> boot=<hex> term=<hex>
recovery=<OUTCOME>`, a ready file containing the bare port, line commands
`quit`/`stop`/`shutdown`/`advance <n>`/`status`/`recovery`/`tick`),
`dmf_publisher` (publishes seven synthetic `Known` items for one scope),
`dmf_agent` (evaluates one contract, optionally acquires it and walks the ladder
with `--ack`/`--apply`/`--restore`/`--reapply-attempt`; exits 3 when the
decision is not authorising) and `dmf_cli` (live subcommands `status`,
`install-policy`, `register-contract`, `decide`, `acquire`, `restore`,
`fence`, `revalidate`, `version`). Their exact flags and exit codes are part of
the tools, not of this document; every tool prints deterministic `key=value`
lines so a supervisor can block on an output line.

## 5. Building and running

```
# configure + build + test + optional install (Windows/PowerShell)
scripts/build.ps1                       # Release, no sanitizer, runs ctest
scripts/build.ps1 -Config Debug
scripts/build.ps1 -Sanitizer asan       # -DDMF_ENABLE_ASAN=ON
scripts/build.ps1 -Sanitizer ubsan      # -DDMF_ENABLE_UBSAN=ON
scripts/build.ps1 -SkipTest
scripts/build.ps1 -Install              # installs into build/prefix

# the whole matrix, including the installed consumer and a fresh clone
scripts/closure.ps1 -Stage all

# equivalent manual form
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure    # no --timeout, by policy
```

Options that affect testing: `DMF_BUILD_TESTS` (ON), `DMF_BUILD_TOOLS` (ON -
required by `dmf_test_multiprocess`), `DMF_BUILD_EXAMPLES` (ON),
`DMF_WARNINGS_AS_ERRORS` (ON; a warning fails the build),
`DMF_ENABLE_ASAN`/`DMF_ENABLE_UBSAN` (OFF), `DMF_DISCOVER_SANITIZER_SUPPORT`
(ON). Requesting a sanitizer the toolchain cannot run is a configure-time
`FATAL_ERROR`, so a build is never silently uninstrumented; the probe result is
printed as `DMF sanitizer probe: ASan=<status> UBSan=<status>`.

Fault injection for the persistence and multiprocess suites is driven through the
coordinator tool:

```
dmf_coordinator --root DIR --allow-anonymous   --crash-before-record N | --crash-after-write-record N | --crash-after-record N
```

The ordinal counts every record the store has ever written in that directory
(1-based). A match terminates the child with exit code 70 and no unwinding. These
switches exist only for crash-consistency proofs; a store configured that way is
never a production configuration.

## 6. Measured results

The recorded host is Windows with the MSVC toolchain; the exact rows and their
evidence classes are in `docs/EVIDENCE-MATRIX.md`.

| Configuration | Result |
| --- | --- |
| Release, `/W4 /WX /permissive-` | PASS, 8 of 8 suites, 23.8 s of ctest wall time (host-load dependent; a verification re-run on the same host measured 28.7 s) |
| Debug, `/W4 /WX /permissive-` | PASS, 8 of 8 suites |
| AddressSanitizer (MSVC `/fsanitize=address` with `/Zi`), Release | PASS, 8 of 8 suites, no sanitizer report |
| UndefinedBehaviorSanitizer | UNSUPPORTED on this host; MSVC ships no UBSan runtime, no clang-cl is installed, and the CMake probe records it at configure time |
| clang-tidy 19.1.5 with the repository `.clang-tidy` over all of `src` | 0 diagnostics attributable to project sources |
| Install plus independent `find_package` consumer outside the source tree | PASS; the consumer prints `CONSUMER OK` |
| Installed tools (`dmf_selftest`, `dmf_coordinator`, `dmf_cli verify`) against the installed prefix | PASS |
| `dmf_test_multiprocess` | PASS, 5 cases, 0.8 s, real child processes and loopback sockets |
| `dmf_test_persistence` | PASS, 14 cases |
| `dmf_test_scale` | PASS, 3 cases, at 500/1000/2000/4000 contracts |

## 7. Defects found and fixed during hardening

Each entry states the defect as it existed and the mechanism that closed it.

1. **Parity-based request/acknowledgement classification.** `is_acknowledgement()`
   was `value % 2 == 0`, which classified every even-numbered request as an
   acknowledgement, so `Client::call` rejected `PUBLISH_EVIDENCE`,
   `EVALUATE_CONTRACT`, `ACQUIRE_AUTHORITY`, `QUERY_STATUS` and every other
   request with `InvalidArgument`; only the handshake worked. Replaced by an
   explicit switch over the thirteen acknowledgement types plus
   `acknowledgement_for()` and `request_for()`
   (`src/net/frame.cpp`, `src/net/client.cpp`).
2. **Accounting counters mutated off-lock.** Accept and session threads reported
   frame and session activity into the same non-atomic counter block the
   coordinator mutated under its own mutex, a data race with lost updates that
   could produce false closure violations. Closed by giving `Accounting` its own
   mutex, returning `counters()` by value, deleting copy and move, and routing
   every reporter through the single facade owned by the store
   (`include/dmf/accounting.hpp`, `src/runtime/accounting.cpp`).
3. **Evidence from a superseded publisher incarnation.** `EvidenceStore::publish`
   superseded live evidence on (scope, kind, publisher) without comparing
   `publisher_boot`, so an item from an older incarnation could replace a newer
   one. Closed by remembering the highest boot incarnation per publisher,
   refusing an older one with `ErrorCode::Stale` and marking supersession for a
   newer one (`src/runtime/evidence_store.cpp`).
4. **A recovered torn tail left in place.** Recovery detected a torn tail but
   left the partial record in the segment, so a valid-looking header in front of
   later bytes could turn a recoverable tail into an unrecoverable one on the
   next open. Closed by truncating the segment to the last verified record
   boundary, reporting `TornTailRecovered` with an explicit detail, and appending
   a durable `TornTailObserved` record
   (`src/store/state_store.cpp`).
5. **Quadratic byte writer.** `ByteWriter::reserve_for` reserved exactly the
   required size on every write, which pinned the vector's capacity to its size
   and made large documents quadratic in copying. Closed by checking the bound
   only and leaving growth to the container's geometric strategy; the method is
   now `ByteWriter::ensure` (`include/dmf/core.hpp`, `src/core/core.cpp`).
6. **`shutdown()`-released reads delaying thread termination.** Releasing a
   blocked `recv` with `shutdown()` delayed thread termination by about 120
   seconds on Windows. Closed by giving `Socket` and `Listener` a loopback
   datagram wakeup channel, waiting in `select()` on the descriptor and the
   channel, and sending one datagram to release; the blocking-read release no
   longer calls `shutdown` at all
   (`include/dmf/net.hpp`, `src/net/socket.cpp`, `src/net/server.cpp`).
7. **Argument parser rejected `--name value`.** The tool argument parser accepted
   only `--name=value`, so every documented invocation that passed a value as a
   separate token silently produced a switch with no value. Closed by consuming
   the following token as the value when it is non-empty and does not start with
   `-` (`tools/tool_support.hpp`).

Not recorded as a defect: the suggestion that the evidence vector carried inside
every decision record makes recording quadratic. The evaluation does embed the
evidence vector it was given in each `DecisionRecorded` record, but nothing in
this repository measures that growth as a performance defect, so it is not listed
above.

## 8. Rules for adding tests

1. No timeout, no sleep, no retry (section 1).
2. Use `TempDir` for every scratch file or store root, and never write outside it.
3. Seed `Rng` explicitly and print the seed in the failure message on any failure
   that depends on generated input.
4. Assert on values returned by the API, not on the absence of a crash.
5. For anything that crosses a process boundary, use `ChildProcess` and block on
   a real event (an output line, a ready file, a blocking wait), never on elapsed
   time.
6. Never bake a build path into a source file: use `tool_path()`.
7. When a test needs an impossible input (a genuinely torn file, a hostile frame,
   an exhausted identity domain), build it as an explicit fixture and say so in
   the test name; do not simulate it by weakening a production path.
8. Keep every fixture bounded by the constants in `include/dmf/core.hpp`; a test
   that needs more than the runtime allows is testing a different runtime.
9. Every new suite goes into `DMF_TEST_SUITES` in `tests/CMakeLists.txt` so it is
   registered with ctest automatically.
10. A case that depends on host timing is allowed only as an upper bound that a
    correct run cannot approach, and its bound must be stated in the test name or
    message.
