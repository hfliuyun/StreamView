# Add A Bounded List 1 Reference-List Modification Loop

Status: Accepted
Date: 2026-08-08

## Context

ADR-0061 added the bounded non-reference B-slice header but stopped short of
list 1 reference-picture modification. It read
`ref_pic_list_modification_flag_l1` and constrained it with `@equals(0)`,
because the list 0 loop's projected field names already occupied the
structure's flat namespace and a nonzero flag would misalign every following
field.

Clause 7.3.3.1 `ref_pic_list_modification()` contains two structurally
identical loops. The first runs when the slice is neither I nor SI; the second
runs when the slice is B. Both loops read `modification_of_pic_nums_idc`, then
`abs_diff_pic_num_minus1` for operation codes 0 and 1, or `long_term_pic_num`
for operation code 2, and terminate on operation code 3. The specification
reuses the same syntax element names in both loops because each loop has its
own scope.

The rule language has no such scope. A structure has one flat field namespace,
and a sentinel repeat projects its body into that namespace, so the second loop
cannot reuse the first loop's names. The language also caps a sentinel repeat
maximum at 1..64, which fixes the iteration bound rather than leaving it to
profile choice.

## Decision

Replace the `@equals(0)` constraint with the real loop, mirroring the list 0
shape and disambiguating the projected names with an `_l1` suffix:

```cpp
if (is_b_slice) {
    bits<1> ref_pic_list_modification_flag_l1;
    if (ref_pic_list_modification_flag_l1 == 1) {
        repeat (64) {
            ue modification_of_pic_nums_idc_l1 @enum(ModificationOfPicNumsIdc);
            computed<bool> uses_abs_diff_pic_num_l1 =
                modification_of_pic_nums_idc_l1 == 0 ||
                modification_of_pic_nums_idc_l1 == 1;
            if (uses_abs_diff_pic_num_l1) {
                ue abs_diff_pic_num_minus1_l1;
            }
            if (modification_of_pic_nums_idc_l1 == 2) {
                ue long_term_pic_num_l1;
            }
        } until (modification_of_pic_nums_idc_l1 == 3);
    }
}
```

The `_l1` suffix is a presentation disambiguator, not a distinct syntax
element. Clause 7.3.3.1 names both loops' elements identically, so each list 1
field keeps the clause reference and a description that states which list it
modifies. A future language scope construct could remove the suffix without
changing the decoded bit layout.

The loop reuses the existing closed `ModificationOfPicNumsIdc` enum, so a
reserved operation code remains fatal at the complete Exp-Golomb codeword and
the terminator value 3 is retained in the tree. A flag of zero publishes no loop
fields at all. The 64-iteration bound is the language maximum for a sentinel
repeat; a list that never terminates fails at the final projected operation and
retains the bounded prefix.

Both loops are now bounded independently, so a B slice may project up to 128
operations. The list 0 loop is unchanged, and list 1 remains absent for P and
all-I slices because the enclosing `is_b_slice` guard consumes no bits when
false.

Package version `0.1.18` advertises the additive field surface. Coverage depth
remains `i-p-b-slice-header` because this increment completes an optional branch
of the existing B-slice shape rather than adding a slice family.

Regression fixtures cover a zero flag with no loop fields; a first-iteration
terminator; the full operation set 0, 1, 2, and 3 in one list 1 loop;
independent list 0 and list 1 loops in the same slice; a reserved operation
code; a truncated operation and a truncated operand; a list that never
terminates; exact child order and source spans; and continued scanning of a
following NAL unit.

## Consequences

The bundled rule now decodes the complete `ref_pic_list_modification()` syntax
for every slice type it supports, so an analyst can see both reference lists'
reordering operations with exact source mapping.

The cost is a visible naming deviation. A reader comparing the tree against
clause 7.3.3.1 sees `modification_of_pic_nums_idc_l1[0]` where the
specification writes `modification_of_pic_nums_idc`. The suffix is documented in
both language references and in each field's description, and it is the direct
consequence of the flat namespace rather than a modeling choice.

A B slice whose both lists are heavily reordered now projects substantially more
fields than before. This stays far inside the structure field limit, but it does
increase the compiled bytecode for this structure.

## Non-goals

This decision does not add `pred_weight_table()` or explicit weighted
biprediction, decode CABAC or CAVLC `slice_data`, add a language scope construct
that would let both loops share clause names, raise the sentinel repeat maximum,
add SP or SI slice types, accept nonzero-reference type-1 headers, parse
reference-picture marking or adaptive memory-management operations, support
field pictures or MBAFF, add POC types 1 or 2, add slice groups or partitioned
data, or change opaque payload semantics. It does not extend the DSL, context
model, compiler, VM, or the diagnostic severity of `@range`, `@equals`, and enum
constraints.
