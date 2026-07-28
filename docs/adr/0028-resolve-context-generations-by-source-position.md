# Resolve Context Generations By Source Position

Status: Accepted
Date: 2026-07-28

## Context

H.264 sequence and picture parameter sets, AAC AudioSpecificConfig values, and
ISO BMFF sample descriptions establish context used by syntax that appears at a
different source position. An identifier is not enough to select that context.
The same identifier may be redefined later, sample-description indexes repeat
between tracks, and lazy or known-offset analysis may discover definitions out
of source order.

A consumer must not bind to a future or partially parsed definition. It also
must not silently continue using a dependent context after the exact definition
on which it was validated has been superseded. For example, a PPS validated
against one SPS generation cannot be assumed valid after a later SPS with the
same ID becomes current.

The analysis tree already provides append-only node identities and source
locations, but it does not answer which definition is current for a format key
at an arbitrary source position. Putting that policy separately in each H.264,
AAC, and ISO BMFF rule would duplicate boundary and invalidation behavior.

## Decision

Core exposes an in-memory `ContextDirectory` type intended to be owned once by
each analysis-session source when format context is present. The type stores
only format-neutral identity and position metadata. Rule owners retain their
typed SPS, PPS, ASC, or sample-description payload separately and associate it
with the stable `ContextDefinitionId` returned by the directory. Session
ownership and rule-runner plumbing arrive with the first consuming format rule.

A context key contains:

- a closed definition kind: H.264 SPS, H.264 PPS, AAC AudioSpecificConfig, or
  ISO BMFF sample description;
- a host-assigned numeric scope; and
- a kind-specific unsigned value.

Scope zero represents the natural global scope of a standalone elementary
stream. A container host assigns a stable nonzero scope per track or equivalent
codec context. H.264 keys use the parameter-set ID, sample descriptions use
their description index, and a singular ASC may use value zero. The directory
does not interpret those values.

A registered definition contains a non-empty absolute source span, the stable
analysis node that presents it, and zero or more exact definition-generation
dependencies. The complete span is the availability boundary. A lookup at
position `P` selects, for that key, the definition with the greatest
`sourceSpan.endExclusive()` not greater than `P`. Therefore:

- a definition is unavailable while `P` is inside its own span;
- it becomes selectable exactly at its exclusive end;
- a later definition does not affect queries before that later span ends; and
- no query can bind to a future definition.

Definitions for one key must have non-overlapping spans. Definitions for
different keys or scopes may overlap. Registration order is independent of
source order so a lazy or known-offset worker can publish an earlier definition
after a later one was discovered. Stable definition IDs follow append order,
are never reused, and snapshots are returned by value.

Only completed, valid definitions are registered. A malformed definition is
reported by its parser and does not create a directory tombstone; the preceding
valid generation remains the latest selectable definition until another valid
generation is registered. Invalid metadata, an overlapping same-key span, or a
failed dependency registration changes no visible directory state.

Dependencies bind exact generations. At registration, every dependency must be
unique, must use another key, and must be the generation selected immediately
before the new definition's source-span start. Missing, future, stale, duplicate,
and same-key dependencies are rejected.

Lookup revalidates every bound dependency at the consumer position, with a
maximum resolution depth of 64 definitions. Each
dependency must still resolve to the same definition ID. A later redefinition
therefore makes the dependent lookup `DependencyUnavailable`; lookup does not
fall back to an older generation of the requested key. A later set of
cross-redefinitions can form a dependency cycle even though every individual
registration was valid at its source position, so lookup detects cycles and
reports them as unavailable rather than recursing without a bound.

The directory has explicit registered, invalid-definition,
duplicate-definition, dependency-unavailable, found, and not-found outcomes.
It does not mutate the analysis tree. The analysis worker translates an
unavailable lookup into `waiting-dependency` or a `DependencyUnavailable`
diagnostic according to the active parser's recovery contract.

The directory follows the existing single-writer analysis-worker model. It has
no internal locking and contains no cancellation loop because one registration
or position lookup has bounded local work; key selection is logarithmic and
dependency traversal is depth-bounded. Durable serialization,
source-fingerprint validation, session ownership, rule-runner integration, and
SQLite paging remain later M4/M5 or consuming-format work.

## Consequences

H.264, AAC, and ISO BMFF analyzers share one precise source-position rule. Track
scope prevents equal sample-description indexes from colliding, and source
order remains correct when definitions are discovered lazily or progressively.

Generation-sensitive dependencies expose stale context instead of silently
decoding with a mismatched SPS, PPS, ASC, or sample description. Consumers can
retain partial results and present the existing dependency-unavailable state.

The core directory intentionally does not own typed format payload. A rule
owner must maintain the payload association for returned definition IDs, and a
future persistent cache must serialize both layers under the same source and
rule identity.

## Considered Options

- Keep only the latest definition per numeric ID: cannot answer historical or
  known-offset queries and collides across tracks and definition kinds.
- Store definitions in the analysis tree and search tree nodes for every
  lookup: mixes presentation hierarchy with context-selection policy and gives
  no exact dependency-generation contract.
- Let a dependent context continue using the generation captured at creation:
  hides later dependency replacement and can decode syntax under stale rules.
- Fall back to an older requested-key generation after dependency failure:
  silently guesses context instead of reporting that the latest definition is
  unusable.
- Require definitions to be registered in source order: conflicts with lazy
  materialization and direct analysis at a known offset.
- Put typed SPS/PPS/ASC/sample-description variants in core: couples the core
  module to format schemas that belong to rules and will evolve independently.
