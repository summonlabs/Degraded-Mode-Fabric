# Degraded Mode Fabric - Persistence

This document specifies the on-disk layout, record framing, recovery rules and
trust boundary of the durable store, as implemented in
`src/store/state_store.cpp`, `src/store/journal.cpp`, `src/store/snapshot.cpp`,
`src/store/file_io.cpp` and the internal headers `src/store/journal.hpp`,
`src/store/snapshot.hpp`, `src/store/file_io.hpp`, with the public surface in
`include/dmf/store.hpp`.

## 1. Layout, file naming and the writer lock

One directory per store (`StoreConfig::root`, created when absent) holds:

```
<root>/
  snapshot-<20-digit-sequence>.dmfs    owned durable projection at that sequence
  journal-<20-digit-base>.dmfj         append-only segment whose base is that sequence
  store.lock                           the exclusive writer lock (empty marker file)
  <any>.staging                        staging file for an incomplete atomic write
```

* Names are produced by `snapshot_file_name()` and `journal_file_name()`, which
  format the sequence with `%020llu` (20 zero-padded decimal digits).
* `parse_snapshot_file_name()` / `parse_journal_file_name()` accept only the exact
  prefix and suffix, a non-empty decimal field of at most 20 digits, and reject
  overflow with checked arithmetic. A file whose name does not parse is ignored
  by recovery and is never deleted by the store.
