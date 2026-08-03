# Import Rule-Declared Context Generations

Status: Accepted
Date: 2026-08-04

## Context

ADR-0044 publishes SPS and PPS generations through a rules-owned
`RuleExecutionSession`, but the next H.264 slice-header rule must select those
generations at the consumer position. The selection must not be implemented by
the Annex B analyzer and must not read values back from presentation nodes.

The first import slice only needs a stable selection and payload hand-off.
Dynamic bit widths, expressions over imported values, and slice dispatch remain
later language work.

## Decision

Add a repeatable structure annotation:

```cpp
@context_import("h264-pps", pic_parameter_set_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue pic_parameter_set_id;
}
```

The annotation accepts one recognized context kind and one unconditional,
top-level, non-array unsigned scalar key field in the same structure. A
structure may declare at most 16 imports. The compiler lowers each import to a
stable typed-field key index and preserves declaration order. Duplicate kind/key
pairs are rejected.

The VM validates import metadata before reading source. After successful field
execution it returns only each selected key value, its exact location, and the
selected import descriptor. It does not expose the complete local environment.

`RuleExecutionSession` resolves every import with `ContextDirectory::resolveBefore`
at the start of the consumer's enclosing source span. It requires the
mapped execution spans to lie inside that enclosing span, never falls back after
a missing or stale dependency, and reports `dependency-unavailable` without
publishing a context generation. For a found generation it attaches the
rules-owned exported scalar payload and its exact dependency closure to the
result by `ContextDefinitionId`. The root is first; dependencies follow in
declaration-order depth-first traversal, with each exact definition included
once. One closure contains at most 64 definitions. Each entry retains its kind,
publishing structure index, ordered exported values, and exact dependency IDs.
A missing payload for a directory generation is an invalid runtime definition;
an oversized closure is a resource-limit result.

Imported payloads are result data in this slice; they do not yet enter the
expression namespace, alter source consumption, or create nodes. Later dynamic
`bits<expression>` and computed-field work will consume the same ordered values
behind this session interface.

## Consequences

The rule owns context selection and payload interpretation. The analyzer remains
format-neutral, stale generations remain visible as unavailable, and tests can
exercise exact position and payload identity without constructing a presentation
tree value bus.

## Non-goals

This decision does not add dynamic-width fields, imported identifiers in
expressions, sentinel loops, compressed slice data, or H.264 slice dispatch.
