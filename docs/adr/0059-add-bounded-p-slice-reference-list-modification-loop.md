# Add A Bounded P-Slice Reference-List Modification Loop

Status: Accepted
Date: 2026-08-07

## Context

ADR-0058 added the optional list 0 active-reference count override to the
bounded progressive non-reference P-slice header. The next mandatory field is
`ref_pic_list_modification_flag_l0`, but the bundled rule still constrains it
to zero and therefore rejects the clause 7.3.3 modification list.

For a P slice, a one flag introduces a post-tested list 0 loop. Each operation
starts with unsigned Exp-Golomb `modification_of_pic_nums_idc`: values zero and
one select `abs_diff_pic_num_minus1`, value two selects `long_term_pic_num`,
and value three terminates the list. Other values are not valid operation
codes.

ADR-0047 added the bounded post-tested sentinel repeat for this exact syntax
shape. Unsigned Exp-Golomb fields can be enum and sentinel controllers, and a
repeat-local computed Boolean can combine operation codes zero and one while
preserving a single source-field name for their shared operand. No engine,
context, or expression-language extension is required.

## Decision

Remove the flag's `@equals(0)` constraint. When the flag is one, decode the
list 0 operations with the existing 64-iteration sentinel bound:

```cpp
enum ModificationOfPicNumsIdc {
    subtract_short_term = 0;
    add_short_term = 1;
    long_term = 2;
    end = 3;
}

bits<1> ref_pic_list_modification_flag_l0;
if (ref_pic_list_modification_flag_l0 == 1) {
    repeat (64) {
        ue modification_of_pic_nums_idc @enum(ModificationOfPicNumsIdc);
        computed<bool> uses_abs_diff_pic_num =
            modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1;
        if (uses_abs_diff_pic_num) {
            ue abs_diff_pic_num_minus1;
        }
        if (modification_of_pic_nums_idc == 2) {
            ue long_term_pic_num;
        }
    } until (modification_of_pic_nums_idc == 3);
}
```

The visible `uses_abs_diff_pic_num[index]` computed field has no source
location. It is true for operation codes zero and one, allowing both branches
to publish the canonical `abs_diff_pic_num_minus1[index]` source field without
duplicating a declaration name. Operation code two instead publishes
`long_term_pic_num[index]`. The terminating code three and its false computed
field remain materialized; no operand follows them.

`ModificationOfPicNumsIdc` is a closed enum. An operation code above three
retains its complete Exp-Golomb field and fails fatally at that field because
the following layout is not declared. This is not a non-fatal `@range`
constraint. Operand values remain unconstrained in this slice because their
semantic validity depends on decoded-picture-buffer state that the bundled
profile does not model.

The bound counts completed operations including the terminator. It is a
bundled-profile resource boundary, not a claim that clause 7.4.3 defines a
64-operation conformance limit. If 64 complete operations contain no
terminator, the existing sentinel assertion fails at the final
`modification_of_pic_nums_idc[63]` after preserving the bounded decoded prefix.
Truncation remains transactional at the current operation code or selected
operand. The Annex B analyzer continues scanning a following NAL unit after
any of these local failures.

When the flag is zero, the loop publishes no fields and the existing QP and
opaque payload boundaries remain unchanged. The weighted-prediction and CABAC
PPS assertions stay immediately after the optional loop, followed by
`slice_qp_delta`. The type-1 direct-header assertion continues to require
`nal_ref_idc == 0`, so `dec_ref_pic_marking()` remains excluded.

Package version `0.1.15` advertises the additive rule surface. Coverage depth
remains `i-p-slice-header` because this increment expands the existing P-slice
list 0 branch without adding another slice family.

Regression fixtures cover coded P values zero and five, the flag-zero absence
path, a terminating list after a reference-index override, all operation codes
and selected operands, an unknown operation code, truncated operation and
operand codewords, the 64-operation missing-terminator boundary, exact child
order and source spans, the QP and opaque payload boundary, and continued
scanning of a following NAL unit.

## Consequences

The bounded P-slice header can now describe list 0 short-term subtraction,
short-term addition, and long-term selection operations while keeping the
program statically projected and runtime work bounded. Presentation names
retain their repeat indexes, so each decoded operation and operand remains
individually source-addressable.

The closed operation enum distinguishes layout uncertainty from an ordinary
value-domain warning: an unknown operation cannot silently consume no operand
and continue at a guessed boundary.

## Non-goals

This decision does not add list 1 modifications, B/SP/SI slice types, field
pictures, MBAFF, a dynamic or context-derived list bound, an effective
reference count, decoded-picture-buffer or PicNum validation, weighted
prediction or `pred_weight_table()`, CABAC or `cabac_init_idc`, nonzero-reference
type-1 headers, reference-picture marking, adaptive memory-management
operations, POC types 1 or 2, slice groups, partitioned data, or CAVLC/CABAC
slice-data decoding. It does not change the context model, VM, compiler, or
opaque payload semantics.
