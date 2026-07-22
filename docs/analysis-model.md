# Analysis Node And Diagnostic Model

Status: Phase 1 implementation baseline. This English document is normative;
the maintained [Chinese companion](zh-CN/analysis-model.md) is provided for
accessibility.

## Analysis Nodes

An analysis tree is append-only with monotonically assigned, one-based node
identifiers. Identifiers are never reused. A node records its kind, name,
materialization state, optional value, optional field location, child IDs, and
attached diagnostics.

Node kinds are root, structure, syntax field, computed field, compressed
payload, and region. A syntax field must have an exact field location. A
computed field must not have a source-backed location. Root nodes cannot be
added as children. Empty names are rejected, and children may be appended only
while their parent is `indexing`.

Tree lookups return node snapshots. A snapshot remains valid after later nodes
are appended; callers do not retain pointers into mutable tree storage. Tree
mutation is owned by one analysis worker. Thread-safe publication and UI model
updates are separate responsibilities.

## Source-Bit Resolution

A source-bit lookup considers only materialized nodes whose field location
contains the requested bit in at least one half-open source span. Disjoint
spans are tested independently; a bit in a gap does not match the node.

The result is deterministic. The deepest matching node wins. At equal depth,
the node with the smaller total source-span coverage wins; if coverage is also
equal, the lower stable node identifier wins. Non-materialized nodes never
shadow a materialized ancestor or sibling. A lookup with no matching node
returns no identifier, allowing the raw bit selection to remain visible while
the analysis-tree selection is cleared.

## Materialization States

The core distinguishes:

- `lazy`: bounds are known but contents have not been requested;
- `indexing`: work is actively materializing or indexing the node;
- `waiting-dependency`: work is blocked on a declared analysis dependency;
- `cancelled`: work stopped on request and may later resume as `indexing`;
- `unsupported`: the syntax is recognized but outside declared support;
- `invalid`: the source is malformed, truncated, unreadable, or exceeds a
  checked resource limit;
- `materialized`: declared contents are complete.

`unsupported`, `invalid`, and `materialized` are terminal. `cancelled` may
transition back to `indexing`. Active states may become terminal, cancelled, or
waiting. A terminal parent accepts no new children.

## Diagnostics And Partial Results

A parse diagnostic has a stable code, severity, message, field path, and
optional exact field location. Initial diagnostic codes cover truncated source,
invalid syntax, unsupported syntax, cancellation, source I/O failure, resource
limits, and unavailable dependencies.

Marking a node cancelled, unsupported, or invalid attaches the diagnostic and
changes only that node's state. Existing nodes, children, values, and source
locations remain available. The tree reports that it contains partial results
while any node is in one of those three states. It is fully materialized only
when every node is materialized.

Invalid examples include a syntax-field node without a location, a computed
field with a source location, adding a root as a child, appending beneath a
parent that is not indexing, and changing an invalid node back to indexing.

## Cancellation

A cancellation source creates copyable cancellation tokens backed by a shared
state whose lifetime extends through every source and token that references it.
Requesting cancellation is thread-safe, non-throwing, and idempotent. Consumers
poll the token at documented cancellation points; the token does not throw,
terminate work, or mutate the analysis tree by itself. The worker that observes
the request publishes its completed work and marks the unresolved node
cancelled with a diagnostic. The storage mechanism is an implementation detail;
see [ADR-0018](adr/0018-portable-cancellation-state.md).
