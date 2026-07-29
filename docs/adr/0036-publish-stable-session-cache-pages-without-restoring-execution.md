# Publish Stable Session Cache Pages Without Restoring Execution

Status: Accepted
Date: 2026-07-29

## Context

ADR-0035 provides a bounded typed background owner, but no production session
submits pages to it. The H.264 analyzer currently returns published node IDs
only; it does not expose the stable scanner records produced at a batch
boundary. `AnalysisTree` also has no cache-page export operation.

Cache reuse must remain subordinate to analysis correctness. Existing tests
construct virtual sources and temporary file sessions directly, so silently
enabling the user's persistent cache in every `AnalysisSession` constructor
would create filesystem side effects and would tempt path-like virtual source
identities into durable namespaces.

Version 1 materialized-result pages still have no manifest or final-page marker.
This change can publish a known complete atomic batch, but cannot later discover
or prove that batch complete using the current read API.

## Decision

Application session APIs keep caching disabled unless the caller supplies an
`AnalysisSessionCacheOptions` value with a database path. The production
executable obtains `QStandardPaths::CacheLocation` after setting the StreamView
application and organization identity, then uses
`analysis-cache.sqlite` below that location. Tests and embedders opt in with an
explicit path. Arbitrary `RandomAccessSource` sessions remain cache-disabled.
Cache activation must finish before the first analyzer batch. A later activation
would omit already published progressive records and is rejected rather than
presented as an active complete write path.

A cache-enabled local-file session computes a version 1 `SourceFingerprint`
from the same already-open `FileSource` handle used by analysis. Session restore
reuses the fingerprint already computed and compared with the saved document.
The session combines it with the analyzer's exact `RuleEntryPointIdentity` to
create `AnalysisCacheNamespace`, then starts one `AnalysisCacheOwner`. Any
fingerprint, namespace, path, driver, lock, or cache-open failure disables cache
acceleration but does not fail source opening, rule resolution, analyzer
creation, or atomic session replacement.

`H264AnnexBAnalysisBatch` gains an optional stable progressive-index update.
The analyzer creates at most one update for each actual scanner batch. It
contains:

- the zero-based global index of the first returned stable record;
- the stable records returned by that scanner batch;
- the exclusive byte frontier covered by complete returned records; and
- an end-of-source flag only for scanner `Complete`.

The stable frontier does not advance merely because the scanner cursor inspected
bytes after an unresolved pending start code. It advances to the greatest end
of a returned record, and advances to source size only on scanner `Complete`.
An empty update is emitted only when `Complete` must publish the terminal
frontier. Deferred mapper or analyzer work does not duplicate the update.

The session writes these updates under progressive-index stream ID 0 and
monotonically increasing page indexes starting at 0. The analyzer's default
256-record batch fits the version 1 page bound. A write is submitted only for a
new update; the page index advances only after queue acceptance.

When the analyzer becomes terminal, the session exports the complete stable
tree under materialized-result stream ID 0. Nodes are read in stable ID order,
converted without changing their parent IDs, values, locations, metadata, or
diagnostics, and partitioned into the largest deterministic consecutive
prefixes accepted by the version 1 body codec. The export rejects a transient
node, an individually oversized node, an invalid topology/value, a page-index
overflow, or more than 256 pages. All resulting pages are submitted in one
owner request and therefore one SQLite transaction. No empty page is written.

The session never waits for a cache write in `analyzeBatch`. It polls previously
accepted futures without blocking and exposes the same nonblocking poll for the
application event loop after the terminal batch. A preflight failure,
`QueueFull`, shutdown, or completed storage error disables additional writes for
that session and records a cache error, while returning the original analyzer
batch unchanged. Already accepted work is drained by the owner's shutdown when
the session is destroyed.

This change is write-only. It does not enumerate pages, treat a missing page as
an end marker, rebuild an `AnalysisTree`, publish a cached presentation
snapshot, seed scanner queues, or resume a live analyzer. Cache pages remain
outside `.svsession`.

## Consequences

- Production local-file analysis now publishes durable pages under the complete
  source/rule/version namespace without introducing path-only identity.
- Direct local-file tests and embedders have no cache side effects unless they
  explicitly opt in; arbitrary virtual-source sessions remain cache-disabled.
- Progressive records are exposed as stable batch output without exposing
  scanner pending state.
- A terminal materialized-result export is all-or-nothing at the SQLite batch
  level; trees that cannot fit the bounded version 1 representation simply run
  without that cache acceleration.
- Cache availability and health never decide whether an analysis session is
  valid.
- Persistent cached presentation and live execution recovery remain future,
  separately versioned work.

## Considered Options

- Enable a default cache inside every session constructor: writes user state
  during tests and gives virtual sources no valid fingerprint contract.
- Derive a namespace from source path or `identity()`: stale and caller-defined
  identities can silently bind unrelated bytes.
- Use the scanner cursor as the stable frontier: it may lie beyond a pending
  start code whose record has not been emitted.
- Write each materialized page as an independent request: a partial result can
  become visible when a later page fails.
- Block `analyzeBatch` on each future: makes optional SQLite latency part of the
  analysis publication path.
- Read pages until missing and attach them to the UI: version 1 has no complete
  set discovery or terminal-page proof.
