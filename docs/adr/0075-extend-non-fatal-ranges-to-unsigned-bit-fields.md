# Extend Non-Fatal Ranges To Unsigned Bit Fields

Status: Accepted
Date: 2026-08-11

## Context

ADR-0040 introduced non-fatal `@range(minimum, maximum)` checks for `ue`
fields. The same distinction between a semantic value violation and a layout
failure also applies to fixed-width syntax fields.

H.264 clause 7.4.3 requires `frame_num` to equal zero for an IDR picture. The
bundled rule reads that field with the dynamic width
`log2_max_frame_num_minus4 + 4`, but currently accepts every value representable
by that width. A nonzero value is non-conformant, yet it neither changes the
field width nor moves `field_pic_flag`, `idr_pic_id`, picture-order syntax, IDR
marking, QP, or the opaque slice payload.

The existing source-anchored `assert` statement cannot represent this contract:
its failure is fatal and stops the structure. Adding a one-off warning assertion
would duplicate the stable range diagnostic and bytecode behavior.

## Decision

Extend `@range(minimum, maximum)` from `ue` fields to unsigned fixed-width and
dynamic-width `bits` fields. Enum-backed fixed-width `bits` fields may use the
same annotation; fatal enum membership and `@equals` checks still execute before
the non-fatal range checks.

The parser and compiler retain the existing arity, uniqueness, and ordered-bound
rules. For a fixed-width `bits<N>` field, the maximum must fit in `N` bits. A
dynamic-width field accepts the full unsigned 64-bit annotation domain because
its runtime width is not known statically; the existing runtime width contract
still requires `1..64`, and every decoded value necessarily fits that selected
width. The existing `ue` maximum remains `2^64 - 2`.

No new typed-IR descriptor or opcode is added. Both unsigned bit encodings reuse
`DslTypedUnsignedRange`, `assert-range-minimum`, and `assert-range-maximum`.
The VM validates that each instruction follows the matching materialized field
and bound. A violation attaches a warning-severity, source-located
`invalid-syntax` diagnostic to the field, preserves the complete field span,
keeps the enclosing structure materialized, and continues with later fields.

Apply `@range(0, 0)` to
`IdrSliceLayerWithoutPartitioningRbsp.frame_num`, with an ITU-T H.264 clause
7.3.3 and 7.4.3 reference. Package `0.1.27` retains coverage depth
`picture-order-count-slice-header`.

## Consequences

The bundled profile reports a nonzero IDR frame number without hiding the
remaining header. Regression coverage includes fixed and dynamic bit fields,
malformed typed IR, the legal value zero, the first illegal value one, the
complete dynamic-width warning span, unchanged following field and payload
locations, and continued scanning of the next NAL.

Unsigned fixed-width semantic domains can reuse the same annotation without
inventing format-specific warning statements. The DSL reference must now
describe type-specific static maxima rather than calling `@range` a `ue`-only
annotation.

## Non-goals

This decision does not extend `@range` to `se`, computed, lazy, compressed
payload, or generated trailing-bit fields. It does not add expression-valued or
signed bounds, a general warning assertion, rule-selected severity, or a custom
diagnostic message. It does not validate `first_mb_in_slice`, signed QP or
deblocking domains, derived picture order, decoded-picture-buffer state, MMCO-5,
or output order.
