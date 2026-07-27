# Resume Cancelled Progressive H.264 Indexes In Place

Status: Accepted
Date: 2026-07-28

## Context

The minimum DSL already accepts one progressive declaration,
`@index(progressive) sequence<Element> name = scan(h264_start_code);`. The
H.264 Annex B analyzer executes that declaration with bounded record-count,
inspected-position, and mapped-byte batches. It retains a monotonic scanner
cursor, queued records, a pending mapper, stable tree nodes, and the next NAL
and logical-view identifiers between calls.

That state currently supports ordinary `in-progress` continuation only. This
profile projects the entry sequence directly onto the analysis root rather
than creating a separate sequence node. A cancellation marks that root
`cancelled`, terminalizes the analyzer, and makes every later call replay
`cancelled`. This falls short of ADR-0004's resumable bounded-work requirement
and the analysis model's explicit `cancelled -> indexing` transition.

Cancellation may be observed at different boundaries. A scanner cancellation
does not fail or finish the scanner and retains its cursor and pending start
code. A cancellation while decoding or mapping a record instead publishes that
record as a terminal partial NAL. In particular, ADR-0025 deliberately makes a
cancelled EBSP-to-RBSP mapper terminal and publishes its committed prefix.
Retrying that mapper in place would require mutable node locations, removal or
reordering of already published children, and a different mapper contract.

Cross-process recovery is also premature. A persisted checkpoint cannot be
safely rebound with only the current path-like source identity. It needs the
later source fingerprint, exact rule-package identity and content hash, cache
namespace, schema version, and durable tree/index storage. Those contracts are
assigned to M5 and the later SQLite cache slice.

## Decision

The accepted recovery slice resumes a cancelled H.264 progressive index in the
same analyzer object. It adds no DSL syntax and does not create a serializable
checkpoint. Keeping the analyzer object is the checkpoint: it preserves the
exact source object, compiled typed program, analysis tree, scanner state,
queued records, deferred scanner result, monotonic identifiers, and any
pending work that has not already been terminalized.

The analyzer will expose an explicit `resumeAfterCancellation` operation. It is
accepted only when all of the following hold:

- the analyzer is terminal with `Cancelled` status;
- the analysis root representing the entry sequence is currently
  `MaterializationState::Cancelled`; and
- a supplied replacement cancellation token has not already been requested.

The replacement token may be absent, making subsequent work uncancellable
until another token is supplied by a later recovery. Reusing the requested
one-way token is rejected. A rejected recovery changes no tree state, scanner
state, cursor, queue, diagnostic, identifier, or terminal result. `Complete`,
`SourceError`, `ResourceLimit`, and `InvalidRule` are not recoverable through
this operation.

Successful recovery performs these state changes without reading source data,
creating a node, or advancing the scanner:

1. Remove every diagnostic with code `Cancelled` attached directly to the
   analysis root and transition that root from `cancelled` to `indexing`.
2. Replace the analyzer and scanner cancellation token.
3. Clear the analyzer's cached cancelled terminal result.
4. If the deferred scanner result itself is `Cancelled`, consume that result so
   the next batch continues the same scanner. Preserve every other deferred
   scanner result and every queued record.

The core analysis tree will provide a restricted resume operation for step 1. It
accepts only a cancelled node, removes only diagnostics whose code is
`Cancelled` from that node, and transitions it to `indexing`. It does not alter
descendant state, descendant diagnostics, or any non-cancellation diagnostic.
This prevents a resumed root from continuing to display a stale root-level
pause while retaining real partial-result evidence below it.

The start-code scanner allows its owning analyzer to replace the cancellation
token without resetting scan state. Its cursor, total inspected count, pending
start-code offset and length, trailing-zero run, buffer, finished flag, and
failed flag remain unchanged. The next scan batch therefore cannot replay a
record that was already returned or skip an uninspected start-code candidate.

Recovery is sequence-level, not retry of an already published element:

- If cancellation was observed by the scanner, no scanner failure is made
  terminal. Recovery continues from its preserved cursor and pending boundary.
- If cancellation occurred while decoding a NAL header or mapping RBSP, the
  existing analyzer contract first publishes that NAL, its completed children,
  and any committed RBSP prefix as a cancelled partial result. That record is
  considered committed and is never decoded or mapped again by index recovery.
  Recovery continues with the next queued or unscanned record.
- Previously returned node identifiers are never replayed in a later batch.
  New records append once using the preserved `nextNalUnitIndex` and
  `nextViewId` state.

When a resumed scan reaches its normal end, the analysis root becomes
`materialized`. A descendant NAL or RBSP region that was committed as
`cancelled` remains cancelled, so the completed index still reports partial
results and is not fully materialized. This distinguishes "the index reached
the end of the source" from "every indexed element was complete." Repeated
cancel/resume cycles are supported under the same rules.

All existing per-batch budgets and cancellation intervals remain unchanged.
Recovery itself consumes no record, inspected-position, mapped-byte,
instruction, or node budget. The first later analysis batch is bounded exactly
like any other batch.

Persistent recovery remains a separate feature. A future durable checkpoint
must validate the source fingerprint and exact rule identity before restoring
scanner or tree state, and may use a different representation from these
private in-memory fields. This decision promises no checkpoint serialization
format and does not copy the analysis tree merely to resume it.

## Consequences

Users and hosts can stop a long Annex B index, retain every published result,
and later continue toward the end of the same source without rescanning the
completed prefix or renumbering nodes. Scanner-boundary cancellation is fully
continued; cancellation inside one NAL keeps that NAL visibly partial while
allowing later NAL units to be indexed.

The append-only tree and terminal mapper contracts stay intact. Recovery does
not require node deletion, child reordering, mutable source locations, or an
unbounded replay/deduplication table.

The in-place boundary is intentionally narrower than crash recovery. Losing
the analyzer object still loses its resumable state until source/rule identity
and durable cache contracts are implemented.

## Considered Options

- Serialize scanner, mapper, tree, and queue state now: unsafe without source
  fingerprints, exact rule identity, cache versioning, and durable storage, and
  it would pull M5 and SQLite work into this runtime slice.
- Restart scanning at byte zero and deduplicate records: repeats source work,
  requires a second stable-identity index, and risks duplicates around a
  partially committed record.
- Resume a cancelled mapper and rewrite its published RBSP node: conflicts with
  ADR-0025 terminal replay and the append-only analysis-tree contract.
- Leave the root `indexing` when cancellation is observed: hides the explicit
  cancelled state and diagnostic required by the analysis model.
- Keep the root cancellation diagnostic after recovery: makes a current
  indexing or materialized root continue to present a stale pause as an error.
