# Cache Stable Analysis Results, Not Live State

Status: Accepted
Date: 2026-07-29

## Context

ADR-0029 provides bounded SQLite pages, ADR-0032 binds durable identity and an
integrity envelope, and ADR-0033 keeps cache pages outside `.svsession`. The two
owner payload versions bound into `AnalysisCacheNamespace` still need concrete
body representations before a cache owner can store useful analysis results.

Serializing a live H.264 analyzer at this point would couple persistence to
scanner look-behind, pending start-code state, source buffers, mapper progress,
deferred queues, cancellation objects, context payloads, and identifier
allocation. Those states have different invariants and are not all stable at a
page commit boundary. A partial checkpoint that omits one of them could replay
or skip records and nodes while appearing valid.

## Decision

Define independent version 1 big-endian bodies for:

- H.264 progressive-index pages containing stable start-code records plus the
  first global record index, indexed-through offset, and end-of-source flag;
- materialized-result pages containing complete stable node records, parent
  identity, closed scalar values, locations, metadata, specifications, and
  diagnostics.

The normative layouts and bounds are maintained in
[Analysis Cache Owner Payloads](../analysis-cache-payloads.md).

Both bodies repeat `streamId` and `pageIndex`. Owner decoding requires the full
expected page key and rejects a mismatch after envelope validation. All
arithmetic, framing, strings, flags, enumerations, record relationships,
locations, and node topology are validated before a page is returned. Known
transient materialization states (`Indexing` and `WaitingDependency`) cannot be
stored. Supported `QVariant` values are closed to absent, Boolean, unsigned
64-bit, signed 64-bit, and `QString`.

These are result representations, not analyzer checkpoints. The progressive
body deliberately excludes pending scanner state. The materialized body
deliberately excludes mutable tree links and allocator state. Neither body
contains mapper, queue, cancellation, context-directory payload, or thread
ownership. No decoder constructs or resumes a live analyzer.

`FieldLocation` gains a validated standalone construction path so decoded
locations can prove their own range/span invariant without inventing a complete
`SourceMapping`.

## Consequences

- Stable index and result pages now have deterministic, cross-platform bytes
  and can be protected by the existing namespace and envelope.
- Copying a valid body under another stream/page key is detected even though
  the envelope binds only namespace and page kind.
- Unsupported values and incompatible versions fail explicitly. Malformed or
  semantically invalid pages are discarded in full and can be rebuilt.
- A future background owner can persist completed pages without serializing
  transient analyzer internals.
- Persistent analyzer recovery remains unavailable. It requires an explicit
  owner lifecycle and, if true execution resume is desired, separate contracts
  for every omitted live state component.

## Considered Options

- Serialize the complete analyzer object graph: no stable commit boundary or
  contract yet exists for its transient state.
- Save only scanner cursor and tree nodes: cursor alone cannot represent a
  pending prefix or queued work and could silently duplicate or skip output.
- Put page key only in SQLite: a copied body of the same kind could pass
  envelope validation under the wrong key.
- Use `QDataStream` or native struct bytes: implicit Qt/platform versions,
  padding, and endianness would make the format unstable.
- Accept arbitrary `QVariant`: metatype registration and conversion behavior
  are not a closed durable data contract.
