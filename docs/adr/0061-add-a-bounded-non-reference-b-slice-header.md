# Add A Bounded Non-Reference B-Slice Header

Status: Accepted
Date: 2026-08-08

## Context

ADR-0057 through ADR-0060 completed the bounded progressive non-reference
P-slice header: the reference-index override, the list 0 modification loop, and
the conditional `cabac_init_idc` field. `NonIdrSliceType` still accepts only the
I values 2 and 7 and the P values 0 and 5, so every B slice fails at the
`slice_type` codeword before any later field is read.

Clause 7.3.3 places three B-only syntax elements inside the existing bounded
shape. `direct_spatial_mv_pred_flag` precedes the reference-count override.
`num_ref_idx_l1_active_minus1` follows `num_ref_idx_l0_active_minus1` inside the
override branch. `ref_pic_list_modification()` reads
`ref_pic_list_modification_flag_l1` after the list 0 loop. Both P and B read
`num_ref_idx_active_override_flag`, `ref_pic_list_modification_flag_l0`, the list
0 loop, and `cabac_init_idc`; only I and SI omit them.

Two properties of the existing rule language decide the achievable boundary.
Field names occupy one flat namespace per structure, so two mutually exclusive
branches cannot both declare `num_ref_idx_active_override_flag`. An `if`
condition accepts only a `computed<bool>` identifier, a field equality, or an
imported `context_value` equality, so a shared branch cannot spell
`is_p_slice || is_b_slice` inline.

## Decision

Extend `NonIdrSliceType` with `b = 1` and `all_b = 6`, and widen the existing
P-slice branches into shared reference-list branches rather than adding parallel
B-only copies:

```cpp
computed<bool> is_p_slice = slice_type == 0 || slice_type == 5;
computed<bool> is_b_slice = slice_type == 1 || slice_type == 6;
computed<bool> uses_reference_lists = is_p_slice || is_b_slice;

if (is_b_slice) {
    bits<1> direct_spatial_mv_pred_flag;
}
if (uses_reference_lists) {
    bits<1> num_ref_idx_active_override_flag;
    if (num_ref_idx_active_override_flag == 1) {
        ue num_ref_idx_l0_active_minus1 @range(0, 31);
        if (is_b_slice) {
            ue num_ref_idx_l1_active_minus1 @range(0, 31);
        }
    }
    bits<1> ref_pic_list_modification_flag_l0;
    if (ref_pic_list_modification_flag_l0 == 1) {
        repeat (64) { ... } until (modification_of_pic_nums_idc == 3);
    }
    if (is_b_slice) {
        bits<1> ref_pic_list_modification_flag_l1 @equals(0);
    }
}
```

Sharing one block is required, not merely economical: a separate B-only block
would redeclare `num_ref_idx_active_override_flag` and
`ref_pic_list_modification_flag_l0` and be rejected as a duplicate field name.
`uses_reference_lists` exists for the same reason, because an `if` condition
cannot combine the two type predicates inline.

List 1 modification is bounded by `@equals(0)` rather than a second loop. The
list 0 loop's projected field names — `modification_of_pic_nums_idc`,
`uses_abs_diff_pic_num`, `abs_diff_pic_num_minus1`, and `long_term_pic_num` —
already occupy the flat namespace, so a list 1 loop would require a second set of
distinct names and a second 64-iteration projection. The constraint is
layout-critical: a nonzero flag introduces modification operations that this
profile cannot parse, so every following field would be misaligned. Value one is
therefore fatal at that bit and retains the decoded prefix.

Weighted prediction gains a second source-anchored prerequisite. Clause 7.3.3
invokes `pred_weight_table()` for a B slice when `weighted_bipred_idc` equals
one, so explicit biprediction must fail while default (0) and implicit (2)
biprediction stay supported:

```cpp
assert(!is_b_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) != 1)
    at pic_parameter_set_id;
```

The existing P-slice `weighted_pred_flag == 0` assertion is unchanged, and both
remain unconditional top-level items because the language forbids assertions
inside a branch and forbids referencing a guarded field from one.

`cabac_init_idc` moves from the `is_p_slice` guard to `uses_reference_lists`,
matching the clause condition that the slice is neither I nor SI. The type-1
direct-header assertion still requires `nal_ref_idc == 0`, so
`dec_ref_pic_marking()` remains absent and these are non-reference B slices.

Package version `0.1.17` advertises the additive field surface, and coverage
depth becomes `i-p-b-slice-header`.

This increment changes the type-1 presentation shape. The structure now
publishes three top-level computed Booleans instead of one, so every child index
after `slice_type` shifts by two for both I and P slices. The two new nodes have
no source location and consume no bits.

Regression fixtures cover B values 1 and 6; `direct_spatial_mv_pred_flag` in both
states; the list 0 and list 1 override counts together and the list 0 count alone
when the slice is P; a B slice reusing the list 0 modification loop; a nonzero
list 1 modification flag failing at its bit; explicit biprediction failing at
`pic_parameter_set_id` while implicit biprediction succeeds; `cabac_init_idc`
present for an entropy-coded B slice; exact child order and source spans; and
continued scanning of a following NAL unit.

## Consequences

The bundled rule covers the three coded slice families that a bounded
non-reference progressive stream can contain, and a B slice now exposes its
direct-prediction mode, both active reference counts, and its CABAC
initialization table with exact source mapping.

Sharing one reference-list block keeps a single definition of the syntax that P
and B have in common, so later increments extend one path instead of two. The
cost is that the flat namespace now constrains where list 1 modification can go:
supporting it requires distinct projected names for the second loop.

Existing type-1 tests that address slice children positionally must shift by two.
This is a visible presentation change rather than a syntax change, and no field's
source span or value is affected.

## Non-goals

This decision does not add `pred_weight_table()` or explicit weighted
biprediction, decode CABAC or CAVLC `slice_data`, add the list 1 modification
loop, add SP or SI slice types, accept nonzero-reference type-1 headers, parse
reference-picture marking or adaptive memory-management operations, support
field pictures or MBAFF, add POC types 1 or 2, add slice groups or partitioned
data, or change opaque payload semantics. It does not extend the DSL, context
model, compiler, VM, or the diagnostic severity of `@range`, `@equals`, and enum
constraints.
