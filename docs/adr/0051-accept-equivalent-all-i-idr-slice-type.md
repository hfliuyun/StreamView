# Accept The Equivalent All-I IDR Slice Type

Status: Accepted
Date: 2026-08-04

## Context

The first bounded IDR slice-header rule accepts the progressive all-I form
with `slice_type == 2`. H.264 also permits the equivalent all-I value 7. The
header layout is identical for these two values; the distinction is a coded
value alias, not a new reference-list, field-picture, or prediction branch.
The DSL now provides a named enum domain for `ue` fields through ADR-0050.

## Decision

Declare `IdrAllISliceType { i = 2; all_i = 7; }` in the bundled H.264 rule and
annotate `slice_type` with `@enum(IdrAllISliceType)`. NAL unit type 5 keeps the
same `IdrSliceLayerWithoutPartitioningRbsp` structure, dynamic imported widths,
IDR marking flags, `slice_qp_delta`, and opaque remaining-bit payload. Values 2
and 7 are both materialized as unsigned `u64` fields with the enum type name;
all other values are fatal `invalid-syntax` at the complete `slice_type`
Exp-Golomb codeword. The current NAL fails locally and subsequent NAL units
continue scanning.

Package version `0.1.9` advertises the additive rule change and retains the
`idr-slice-header` coverage token. The type-7 regression fixture verifies the
7-bit codeword and the exact remaining 11-bit opaque suffix. An invalid type-3
fixture verifies a diagnostic at absolute bit 33 with a 5-bit source span and a
following AUD that still materializes.

## Consequences

Common progressive IDR all-I slices using either normative code value now share
one source-mapped header projection. The rule remains format-neutral and does
not infer any additional slice syntax from the enum member name.

The bounded Phase 3 slice-header goal remains incomplete: imported-context
conditional branches for bottom-field POC, redundant pictures, and deblocking
controls, followed by non-IDR/P/B reference-list and weighted-prediction
branches, are still separate increments.

## Non-goals

This decision does not support `slice_type` values 0, 1, 3, 4, 5, or 6,
non-IDR NAL units, P/B/SP/SI syntax, field pictures, POC types 1 or 2,
reference-list modification, weighted prediction, adaptive memory-management
operations, or CAVLC/CABAC decoding.
