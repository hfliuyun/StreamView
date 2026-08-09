# Decode the Bounded Explicit Weighted-Prediction Table

Status: Accepted
Date: 2026-08-09

## Context

Every bounded non-IDR slice-header increment so far has carried two
source-anchored assertions:

```
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;
assert(!is_b_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) != 1)
    at pic_parameter_set_id;
```

They exist because clause 7.3.3 reads `pred_weight_table()` whenever a P or SP
slice uses a PPS with `weighted_pred_flag == 1`, or a B slice uses one with
`weighted_bipred_idc == 1`. Without the assertions the rule would read the
table's bits as the next slice-header field. The consequence is that the bundled
profile rejects every stream using explicit weighting, which ordinary encoder
output does use.

Clause 7.3.3.2 reads `luma_log2_weight_denom` and `chroma_log2_weight_denom`,
then loops `num_ref_idx_l0_active_minus1 + 1` times over a per-entry
`luma_weight_l0_flag` guarding a weight/offset pair and a
`chroma_weight_l0_flag` guarding two Cb and two Cr values. A B slice repeats the
whole loop for list 1.

The table body was never the obstacle. The count was. `num_ref_idx_lX_active_minus1`
exists only when `num_ref_idx_active_override_flag == 1`; otherwise the count comes
from the PPS default. Naming the overridden field from the controller position
failed with `Computed field dependency is not guaranteed on the current branch`,
and the narrowing assertion that would have avoided the choice had no legal
position — at top level it reported the same branch-guarantee error, and inside
the `if` where the flag is available it reported `Assertions must be
unconditional top-level items`.

ADR-0066 added `optional_value(field_identifier, fallback_expression)` for
exactly this dependency, so the count is now expressible without a language
change.

## Decision

Remove both assertions and decode `pred_weight_table()` under a computed
presence guard:

```
computed<bool> uses_explicit_weighting =
    (is_p_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 1) ||
    (is_b_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) == 1);
if (uses_explicit_weighting) {
    ue luma_log2_weight_denom @range(0, 7);
    ue chroma_log2_weight_denom @range(0, 7);
    computed<u64> effective_l0_count =
        optional_value(num_ref_idx_l0_active_minus1,
                       context_value(pic_parameter_set_id, h264_pps,
                                     num_ref_idx_l0_default_active_minus1)) + 1;
    repeat (effective_l0_count, 32) {
        bits<1> luma_weight_l0_flag;
        if (luma_weight_l0_flag == 1) {
            se luma_weight_l0;
            se luma_offset_l0;
        }
        bits<1> chroma_weight_l0_flag;
        if (chroma_weight_l0_flag == 1) {
            se chroma_weight_l0_cb;
            se chroma_offset_l0_cb;
            se chroma_weight_l0_cr;
            se chroma_offset_l0_cr;
        }
    }
    if (is_b_slice) {
        computed<u64> effective_l1_count = /* the same shape for list 1 */;
        repeat (effective_l1_count, 32) { /* the same shape with _l1 names */ }
    }
}
```

A single `computed<bool>` carries the presence condition rather than nested
conditionals. A probe settled this rather than taste: nesting `if (is_p_slice)`
around an imported equality, and doing the same for `is_b_slice`, is rejected
with `Duplicate field name`, because a structure has one flat field namespace and
mutually exclusive branches cannot redeclare a name. Nesting would therefore
force two copies of the entire table under distinct names, which is the cost
ADR-0066 exists to avoid.

Chroma weight fields are unconditionally present. `chroma_format_idc` is declared
only under `profile_idc == 100` and pinned `@equals(1)` there, and the other
supported profiles do not declare it at all, so ChromaArrayType is 1 throughout
the supported subset. No SPS export and no context-contract change is needed.

The `_cb` and `_cr` suffixes are required rather than stylistic. Clause 7.3.3.2
loops twice over a single chroma element, but the single-integer `repeat (n)` form
is always the sentinel form, so a fixed two-iteration loop is inexpressible; the
four fields are written out. The `_l1` suffix follows the existing precedent of
the list 1 modification loop: one flat namespace per structure, so the suffix
disambiguates presentation only and does not denote a different syntax element.

Both loops are bounded at 32, matching the `@range(0, 31)` already carried by the
count fields.

`weighted_bipred_idc == 3` needs no new assertion. `@enum` supplies fatal
validation and `WeightedBipredIdc` declares only 0, 1, and 2, so the reserved
value is already `invalid-syntax` at the PPS — the correct anchor, since that is
where the value is read.

## Consequences

This is the first bundled-rule increment in four that changes decoded output, so
`rule.toml` moves from `0.1.19`. The preceding three were capability-only and
deliberately did not bump.

Twenty-four analyzer tests fail, measured by applying the change and running the
suite rather than estimated:

- **Twenty-two are index shifts.** The child count moves 12 → 13 uniformly,
  because a computed field always materializes as a visible tree node. Every
  non-IDR slice therefore gains one node whether or not it carries a table. This
  is the direct consequence of the existing "computed fields are always
  materialized, no hidden mechanism" design, not an avoidable detail.
- **Two are real semantic changes and were rewritten, not reindexed.**
  `rejectsWeightedNonIdrPSliceAtPictureParameterSetAndContinues` (10 → 19
  children) and
  `rejectsExplicitWeightedBipredictionAtPictureParameterSetAndContinues` asserted
  precisely the limitation this increment removes: the streams they expected to be
  rejected now decode.

`acceptsImplicitWeightedBipredictionInNonIdrBSlice` stays valid unchanged;
`weighted_bipred_idc == 2` does not enter the guard.

The two rewrites became five tests, covering both `optional_value()` leaves in
both of their states:

- `decodesWeightedPredictionTableWithImportedDefaultCountInNonIdrPSlice` — a P
  slice whose list 0 count falls back to the imported PPS default.
- `decodesWeightedPredictionTableWithOverriddenCountInNonIdrPSlice` — a P slice
  whose list 0 count comes from the declared slice-header override.
- `decodesExplicitWeightedBipredictionTableForBothReferenceLists` — a B slice
  where both counts fall back to their imported PPS defaults.
- `decodesExplicitWeightedBipredictionTableWithOverriddenCounts` — a B slice
  where both counts are declared, and the two lists run to different lengths.
- `reportsTruncatedExplicitWeightedPredictionTable` — a table cut short by the
  end of the stream.

Each asserts the full ordered child-name list rather than positional indices, so a
later slice-header increment that inserts a field fails with a name mismatch
instead of a silently-shifted comparison. The truncation case ends the stream
between `luma_weight_l0[0]` and `luma_offset_l0[0]`, confirming the partial prefix
stays materialized and the diagnostic anchors on the unread field. The bitstreams
were assembled by a generator and their decodes read back through `svtool analyze`,
so every asserted value is observed rather than hand-computed.

The analyzer suite finishes at 97 passing.

## Non-goals

Implicit weighted biprediction (`weighted_bipred_idc == 2`) reads no table and is
unchanged. Chroma formats other than 4:2:0 remain outside the supported subset.
The semantics of the decoded weights — how a decoder applies them to prediction
samples — are outside an analyzer's scope; this increment decodes and presents
the syntax. Field-coded and MBAFF streams remain out of scope, so no
`field_pic_flag`-dependent count adjustment applies.
