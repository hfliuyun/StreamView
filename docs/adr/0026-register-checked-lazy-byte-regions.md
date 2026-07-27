# Register Checked Lazy Byte Regions

Status: Accepted
Date: 2026-07-28

## Context

ADR-0013 requires rules to declare safe lazy boundaries, and the analysis model
already distinguishes a `Lazy` node whose bounds are known from unsupported,
invalid, cancelled, and materialized nodes. The executable DSL still has no
declaration that creates such a node. Rules must either consume every declared
field immediately or leave payload bytes outside the rule entirely.

The current VM is deliberately linear. It can evaluate bounded scalar
expressions and preserve source mappings, but it does not yet support nested
structure calls, a runtime call stack, lazy decode recipes, or progressive
index persistence. The first lazy slice must establish a useful, checked
boundary without importing those later features or reading a large payload
merely to skip it.

## Decision

The minimum DSL adds a dedicated lazy-byte-region item:

```cpp
struct Packet {
    bits<16> payload_size;

    @lazy(payload_size)
    bytes payload @description("Deferred packet payload");
}
```

`@lazy(byte_count_expression)` must immediately precede `bytes name`.
`@description` and `@spec` may follow the name before the semicolon. `bytes`
without `@lazy`, generic annotations before `@lazy`, array suffixes,
`@equals`, and `@enum` are not accepted. The byte count describes logical
bytes in the current execution view, not absolute source bytes.

In leading struct-item position, the token sequence `@lazy(` is a reserved
introducer and is recognized before the generic annotation list. It is not a
generic annotation carrying annotation-value arguments; this distinction lets
its argument use the complete bounded expression grammar.

The byte-count expression uses the accepted bounded expression grammar and
must have type `u64`. It may reference only earlier scalar unsigned syntax or
computed fields that are guaranteed on every path reaching the declaration.
Pure calls are expanded at compile time under the existing expression depth,
node, and work limits. A lazy region is not a scalar value and cannot be used
as a later expression dependency or controller. Its name shares the
structure-wide field namespace.

Lazy regions may appear in conditional, switch, and bounded-repeat bodies.
They inherit the enclosing presence guards and repeat indexes. Every projected
region counts toward the existing 99,999-item structure limit. A selected
region consumes one VM instruction and one analysis-node slot; a region whose
guards are false still consumes its instruction but does not evaluate its byte
count, consume input, or create a node.

The compiler accepts a lazy region only when its structure-relative start is
statically known to be byte-aligned. Registering a runtime-sized region makes
the following exact static offset unknown. Existing conservative alignment
rules therefore continue to reject a later little-endian field or lazy byte
region unless a future analysis can prove its alignment. This slice does not
weaken branch or repeat alignment checks.

The typed IR represents a lazy region as a declaration-order field of kind
`LazyBytes`. Its required typed `u64` expression and presence guards are
validated before execution. It emits one `register-lazy-bytes` instruction;
there is no read, call, jump, or separate lazy-expression opcode.

When the instruction is selected, the VM performs these steps in order:

1. Evaluate the byte-count expression with checked `u64` arithmetic.
2. Reject multiplication by eight if it would overflow the bit-coordinate
   domain.
3. Verify that the absolute logical start is byte-aligned and that the complete
   bit range fits the reader's remaining enclosing range.
4. Resolve that logical range through the execution `SourceMapping`, retaining
   every forwarded source span and excluding every source-coordinate gap.
5. Check the node budget and append a `Region` named by the declaration.
6. Seek the reader to the checked exclusive end without reading payload data.

A positive-length region is created in `MaterializationState::Lazy`. A
zero-length region has no deferred contents and is created directly as
`Materialized`, with a valid empty logical range and no source spans. The
containing structure may become materialized while a positive child remains
lazy; the complete analysis tree is not fully materialized until that child is
resolved or terminalized.

Checked-expression failure or byte-to-bit overflow is `invalid-syntax` at the
lazy field path. A declared range larger than the remaining enclosing reader is
`truncated-source` and carries the available mapped prefix when non-empty. A
misaligned execution start, missing or non-`u64` typed expression, invalid
field reference, opcode/type mismatch, or failed mapping resolution is an
invalid typed definition. A node-limit breach is `resource-limit`.
Cancellation is observed at the instruction boundary. Every failure occurs
before the lazy node is appended or the reader is advanced, and retains all
earlier completed fields.

`register-lazy-bytes` performs no source read. This is observable even when the
registered range crosses several mapping spans or the source would fail if the
payload were read. The resulting node location remains the coordinate authority
for tree-to-raw selection and later work. Existing raw-bit reverse lookup
continues to ignore non-materialized nodes, so it does not select a positive
lazy region until that region is materialized.

This slice registers the safe boundary and durable lazy node only. Typed
nested-structure materialization, decode recipes, user-triggered expansion,
incremental insertion of children into an already published GUI subtree, and
recoverable progressive indexes remain separate decisions. They will consume
the checked node location rather than infer the boundary again.

## Consequences

Rules can make a large payload visible in the tree and selectable from the tree
without reading or allocating it. The operation remains deterministic,
source-mapped, bounded by the enclosing reader, charged to the existing sandbox
budgets, and compatible with conditional and repeat projection.

The initial syntax intentionally represents only uninterpreted logical bytes.
It does not create a general eager byte-array value, accept arbitrary type
annotations, or promise a nested parse target. Later on-demand parsing can add
a typed target while preserving this boundary and its source coordinates.

Conservative static alignment may reject a declaration that a more powerful
solver could prove safe. That limitation is explicit and preferable to
guessing an alignment after a runtime-sized region.

## Considered Options

- Treat `@lazy` as a generic field annotation: the current annotation grammar
  cannot carry a full expression, and unknown annotations are otherwise
  discarded by typed lowering.
- Add lazy nested-structure calls immediately: useful eventually, but it would
  combine structure composition, call-depth semantics, decode recipes, tree
  insertion, and cancellation/resume policy in one slice.
- Copy or probe the payload while registering it: this defeats lazy work and
  can make initial cost proportional to source size.
- Store only an absolute source span: this loses logical coordinates and is
  incorrect for a region crossing excluded bytes in a mapped view.
- Reject empty regions: zero is a valid checked boundary; materializing it
  immediately avoids a permanently pending node with no contents.
