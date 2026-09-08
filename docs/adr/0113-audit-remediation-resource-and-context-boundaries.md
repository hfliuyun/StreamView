# ADR-0113: Audit Remediation Resource and Context Boundaries

- Status: Proposed; implementation and scoped independent review complete,
  cross-platform delivery validation pending
- Date: 2026-09-07

## Context

The September 7 audit reproduced source replacement during Save As, rejected
rule content remaining installed, sample-table reads consuming the container's
lifetime budget, missing configuration context, and duplicate sample retention.
Passing the existing matrix does not close these defects. Phase 5/6 acceptance
must be rechecked before Phase 7 release work proceeds.

## Decisions

### Saving and Installing

The read-only media and exact-rule-identity contracts remain unchanged.
`SessionDocument::save` checks source/destination aliases before opening and
before committing. Rule installation stages catalog registration in a private
copy, installs only accepted content, then publishes the staged catalog. A
rejected conflict writes no package; a store error publishes no catalog entry.
This covers a synchronous application operation, not concurrent external
writers to the package store or filesystem.

The September 8 amendment also requires serialized output to fit the reader's
1 MiB document limit before the destination is opened. Oversized output leaves
the existing destination untouched.

### Transient Window Reads

`WindowDecoder` gains a separate transient read operation using the same typed
DSL, original mapping, and decoder, but with a temporary tree, fresh memoization
state, and independent per-request limits and cancellation. Its result owns
that bounded tree; releasing the result releases its nodes.

The MP4 extractor consumes temporary values for run/sample/chunk/sync tables.
It does not retain their trees or spend the container analyzer's budget.
Existing run-row and descriptor-page limits still apply. Interactive
`decodeWindow` retains its memoized, cumulative presentation behavior. Neither
path may reserve memory proportional to an untrusted page size before checking
its node bound.

### Configuration and Sample Transactions

Bindings record all declared configuration payload nodes. Sample execution is
isolated by exact rule identity, track, and sample-description index.

Configuration definitions have explicit baseline visibility, retaining real
physical spans even when configuration follows media data. A source-ordered
in-band definition visible at the consumer position overrides the baseline of
the same key. Exact dependency-generation validation remains in force. Physical
offsets must never be rewritten to fake visibility.

A sample transaction covers context definitions, payloads, source/tree binding,
and execution counters in addition to its presentation tree. Terminal source,
cancellation, and resource failures publish no state from earlier units of the
failed sample. Per-unit syntax diagnostics remain inspectable under the existing
sample-runner contract.

Repeated navigation reuses results only while their context dependencies remain
valid. Retained sample presentation/context has a session-wide node bound
separate from each navigation's working budget. Capacity exhaustion returns
`ResourceLimit` atomically, without silently dropping context. Disk-backed
context eviction is not introduced in this remediation.

The retained sample-node ceiling is 100,000, including private tree roots and
configuration nodes across all track/description sessions. Cached results are
invalidated when their context directory gains a generation. Replaying an
already published definition is idempotent only if key, source spans, baseline
visibility, exact dependencies, typed structure and exported values all agree;
the original definition/node identity is retained. Sample context is ephemeral
and does not change the session or disk-cache schemas.

### Rebinding Stale Definitions

An out-of-order visit can publish a PPS against baseline SPS, then discover an
in-band SPS before that PPS. On revisiting the same physical PPS, identical
republication is insufficient: the exact dependency generation has changed.

The directory reports the overlapping active definition on registration
conflicts and permits explicit replacement only for the same key, exact source
spans and baseline visibility, with changed, currently valid dependencies. The
old definition must itself be stale at its source end. Replacement allocates a
new monotonic ID and retires only the old lookup candidate, retaining the old
definition snapshot. Invalid replacement attempts mutate nothing.

The rule session invokes this operation only for the same typed structure at
the same source spans after successful re-execution. It does not silently bind
old exported values to new dependencies. Replacement is staged in the existing
compound and sample transactions; a later terminal sample failure rolls it back.
The new generation invalidates cached samples through the existing generation
check. General overlapping definitions remain rejected.

## Verification

Public-interface regressions cover saving aliases, install conflict/restart,
independent page budgets, disjoint mappings, read failure/retry, genuine slice
headers with configuration-only parameter sets before/after media, same-codec
track/description isolation, repeat navigation, and whole-sample rollback.
Stale-definition replacement is also covered when a later unit fails through
source error, cancellation, or resource exhaustion; retry must succeed without
retaining the failed replacement. These combined cases are coverage additions,
not a claim of a new pre-fix RED result.

On September 8, three scoped read-only reviews found no confirmed blocking
defect in the changed context transactions, sample owner, storage, and transient
paging paths. They were static reviews, not cross-platform or release audits.
Multi-owner retention-cap stress, very large window requests, and interleaved
interactive/transient page cancellation remain follow-up coverage gaps.
Actual results belong in the implementation plan after execution. This ADR
claims no new Hosted CI result or release readiness.