* A writable open takes an exclusive operating-system lock on `store.lock`:
  `_locking(_LK_NBLCK)` on Windows, `flock(LOCK_EX | LOCK_NB)` elsewhere. A second
  writer on the same root is refused with `ErrorCode::AlreadyExists` ("the store
  root is locked by another process"). `StateStore::close()` releases the lock, so
  a store root becomes writable again as soon as its owner closes it, even before
  the process exits. A read-only open takes no lock.
* At open, every file whose name ends in `.staging` is removed. The rename is the
  commit point of an atomic write, so a staging file is always debris and never
  authoritative.
* The newest snapshot sequence present wins. The active journal is exactly
  `journal-<newest snapshot sequence>.dmfj`; for a store with no snapshot the
  active base is 0 and the active journal is `journal-00000000000000000000.dmfj`.
* After the active pair is durable, recovery deletes superseded segments
  (base below the active base) and superseded snapshots (sequence below the
  newest snapshot).

Segment rotation is driven by `StoreConfig`: `max_journal_bytes` (default 8 MiB)
and `max_records_per_segment` (default 200,000). `rotation_due()` reports the
condition (always false on a read-only store); the runtime does not rotate by
itself - a rotation happens inside `take_snapshot()`.

### Read-only inspection

`StoreConfig::read_only` opens a store for inspection. On that path the store
takes no lock, does not create the root, does not remove staging debris, does not
create or rotate a segment, does not delete superseded segments or snapshots,
does not append a `TornTailObserved` record, and `close()` writes no journal
footer. Every mutating call - `append`, `append_batched`, `flush`,
`take_snapshot` - fails with `ErrorCode::InvalidState` ("store is opened
read-only"). `dmf-cli verify` opens its store this way and sets no other
`StoreConfig` field.

A read-only open is non-mutating in full, including the tearing path. The
recovery block is gated on the mode: a writable open truncates a genuine torn
tail to the last verified record boundary and records a durable
`TORN_TAIL_OBSERVED` marker, while a read-only open reports
`TornTailRecovered` with the detail "a torn journal tail is present; a read-only
open leaves it untouched" and creates, appends, truncates and removes nothing.
That is exactly the guarantee `include/dmf/store.hpp` states. The persistence
suite's read-only case exercises the ordinary path; the truncating path is
covered by the writable recovery cases.

## 2. Journal framing

All integers are little-endian. CRC-32C is the Castagnoli polynomial as
implemented in `src/core/core.cpp`.

```
journal file = header(28) record* [footer(28)]

header (28 bytes)
  offset  size  field
  0       4     magic "DMFJ"
  4       4     version, u32, must equal kDurableFormatVersion (1)
  8       8     boot incarnation, u64, must be non-zero
  16      8     base_sequence, u64
  24      4     CRC-32C over bytes [0, 24)

record = record header(28) payload(payload_length) payload CRC(4)

record header (28 bytes)
  offset  size  field
  0       4     magic "DMFR"
  4       4     version, u32, must equal 1
  8       2     RecordType, u16, must be a defined type
  10      2     flags, u16, must be 0
  12      8     sequence, u64, must be the successor of the previous record
  20      4     payload_length, u32, at most kMaxRecordPayload (4 MiB)
  24      4     CRC-32C over bytes [0, 24)

payload CRC (4 bytes)
                CRC-32C over the payload bytes only

footer (28 bytes), written only by a clean close
  offset  size  field
  0       4     magic "DMFE"
  4       4     version, u32, must equal 1
  8       8     record_count, u64, records in this segment
  16      8     last_sequence, u64
  24      4     CRC-32C over bytes [0, 24)
```

CRC coverage, exactly:

| Framed unit | CRC covers | CRC stored at |
| --- | --- | --- |
| journal header | bytes 0..23 (magic, version, boot, base_sequence) | bytes 24..27 |
| record header | bytes 0..23 (magic, version, type, flags, sequence, payload_length) | bytes 24..27 |
| record payload | the payload | the 4 bytes that follow the payload |
| journal footer | bytes 0..23 (magic, version, record_count, last_sequence) | bytes 24..27 |

The record header carrying its own CRC is what lets recovery distinguish a torn
payload from a corrupted header. The declared payload length is refused before
allocation, so a hostile header cannot drive allocation.

## 3. Snapshot framing

```
snapshot file = header(32) payload(payload_length) footer(16)

header (32 bytes)
  offset  size  field
  0       4     magic "DMFS"
  4       4     version, u32, must equal 1
  8       8     sequence, u64, must equal the sequence in the file name
  16      8     payload_length, u64, at most max_snapshot_bytes
  24      4     payload_crc, CRC-32C over the payload
  28      4     CRC-32C over bytes [0, 28)

payload
                the canonical encoding of DurableState

footer (16 bytes)
  offset  size  field
  0       4     magic "DMFF"
  4       4     version, u32, must equal 1
  8       4     payload_crc, must equal the header's payload_crc
  12      4     CRC-32C over bytes [0, 12)
```

`decode_snapshot()` validates in this order: minimum size, magic, header CRC,
version, declared payload length against the configured maximum, exact total
length (`header + payload_length + footer == file size`), payload CRC, footer
magic, footer version, footer agreement with the header, footer CRC. The file
name sequence must also equal the header sequence
(`StateStore::open` checks this separately).

Snapshot payload: `DurableState` in declaration order
(`src/store/state_store.cpp`, `encode(ByteWriter&, const DurableState&)`):

```
u64 boot
u64 term
u64 last_commit_tick
Policy
u32 contracts        + ServiceContract[contracts]
u32 grants           + Grant[grants]
u32 fences           + FenceRecord[fences]
u32 decisions        + Decision[decisions]
u32 restorations     + RestorationEvaluation[restorations]
EvidenceVector historical_evidence      (historical only, never live)
u64 evidence_generation
u64 capacity_generation
u64 fabric_generation
AccountingCounters
u64 journal_sequence
u64 journal_records
u64 boot_count
u64 grant_id_high_water
u64 decision_id_high_water
u64 fence_id_high_water
u64 evidence_id_high_water
u64 plan_id_high_water
```

The collection counts are decoded through `ByteReader::count(kMaxPlanEntries)`,
so they are bounded before allocation. An all-zero policy is legal durable state
meaning "no policy has been installed"; any other policy must validate, and the
snapshot is rejected otherwise. `decode_finish()` rejects trailing bytes.

## 4. Record types

The vocabulary is append-only: new types get new numbers and existing numbers
never change meaning. `is_valid(RecordType)` accepts 1..21.

| # | Type | Payload |
| --- | --- | --- |
| 1 | `ContractRegistered` | `ServiceContract` |
| 2 | `ContractRetired` | `u64` contract id |
| 3 | `PolicyInstalled` | `Policy` |
| 4 | `FabricGenerationAdvanced` | `u64` generation |
| 5 | `CapacityGenerationAdvanced` | `u64` generation |
| 6 | `EvidencePublished` | `EvidenceItem` |
| 7 | `GrantIssued` | `Grant` |
| 8 | `GrantAcknowledged` | `u64` grant, `u64` tick, `u64` attempt, `u64` sequence |
| 9 | `GrantApplied` | as `GrantAcknowledged` |
| 10 | `GrantFenced` | `FenceRecord` |
| 11 | `GrantExpired` | `FenceRecord` |
| 12 | `GrantRevoked` | `FenceRecord` |
| 13 | `DecisionRecorded` | `Decision` |
| 14 | `BootAdvanced` | `u64` boot, `u64` term, `u64` tick |
| 15 | `CoordinatorTermAdvanced` | `u64` term |
| 16 | `TornTailObserved` | `u64` discarded byte count |
| 17 | `RestorationProven` | `RestorationEvaluation` (must carry outcome Proven) |
| 18 | `RestorationRefused` | `RestorationEvaluation` |
| 19 | `AccountingCheckpoint` | `AccountingCounters` |
| 20 | `EvidenceGenerationAdvanced` | `u64` generation |
| 21 | `GrantRebound` | `u64` grant id, `AuthorityBinding` |

Replay applies the same `apply_record()` function that live mutation uses, so a
restarted process reconstructs exactly the state its predecessor committed. The
rules enforced during replay include:

* `ContractRegistered` replaces an existing contract with the same identity or
  appends a new one, bounded by `max_retained_contracts`;
* `GrantIssued` refuses a reused grant identity (`AlreadyExists`) and refuses to
  exceed the live grant bound (`CapacityExceeded`);
* `GrantAcknowledged`/`GrantApplied`/`GrantFenced`/`GrantExpired`/
  `GrantRevoked` must name a retained grant and must be a legal lifecycle
  transition, otherwise replay fails with `NotFound` or `InvalidState`;
* `GrantRebound` must name a retained, non-terminal grant and must keep the same
  contract, contract generation, subject, subject generation and scope; it
  replaces the grant's whole binding, which is how a surviving grant is moved to
  the current generation set without extending what it promises. A terminal grant
  cannot be rebound;
* `BootAdvanced` requires boot and term to be set, boot to strictly advance and
  term not to regress;
* `CoordinatorTermAdvanced` requires a strict increase;
* `EvidencePublished` inserts into the historical vector, normalises it, and
  raises the evidence generation and evidence id high-water mark;
* `AccountingCheckpoint` needs a mutable counter block.

Bounded retention (`prune`): live grants are never pruned. Terminated grants are
kept newest-transition-first inside the budget
`max_retained_grants - live_grant_count`; fences and decisions are trimmed to
`max_retained_fences` / `max_retained_decisions` newest-first; restoration
records reuse `max_retained_decisions`; historical evidence is rebuilt to 3/4 of
`max_retained_evidence` when it reaches that bound. Pruning runs from
`append_batched` once a table exceeds twice its bound, and from
`take_snapshot`. Grant and fence pruning increments `grants_pruned` /
`fences_pruned`; decision pruning computes a dropped count but does not account
for it.

## 5. Recovery rules

`StateStore::open()` is the only recovery entry point. It returns a
`Result<unique_ptr<StateStore>>`: on any refusal it returns a `Status` and no
store is produced. On a writable open the only on-disk effect a refused open can
have is the truncation of a detected torn tail, which happens before the
fresh-journal refusal check described below; every other file change (staging
removal, superseded segment and snapshot removal) happens only after the active
pair is durable, so a refused open never rewrites durable content. A read-only
open follows the same replay rules and, as section 1 states, can also truncate a
torn tail, but writes nothing else.
`RecoveryOutcome` values are `FreshStore`, `CleanReopen`, `TornTailRecovered`,
`Corrupt`, `VersionUnsupported`, `IntegrityFailure`, `SequenceFailure`; only the
first three are reachable as an outcome, because the failure outcomes are
reported through the returned `Status` code instead of being stored.

Replay of the active segment proceeds record by record:

1. the file must be at least 28 bytes and its header must decode, with a magic,
   CRC, format version and a non-zero boot incarnation; the header's
   `base_sequence` must equal the base in the file name;
2. each record header must decode (magic, CRC, version, defined type, zero flags,
   payload length within 4 MiB) and its `sequence` must be exactly the expected
   successor of the previous record, otherwise `SequenceRegression`;
3. the payload and its CRC must be present and must verify, otherwise
   `IntegrityFailure`;
4. the payload must decode completely with no trailing bytes;
5. `apply_record()` must accept the record.

A genuine torn tail versus corruption:

| Condition | Classification |
| --- | --- |
| fewer than 28 bytes remain and the segment has no snapshot | torn tail |
| fewer than 28 bytes remain and a snapshot was loaded | `Corrupt` ("journal ends with a truncated record header") |
| a decoded record header's payload plus trailer extends beyond end of file | torn tail |
| bad magic | `Corrupt` |
| header or payload CRC mismatch | `IntegrityFailure` |
| unknown record type or enum | `InvalidEnum` |
| sequence not the expected successor | `SequenceRegression` |
| decoded record leaves trailing bytes | `TrailingGarbage` |
| a footer whose magic, CRC, record count or last sequence disagrees with the replayed records | `Corrupt` |
| bytes after the last record that are neither a footer nor a record | `TrailingGarbage` |

On a torn tail the store:

* sets `recovery = TornTailRecovered` with the detail "discarded a torn journal
  tail and truncated the segment; every committed record was retained";
* increments `torn_tails_recovered` in the accounting block;
* closes the read handle and reopens the segment for append, truncates it to the
  byte offset that follows the last record that verified, flushes and closes it,
  and (on a writable open) reopens it for append. The truncation is the point of
  the exercise: leaving a valid-looking header in front of a later record's bytes
  would turn a recoverable tail into an unrecoverable one on the next open;
* on a writable open, appends a `TornTailObserved` record carrying the number of
  discarded bytes, so a later reader sees the observation durably instead of
  assuming a clean shutdown. A read-only open truncates but does not append.

On a clean end the store sets `CleanReopen`, distinguishing in
`recovery_detail` between "segment closed cleanly" (a valid footer was found)
and "segment replayed to its end". A segment with no snapshot and no footer is
also `CleanReopen` ("journal replayed without a snapshot"). A read-only open with
no active segment reports `CleanReopen` when a snapshot was loaded and
`FreshStore` otherwise, with the detail "read-only open: no journal segment is
present".

### The never-silently-truncate rule

* Corruption is never repaired, never rewritten and never partly accepted: the
  first offending record aborts the whole open with a `Status`, so a caller
  cannot observe a partly replayed store or a silently repaired one.
* The only bytes the store ever discards are a *genuine torn tail* - either the
  remaining bytes cannot hold a record header at all, or a header that passed its
  own integrity check declares a payload the file does not contain. In that one
  case the segment is truncated back to the last fully verified record boundary.
* On a writable open that truncation is never silent. It is reported three ways:
  the open returns `RecoveryOutcome::TornTailRecovered`, `recovery_detail` says
  that the segment was truncated and that every committed record was retained, and
  a `TornTailObserved` record carrying the discarded byte count is appended
  durably. A read-only open reports the outcome and the detail but appends
  nothing.
* Nothing else is deleted except `.staging` debris and superseded segments or
  snapshots that are strictly older than the active pair, and those deletions
  happen only after the active pair is durable.

## 6. Snapshot transactional replacement

`write_file_atomic()` is the only way a file is created or replaced:

1. remove any existing `<path>.staging`;
2. create `<path>.staging` and write the whole payload, retrying short writes;
3. flush (`_commit` on Windows, `fsync` elsewhere);
4. close;
5. rename the staging file over the target;
6. sync the parent directory (a no-op on Windows, which has no portable directory
   flush).

Any failure before the rename removes the staging file and leaves the previous
target untouched. A crash between create and rename leaves only a staging file,
which the next open deletes.

`take_snapshot()` performs, in order: flush the journal; prune; append an
`AccountingCheckpoint` record carrying the current counter block (this is what
preserves the counters that no other record derives - protocol, session, journal
and evidence activity - across a hard kill); encode `DurableState` into the
snapshot payload (bounded by `max_snapshot_bytes`, capped at
`kMaxDocumentBytes`); write the snapshot atomically under
`snapshot-<journal_sequence>.dmfs`; create the successor segment
`journal-<journal_sequence>.dmfj` atomically with the durable boot incarnation in
its header and switch the append handle to it; then remove the previous segment
and any older snapshots; then sync the directory. The new segment is created
before the predecessor is removed, so a crash in the middle leaves two readable
segments and a snapshot that both point at the same sequence.

`close()` releases the writer lock, writes the journal footer and closes the
handle; it is idempotent via an internal `closed` flag, and the destructor calls
it. A read-only store releases nothing (it holds no lock) and writes no footer.

## 7. Restart semantics

`Coordinator::create()` performs the following durable steps before any request
is served:

1. open (or create) the store, applying the recovery rules above; the coordinator
   always opens writable and raises `max_retained_grants`/`max_retained_decisions`
   to at least `max_live_grants`/`max_decisions`;
2. observe the durable id high-water marks (decision, grant, fence, evidence and
   plan) so no identity is ever reused, and resume the logical clock from
   `last_commit_tick` (or tick 1);
3. build the grant index from the replayed durable state (it must exist before
   the fence sweep, which looks every live grant up through it);
4. advance the boot incarnation and the coordinator term in a single
   `BootAdvanced` record: `boot = state.boot.next()` (or 1) and
   `term = state.term.next()` (or 1), with the current tick as
   `last_commit_tick`;
5. bootstrap the fabric, capacity and evidence generations to 1 if they were
   unset;
6. fence every grant that was live when the previous incarnation stopped: one
   `GrantFenced` record per grant with `FenceReason::BootAdvance` and
   `AuthorityDelta::Boot`, so pre-restart authority is visibly terminated rather
   than silently reused;
7. rebuild the grant index again (the fence sweep mutated the durable grant
   table) and rebuild the contract and scope indexes from the durable state;
8. clear the live evidence frontier. Historical evidence is retained in the
   snapshot for lineage and forensics only, and the runtime never derives current
   capability from it after a restart;
9. set `policy_installed` from the validity of the stored policy. A store with no
   installed policy therefore reports `policy_installed = false` and refuses
   evaluation with `InvalidState` until a policy is installed.

The recovery outcome and detail are reported through `Coordinator::recovery()`,
the `QUERY_STATUS` body, `dmf-cli status`, the `dmf-cli verify` output and the
coordinator tool's `DMF_READY ... recovery=...` line.

## 8. Integrity trust boundary

The store's integrity mechanism is CRC-32C. That is an integrity check, not a
security control:

* it detects torn writes, truncation at record granularity, accidental
  corruption and bit flips with the usual CRC-32C detection properties;
* it provides no authentication: nothing in the format binds a record to a
  writer, a process or a key, so anyone with write access to the directory can
  append, edit or replace records and simply recompute the CRCs;
* it provides no confidentiality: payloads are plaintext on disk, including
  contract definitions, evidence and accounting;
* it provides no rollback protection: an older snapshot plus an older segment is
  a self-consistent store, and nothing detects that it is stale;
* it does not detect a torn tail that happens to end on a record boundary, and it
  cannot distinguish "the writer intended this" from "the writer wrote this";
* the writer lock is advisory and local to one host's file system; it prevents
  two cooperating runtimes from interleaving appends, and it is not an access
  control. The only access control is the file system's.

Deployments that need authenticity, confidentiality or rollback protection must
add them outside this store (for example by placing the root on an authenticated
volume and controlling who can open it). Where the code compares secrets - the
bearer token in the control protocol - it does so outside the durable format; the
store itself has no secrets.

## 9. Fault injection

`StoreConfig` exposes three crash points. Each is an ordinal compared against
`state.journal_records + 1`, counting every record the store has ever written
(1-based, across segments and restarts of the same store directory):

```
crash_before_record        before anything reaches the descriptor
crash_after_write_record   after write(2) but before the flush
crash_after_record         after the flush, before the caller sees a result
```

A match calls `std::_Exit(70)`: no stack unwinding, no destructors, no stream
flush - a hard kill at that byte of progress. The coordinator tool exposes the
same knobs as `--crash-before-record`, `--crash-after-write-record` and
`--crash-after-record`. A store configured this way is a test fixture; it is
never a production configuration.

Two consequences must be stated precisely:

* `crash_after_write_record` kills the process after `write(2)` returned. On a
  normal file system the bytes are already in the page cache, so the file often
  contains the full record and recovery reports a clean replay. A *torn tail*
  (a partial record) is therefore not reliably produced by the crash points
  alone; producing one requires truncating the segment to a byte offset inside a
  record, which is a deliberately synthetic fixture.
* `crash_after_record` kills after the flush, so the record is durable and
  recovery replays it.

## 10. Residual hazards in the store

These are properties of the current code, not plans:

1. **Segment byte accounting after truncation.** After a tail recovery the store
   reports `bytes_in_segment` from the file size it measured *before* the
   truncation, so the reported segment length overstates the file until the next
   snapshot or rotation. The value is advisory (rotation uses
   `max_journal_bytes` against it).
2. **Boot incarnation in a recovery-created segment.** A journal that must be
   created during recovery (snapshot present, segment absent) is written with
   `JournalHeader::boot = 1` rather than the durable boot incarnation, unlike
   the segment created by `take_snapshot()`, which uses the durable value.
3. **Retention floors versus caller configuration.** Retention relies on
   `max_retained_grants`/`max_retained_decisions` being at least the live bounds;
   `Coordinator::create()` raises them, but a direct `StateStore` user can
   configure a store whose live grant table exceeds its retention budget.
4. **Decision pruning is not accounted.** Pruning a decision vector does not
   increment any accounting counter (the dropped count is computed and
   discarded), while grant and fence pruning does account for what it removed.
   The closure identities do not cover decisions, so this is a reporting gap
   rather than a closure failure.
5. **Read-only truncation.** As stated in section 1, a read-only open still
   truncates a detected torn tail even though `include/dmf/store.hpp` states that
   a read-only open truncates nothing.
