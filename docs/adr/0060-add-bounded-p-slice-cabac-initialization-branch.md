# Add A Bounded P-Slice CABAC Initialization Branch

Status: Accepted
Date: 2026-08-07

## Context

ADR-0059 completed the bounded list 0 modification syntax for the progressive
non-reference P-slice header. The bundled rule then uses a source-anchored
assertion to require the selected PPS's `entropy_coding_mode_flag` to be zero.
This preserves the `slice_qp_delta` boundary, but it rejects every CABAC-coded
P slice before the conditional `cabac_init_idc` field can be read.

Clause 7.3.3 places unsigned Exp-Golomb `cabac_init_idc` after weighted
prediction and reference-picture marking syntax and before `slice_qp_delta`
when entropy coding is enabled and the slice type is neither I nor SI. The
current bundled profile accepts only P and I values and requires type-1 direct
headers to have `nal_ref_idc == 0`. Its exact condition is therefore a P slice
whose imported PPS `entropy_coding_mode_flag` equals one; no
`dec_ref_pic_marking()` branch intervenes.

The PPS already exports `entropy_coding_mode_flag`, and the slice structure
already imports that exact PPS generation. Existing nested imported equality
guards and unsigned Exp-Golomb range constraints can express this syntax
without extending the parser, compiler, VM, or context model.

## Decision

Remove only the P-slice assertion that requires
`entropy_coding_mode_flag == 0`. Preserve the weighted-prediction prerequisite,
then read `cabac_init_idc` under nested local and imported guards before
`slice_qp_delta`:

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;

if (is_p_slice) {
    if (context_value(pic_parameter_set_id,
                      h264_pps,
                      entropy_coding_mode_flag) == 1) {
        ue cabac_init_idc @range(0, 2);
    }
}

se slice_qp_delta;
```

The outer guard is required. An I or all-I slice does not contain
`cabac_init_idc` even when its selected PPS enables entropy coding. A false
local or imported guard consumes no source bits and publishes no field. The
two coded P values zero and five share the same branch.

Clause 7.4.3 constrains `cabac_init_idc` to `0..2`. This is a semantic value
domain, not a layout selector for any following slice-header field, so the
rule uses non-fatal `@range(0, 2)` rather than a closed enum. A value above two
retains its complete Exp-Golomb field and exact source span, attaches the
existing warning-severity `invalid-syntax` diagnostic, and continues with
`slice_qp_delta`, optional deblocking syntax, and the opaque `slice_data`
boundary. A truncated Exp-Golomb codeword remains a fatal source read failure
and does not publish a partial field.

The existing weighted-prediction assertion remains immediately before the new
branch because the bundled profile still does not declare
`pred_weight_table()`. The direct type-1 header assertion continues to require
`nal_ref_idc == 0`, so reference-picture marking remains absent.

Package version `0.1.16` advertises the additive field surface. Coverage depth
remains `i-p-slice-header` because this increment completes one optional field
of the existing non-reference P-slice shape without adding another slice
family or decoding its compressed data.

Regression fixtures cover the entropy-disabled absence path; entropy-enabled
P values zero and five; all legal `cabac_init_idc` values; value three with a
warning and an unchanged QP/deblocking/payload boundary; an entropy-enabled
all-I slice with field absence; a truncated codeword; exact child order and
source spans; and continued scanning of a following NAL unit.

## Consequences

The bundled rule can now expose which CABAC initialization table a supported
non-reference P slice selects while retaining exact source mapping and the
existing bounded slice-header projection. Entropy-enabled I slices remain
correctly aligned because the local P-slice guard short-circuits the imported
condition.

Out-of-range initialization identifiers remain visible to an analyst instead
of discarding a header whose subsequent layout is still known. This follows
the same framing-versus-conformance boundary as the existing unsigned
Exp-Golomb range constraints.

## Non-goals

This decision does not decode CABAC or CAVLC `slice_data`, initialize or
validate CABAC context tables, add weighted prediction or
`pred_weight_table()`, add B/SP/SI slice types, add list 1 modification, accept
nonzero-reference type-1 headers, parse reference-picture marking or adaptive
memory-management operations, support field pictures or MBAFF, add POC types
1 or 2, add slice groups or partitioned data, or change opaque payload
semantics. It does not extend the DSL, context model, compiler, VM, or the
diagnostic severity of `@range` and enum constraints.
