# Add A Bounded P-Slice Reference-Index Override

Status: Accepted
Date: 2026-08-07

## Context

ADR-0057 added bounded progressive non-reference P-slice headers for coded
slice types 0 and 5. The rule reads
`num_ref_idx_active_override_flag`, but currently requires it to be zero and
therefore always inherits the list 0 active-reference count from the selected
PPS.

Clause 7.3.3 places an unsigned Exp-Golomb
`num_ref_idx_l0_active_minus1` immediately after a one override flag for P and
SP slices. B slices additionally carry a list 1 count, but those slice types
remain outside the bundled profile. The count does not change whether the
following `ref_pic_list_modification_flag_l0` bit is present.

The existing DSL already supports a local scalar condition, a guarded `ue`
field, and a non-fatal unsigned range. No engine or context extension is
needed for this syntax branch.

## Decision

Keep `num_ref_idx_active_override_flag` as mandatory P-slice syntax, but remove
its `@equals(0)` constraint. When its value is one, read the list 0 override in
clause order:

```cpp
bits<1> num_ref_idx_active_override_flag;
if (num_ref_idx_active_override_flag == 1) {
    ue num_ref_idx_l0_active_minus1 @range(0, 31);
}
bits<1> ref_pic_list_modification_flag_l0 @equals(0);
```

The override count means that the slice uses
`num_ref_idx_l0_active_minus1 + 1` active list 0 entries instead of the PPS
default. Values `0..31` are conformant for the supported progressive frame
path. A value above 31 retains the complete codeword, emits the existing
source-located `invalid-syntax` warning, and continues decoding because the
Exp-Golomb length and all later field positions remain known.

When the flag is zero, the override field is absent and the existing P-slice
child order and bit boundaries remain unchanged. The rule does not publish a
computed effective-reference count because no currently decoded downstream
syntax consumes that value.

The reference-list modification flag remains mandatory and constrained to
zero. The weighted-prediction and CABAC PPS assertions remain after that flag,
and the type-1 direct-header assertion continues to exclude
`dec_ref_pic_marking()` by requiring `nal_ref_idc == 0`.

Package version `0.1.14` advertises the additive rule surface. Coverage depth
remains `i-p-slice-header` because the increment expands an existing P-header
branch without adding another slice family.

Regression fixtures cover coded P values 0 and 5 with a non-default override,
the zero-flag absence path, an out-of-range count warning without payload
misalignment, a truncated count, a still-unsupported list modification after
an override, exact source spans, and continued scanning of a following NAL
unit.

## Consequences

The bounded P-slice header can now describe its own list 0 active-reference
count while keeping every later unsupported layout branch explicit. The new
source-backed field appears only when the controlling flag is one, between the
flag and `ref_pic_list_modification_flag_l0`.

The non-fatal range diagnostic separates a conformance value problem from
layout uncertainty: value 32 is warned about, but does not move or hide the QP
and opaque slice-data boundary.

## Non-goals

This decision does not add list 1 override counts, B/SP/SI slice types, field
pictures or the MBAFF-specific `0..15` limit, a computed effective-reference
count, validation against the decoded picture buffer, the reference-list
modification loop, weighted prediction or `pred_weight_table()`, CABAC or
`cabac_init_idc`, nonzero-reference type-1 headers, reference-picture marking,
adaptive memory-management operations, POC types 1 or 2, slice groups,
partitioned data, or CAVLC/CABAC slice-data decoding. It does not add a new
context mechanism or change opaque NAL semantics.
