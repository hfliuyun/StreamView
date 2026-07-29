# Own Analysis Cache On A Bounded Background Queue

Status: Accepted
Date: 2026-07-29

## Context

ADR-0029 requires each `PagedCache` instance and its Qt SQL connection to be
created, used, and destroyed on one thread. ADR-0032 and ADR-0034 now provide a
durable analysis namespace plus exact-key progressive-index and
materialized-result payloads, but callers still have to perform synchronous
storage work on their own thread.

The desktop analysis loop currently publishes bounded batches on the GUI
thread. Opening SQLite, encoding a large stable result, waiting for a write,
or destroying a cache connection there would make cache acceleration part of
the interactive latency path. An unbounded asynchronous queue would only move
that problem into retained memory and shutdown behavior.

The existing version 1 materialized-result body also has no total-page count or
final-page marker, and `PagedCache` deliberately has no page-enumeration API.
Reading pages until one is missing cannot prove that the preceding pages form a
complete snapshot. The background owner must not turn that ambiguity into a
claim of persistent analysis recovery.

## Decision

Rules exposes one `AnalysisCacheOwner` for one database path and one validated
`AnalysisCacheNamespace`. The caller supplies the database path; application
cache-location policy remains outside the rules and core modules.

Starting the owner creates a dedicated worker thread, calls `PagedCache::open`
on that thread, and waits only for the open result. The worker exclusively owns
the returned `PagedCache`. Every exact-key read, atomic batch commit, and final
destruction of the cache occurs on that same worker thread. A failed open joins
the worker and exposes no owner.

The public API accepts typed progressive-index or materialized-result pages.
Before a write request enters the queue, the submitting thread:

1. encodes and semantically validates every owner body;
2. wraps each body in the namespace-bound envelope;
3. rejects duplicate keys and batches outside the existing 1 through 256 page
   limit; and
4. retains only the bounded encoded page bytes for the worker commit.

This preflight preserves the required body/envelope/full-key stack while
preventing malformed object graphs or oversized strings from occupying the
background queue. The worker calls only the opaque atomic `commitBatch` with
the prevalidated keys and complete envelopes.

Reads enter the queue as exact full page keys. The worker reads that key,
validates the namespace and page-kind envelope, then decodes the body against
the same full key before returning a typed page. Outcomes distinguish found,
normal missing data, invalid requests, corrupt or incompatible cached bytes,
and storage failure. No partially decoded page is returned.

The queue is bounded by both outstanding request count and retained encoded
write bytes, including the request currently executing. Defaults are 64
requests and 16 MiB. A submission that would exceed either bound returns
`QueueFull` without blocking or retaining the request. Once shutdown starts,
new submissions return `ShuttingDown`.

Each accepted operation has a future for its terminal result. `flush` waits for
all requests accepted before the call and does not admit new work itself.
Explicit shutdown is idempotent: it stops admission, drains every accepted
request, destroys `PagedCache` on the worker, joins the thread, and then
returns. The owner destructor performs the same draining shutdown. Cache
storage or decode failures complete their individual request and do not kill
the worker or discard later accepted work.

Cache data remains optional acceleration. A caller may report a cache failure,
disable further caching for that analysis, and rebuild from the immutable
source plus exact rule identity. A cache miss, corrupt page, full queue, or
storage failure must not invalidate an otherwise valid analysis session or
replace its live tree.

The owner exposes exact-page operations only. Version 1 progressive pages may
be stored and inspected as stable scanner output, but they do not restore the
scanner's pending state or a live analyzer. Version 1 materialized pages may be
stored atomically when their complete key set is already known, but this ADR
does not define cross-process discovery or complete-snapshot reconstruction.
Those require a separately versioned manifest or final-page contract before a
presentation snapshot can be published from cache.

## Consequences

- Qt SQL ownership and `PagedCache` destruction remain on one dedicated
  thread, while callers submit bounded value requests from any thread.
- Preflight makes every accepted write finite and keeps invalid payloads out of
  the queue; SQLite still supplies all-or-nothing batch publication.
- Queue pressure and shutdown are explicit, testable outcomes rather than
  hidden blocking or detached work.
- Exact-key reads perform the complete envelope and body validation stack and
  reject pages copied under another stream or page coordinate.
- The application can connect cache writes without making cache health part of
  analysis correctness.
- Persistent live analyzer recovery remains unavailable, and materialized
  snapshot reads remain unavailable until their completeness is representable.

## Considered Options

- Move an already opened `PagedCache` to another thread: its owner is captured
  during open and wrong-thread destruction is fatal.
- Use a synchronous facade on the caller thread: preserves correctness but
  keeps SQLite and shutdown latency on the GUI path.
- Use an unbounded task queue: makes retained payload memory and shutdown time
  proportional to producer speed and source size.
- Encode arbitrary typed objects only after enqueue: lets rejected strings,
  diagnostics, and node vectors consume memory outside the page-byte bounds.
- Treat the first missing materialized page as end of snapshot: cannot
  distinguish a complete result from an interrupted, evicted, or unknown page
  sequence in version 1.
- Restore a live analyzer from progressive pages: the payload deliberately
  omits pending scanner, mapper, queue, cancellation, context, and allocator
  state.
