# Register A Compressed Remaining-Bit Payload Terminal

Status: Accepted
Date: 2026-08-04

## Context

The H.264 slice-header slice can describe the bounded header syntax once it
imports the active PPS and SPS values. The following `slice_data()` is entropy
coded with CAVLC or CABAC. Decoding it is explicitly outside the v0.1 scope,
but omitting it would leave the dispatched RBSP view partially consumed and
would violate ADR-0037's exact-consumption contract. AAC has the same boundary
at a future `raw_data_block()`: the rule must identify the compressed range
without pretending to decode its Huffman payload.

The existing lazy byte region cannot state this boundary. It requires a byte
count and byte-aligned start, while H.264 slice data begins at the current bit
position and extends to the end of the bounded logical view. The analyzer must
not infer that range from field names or format knowledge. The declaration,
node kind, and terminal consumption therefore belong to the DSL.

## Decision

The DSL adds a named terminal structure item:

```cpp
compressed_payload slice_data
    @description("Entropy-coded slice data.")
    @spec("ITU-T H.264", "7.3.2.10");
```

`compressed_payload` is a contextual identifier. The item may have
`@description` and `@spec` after its name and no other annotations. It cannot
have an array suffix or leading annotations. Its name participates in the
structure-wide field namespace.

The item occurs at most once, unconditionally at a structure's top level, and
must be the final item. It cannot appear inside a conditional, switch, or
repeat body. It is mutually exclusive with `rbsp_trailing_bits`; both terminals
claim the remainder of their current view and cannot be composed. A structure
containing only a compressed payload is non-empty.

The typed IR represents the item as one declaration-order field of kind
`CompressedPayload`. It has no scalar value, expression, condition, array,
constraint, enum, or context eligibility. It therefore cannot be a controller,
expression dependency, context key, import key, or exported value. It emits one
`register-compressed-payload` instruction, consumes one instruction and one
analysis-node slot, and is one cancellation point.

Before execution reads the source, the VM validates the field kind, metadata,
terminal position, opcode, and lack of scalar-only properties. Malformed typed
IR is an invalid definition and performs no source read.

When selected, the instruction takes every bit remaining in the current bounded
reader. It resolves that logical range through the execution `SourceMapping`,
preserving all mapped source spans, appends a `CompressedPayload` analysis node
in `Materialized` state, and seeks the reader to its end without reading or
copying payload data. The range may be non-byte-aligned, may cross excluded
source spans, and may be empty. An empty declaration still publishes a node
with its valid empty logical range so the rule's explicit terminal remains
visible. Mapping failure is an invalid definition; node-budget exhaustion is a
resource limit. Both occur before the node is appended or the reader advances.

Materialized means that the promised opaque representation is complete. It
does not mean the compressed syntax was decoded, and the node is neither lazy
nor unsupported. Exact dispatched-view consumption then succeeds naturally
because the instruction advances to the reader's exclusive end.

This slice adds the language primitive only. The bundled H.264 rule adopts it
with the later slice-header structure and dispatch change, so its package
version does not change in this step. The language compatibility string remains
`0.1` because the syntax is an additive bounded v0.1 item.

## Consequences

Rules can explicitly preserve compressed syntax at bit precision without
moving H.264, AAC, CABAC, CAVLC, or Huffman knowledge into the analyzer. The
node retains complete logical and source coordinates and is selectable even
though its bytes were never read.

The terminal deliberately consumes all remaining bits. A format whose
compressed range has a declared length or is followed by ordinary syntax needs
a different bounded declaration; this item cannot be used as an unchecked
middle-of-structure skip.

## Considered Options

- Reuse `@lazy(...) bytes`: it requires a known byte count and byte alignment,
  and it incorrectly promises deferred expansion rather than completed opacity.
- Let the analyzer recognize `slice_data` or `raw_data_block`: that would put
  formal-format semantics in C++ and make renaming a rule field change runtime
  behaviour.
- Leave the suffix unconsumed: dispatched structures would fail exact
  consumption, or the runner would have to weaken that conformance guarantee.
- Mark the node `Unsupported`: the rule has represented the requested opaque
  boundary completely; unsupported is reserved for a requested decode that the
  active rule cannot provide.
- Require at least one remaining bit: an explicit empty terminal is deterministic
  and retains the declaration without inventing a special absence rule.
