# Add Bounded Reference-Picture Marking

Status: Accepted
Date: 2026-08-08

## Context

Every bounded non-IDR slice-header increment so far has required
`nal_ref_idc == 0` on a type-1 NAL unit. ADR-0054 introduced that
source-anchored assertion precisely because payload dispatch selects on
`nal_unit_type` alone, so without it the rule would silently interpret
reference-picture marking bits as the next slice-header field. The consequence is
that the bundled profile covers only non-reference slices, which excludes the
common case: a stream whose P and B slices are themselves reference pictures.

Clause 7.3.3 reads `dec_ref_pic_marking()` whenever `nal_ref_idc` is nonzero.
For a non-IDR slice, clause 7.3.3.3 reads
`adaptive_ref_pic_marking_mode_flag`; value zero selects sliding-window marking
and reads nothing further, while value one enters a loop of memory-management
control operations terminated by operation zero. Each operation reads a distinct
operand set: operations 1 and 3 read `difference_of_pic_nums_minus1`, operation 2
reads `long_term_pic_num`, operations 3 and 6 read `long_term_frame_idx`,
operation 4 reads `max_long_term_frame_idx_plus1`, and operations 0 and 5 read no
operand.

ADR-0063 added the `header_value(element_field)` leaf for exactly this
dependency, so the presence condition is now expressible without a language
change.

## Decision

Remove the `assert(nal_unit_type != 1 || nal_ref_idc == 0)` prerequisite and
decode `dec_ref_pic_marking()` under a sequence-element guard:

```cpp
if (header_value(nal_ref_idc) == 0) {
} else {
    bits<1> adaptive_ref_pic_marking_mode_flag;
    if (adaptive_ref_pic_marking_mode_flag == 1) {
        repeat (64) {
            ue memory_management_control_operation
                @enum(MemoryManagementControlOperation);
            computed<bool> marking_uses_pic_num_difference =
                memory_management_control_operation == 1 ||
                memory_management_control_operation == 3;
            if (marking_uses_pic_num_difference) {
                ue difference_of_pic_nums_minus1;
            }
            if (memory_management_control_operation == 2) {
                ue long_term_pic_num_mmco;
            }
            computed<bool> marking_uses_long_term_frame_idx =
                memory_management_control_operation == 3 ||
                memory_management_control_operation == 6;
            if (marking_uses_long_term_frame_idx) {
                ue long_term_frame_idx;
            }
            if (memory_management_control_operation == 4) {
                ue max_long_term_frame_idx_plus1;
            }
        } until (memory_management_control_operation == 0);
    }
}
```

The empty `then` branch is deliberate. An `if` condition accepts only an
equality, so the non-reference case cannot be spelled
`header_value(nal_ref_idc) != 0`. Inverting the branch keeps the guard in the one
form the language admits; the empty path consumes no bits and creates no node.

`MemoryManagementControlOperation` is a closed enum over `0..6`. A reserved value
selects no operand set, so it is layout-critical: every following field would be
misaligned. It therefore fails fatally at the complete Exp-Golomb codeword, the
same framing-versus-conformance boundary as `ModificationOfPicNumsIdc`.
Operation zero is the sentinel and is retained in the tree.

Only one name collides with the flat field namespace: clause 7.3.3.3
`long_term_pic_num` is already projected by the list 0 modification loop. That
one field becomes `long_term_pic_num_mmco`. Unlike ADR-0062, where four names
collided and a uniform `_l1` suffix was clearer, the other four operation fields
are unique to this loop and keep their clause names. The suffix marks a
presentation collision, not a different syntax element.

Two computed Booleans exist because an `if` condition cannot express a
disjunction inline. They mirror `uses_abs_diff_pic_num` in the existing
modification loops and have no source location.

The loop is bounded at 64 operations, the language maximum for a sentinel
repeat. This is a resource bound, not a claimed conformance limit.

The type-5 assertion still requires `nal_ref_idc != 0`, which is correct: an IDR
picture is always a reference picture, and `IdrSliceLayerWithoutPartitioningRbsp`
already declares the `IdrPicFlag` branch of clause 7.3.3.3 as
`no_output_of_prior_pics_flag` and `long_term_reference_flag`.

Package version `0.1.19` advertises the additive field surface. Coverage depth
becomes `i-p-b-reference-slice-header`, because this increment widens which
slices the profile accepts rather than completing an optional field of an
already-accepted shape.

Regression fixtures cover a non-reference slice with no marking fields;
sliding-window marking; each operation's operand set; a multi-operation list; the
terminator retained in the tree; a reserved operation failing at its codeword; a
truncated operation and a truncated operand; exact child order and source spans;
and continued scanning of a following NAL unit.

## Consequences

The bundled profile now accepts reference P and B slices, which is what a real
stream contains. Combined with the preceding increments, the type-1 slice-header
projection covers I, P, and B slices in both reference and non-reference form.

Removing the prerequisite changes an existing behavior rather than only adding
one. A type-1 NAL with nonzero `nal_ref_idc` previously failed at
`nal_ref_idc` before payload mapping; it now decodes. The regression that pinned
the old rejection is replaced rather than amended, because the behavior it
asserted is the thing this increment removes.

The empty `then` branch is the first place the bundled rule inverts a guard to
satisfy the equality-only condition form. If that pattern recurs, admitting
`header_value` into a `computed<bool>` would let a rule write
`computed<bool> is_reference = header_value(nal_ref_idc) != 0;` and read more
directly. That is deferred rather than adopted here, since it widens where the
leaf is admitted.

## Non-goals

This decision does not validate marking semantics, track the decoded-picture
buffer, enforce clause 7.4.3.3 relationships between operations, detect
contradictory or duplicate operations within one list, add
`pred_weight_table()` or explicit weighted biprediction, decode CABAC or CAVLC
`slice_data`, add SP or SI slice types, support field pictures or MBAFF, add POC
types 1 or 2, add slice groups or partitioned data, or change opaque payload
semantics. It does not extend the DSL, context model, compiler, VM, or the
diagnostic severity of `@range`, `@equals`, and enum constraints.
