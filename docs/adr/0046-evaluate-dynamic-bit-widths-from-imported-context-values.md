# Evaluate Dynamic Bit Widths From Imported Context Values

Status: Accepted
Date: 2026-08-04

## Context

ADR-0045 returns exact rules-owned context payloads after a consumer structure
materializes. A bounded H.264 slice header must use the selected SPS generation's
`log2_max_frame_num_minus4` export to read `frame_num`, whose width is that value
plus four. The VM therefore needs one imported scalar before it can finish the
consumer, without moving directory policy or H.264 field knowledge into the
Annex B analyzer.

The first dynamic-width slice should reuse the existing checked expression
language and keep the context interface narrow. It does not need general member
access, an imported object model, or arbitrary calls from rules into the engine.

## Decision

Allow a non-literal unsigned arithmetic expression as the width of a big-endian
`bits` field. A context export is referenced only through the reserved expression
form `context_value(import_key, context_kind, exported_field)`:

```cpp
@context_import("h264-pps", pic_parameter_set_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue slice_type;
    ue pic_parameter_set_id;
    bits<context_value(pic_parameter_set_id,
                       h264_sps,
                       log2_max_frame_num_minus4) + 4> frame_num;
}
```

The kind identifier is one of `h264_sps`, `h264_pps`, `aac_asc`, or
`iso_bmff_sample_description`. The import key must be an earlier field and must
identify exactly one `@context_import` on the structure. The target kind may
name the imported root or one definition in its exact dependency closure. The
compiler requires exactly one publishing structure for that target kind and an
`@context_export` field with the requested name. It lowers the reference to the
root import index, target kind, publishing structure index, and ordered export
index; runtime code does not compare field names.

This first slice permits `context_value` only inside a dynamic `bits` width.
Dynamic widths have type `u64`, use the existing checked arithmetic and pure-call
inlining limits, and are big-endian only. They cannot be fixed arrays, enum
fields, context keys, dependencies, imports, or exports. A dynamic field makes
the exact static offset of following fields unknown. Literal `bits<N>` fields
retain their existing typed IR and behavior.

The VM validates every dynamic-width expression and imported-reference descriptor
before reading source. It keeps the existing `read-unsigned-bits` opcode; when
the field is selected, it asks the execution context for the referenced scalar,
evaluates the expression, and then reads exactly that many bits. Widths outside
`1..64` and checked-arithmetic failures are `invalid-syntax` and consume no bits
for that field. Truncation, mapping, cancellation, instruction, node, and partial
result behavior remain the same as for literal-width fields.

`RuleExecutionSession` is the only production resolver. At the first requested
value it resolves the root import at the consumer enclosing span start using the
ADR-0045 algorithm, materializes and caches that exact closure for the run, then
selects the statically lowered target structure and export ordinal. Missing,
future, or stale generations remain `dependency-unavailable` with no fallback.
A missing or ambiguous target definition, schema mismatch, or missing payload is
invalid runtime definition. A successful run returns the same exact imported
closure as ADR-0045; imported values create no presentation nodes.

## Consequences

Rules can express the first layout-critical H.264 slice-header width while the
analyzer remains format-neutral. The compiler owns schema names, the session owns
generation selection and payload identity, and the VM sees only a bounded scalar
resolver plus stable typed descriptors.

## Non-goals

This decision does not add imported values to computed fields, conditions, lazy
sizes, or repeat bounds; dynamic little-endian fields or arrays; sentinel loops;
compressed remaining-bit payloads; or H.264 slice dispatch.
