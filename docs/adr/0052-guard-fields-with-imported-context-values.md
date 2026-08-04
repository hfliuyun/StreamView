# Guard Fields With Imported Context Values

Status: Accepted
Date: 2026-08-04

## Context

ADR-0046 lets a dynamic bit width read one scalar from the exact imported
context closure. The remaining bounded H.264 IDR slice-header syntax also
depends on scalar flags exported by the selected PPS: bottom-field picture-order
count is present only when the PPS enables it, and redundant-picture syntax is
present only when the PPS declares that capability. Keeping those decisions in
the rule requires an imported value before the affected fields are read.

The existing conditional language projects both branches into a guarded linear
field stream. Extending that model with one statically described imported scalar
does not require general imported expressions, an object model, or control-flow
jumps.

## Decision

Accept one additional conditional form:

```cpp
if (context_value(pic_parameter_set_id,
                  h264_pps,
                  bottom_field_pic_order_in_frame_present_flag) == 1) {
    se delta_pic_order_cnt_bottom;
}
```

The left side must be exactly
`context_value(import_key, context_kind, exported_field)`, the operator must be
`==`, and the right side must be one unsigned integer literal. The imported
value has type `u64`, so the complete `u64` literal range is accepted. Arithmetic,
Boolean combinations, negation, `!=`, ordering, calls around the imported value,
and imported Boolean shorthand are rejected. Existing field equality and
`computed<bool>` shorthand conditions retain their behavior.

The import key, reachable target kind, unique publishing structure, and ordered
export are resolved with the same static contract as ADR-0046. The compiler
lowers the left side to a typed imported-context descriptor and attaches it, the
expected literal, and a positive or negated sense to every projected branch
field. Descriptor identity includes the import ordinal, target kind, publishing
structure, and export ordinal, so nested guards over different exports remain
distinct. No conditional opcode, jump, or runtime field-name lookup is added.

Before reading source, the VM validates every imported guard as a canonical
leaf of type `u64` with no operands, equality operator, and a complete exact
import/publisher/export descriptor. A malformed descriptor is an invalid typed
definition even when its branch would not be selected.

When a field's guard is reached, the VM asks the existing execution-context
resolver for the scalar. `RuleExecutionSession` resolves and caches the same
exact generation closure defined by ADR-0045 and ADR-0046. A matching guard
selects the field; a nonmatching guard skips it without reading source, creating
an analysis node, or applying field constraints. Missing, future, or stale
generations remain `dependency-unavailable` with the diagnostic anchored to the
materialized import-key field. Schema or payload mismatches remain invalid
runtime definitions. A successful execution returns the same cached imported
closure, and imported values create no presentation nodes.

## Consequences

Rules can express imported layout-presence flags while the analyzer remains
format-neutral. Imported guards compose with existing field, switch, and repeat
guards because all of them are lowered into the same ordered projection model.
Only selected fields consume bits, so following syntax begins at the exact end
of the preceding selected field.

## Non-goals

This decision does not permit imported values in computed fields, lazy byte
counts, array lengths, enum or annotation arguments, payload dispatch, switch
controllers, repeat controllers or bounds, sentinel termination, or general
expressions. It does not add `!=`, ordering, Boolean combinations, imported
Boolean shorthand, arbitrary member access, or fallback context-generation
selection.
