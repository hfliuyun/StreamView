# Decode Picture-Order-Count Types One And Two

Status: Accepted
Date: 2026-08-11

## Context

Before this increment, the bundled H.264 rule decoded SPS and slice-header
picture-order syntax only for `pic_order_cnt_type == 0`. The SPS rejected every
other type with `@equals(0)`, and both slice structures encoded that prerequisite
as a divisor inside the `pic_order_cnt_lsb` width. Baseline, Main, and High
streams may also use types 1 and 2, so this was the broadest remaining
slice-header syntax boundary.

The existing DSL can express every required syntax branch. Bounded repeats can
project the type-1 SPS offset cycle, `optional_value` can normalize fields that
exist only in one SPS branch, imported equality guards can select a POC type,
and a computed repeat count can preserve the specified
`delta_pic_order_cnt[0]` and `[1]` presentation names. No new parser, IR, VM, or
analyzer behavior is required.

## Decision

Replace the SPS `@equals(0)` constraint with a closed `PicOrderCntType` enum
containing values 0, 1, and 2. A reserved value remains a fatal
`invalid-syntax` failure at the complete Exp-Golomb codeword because it would
select no defined layout.

The SPS selects these mutually exclusive branches after `pic_order_cnt_type`:

- type 0 reads the existing ranged `log2_max_pic_order_cnt_lsb_minus4`;
- type 1 reads `delta_pic_order_always_zero_flag`, the two signed offsets,
  ranged `num_ref_frames_in_pic_order_cnt_cycle`, and at most 255 projected
  `offset_for_ref_frame[index]` values; and
- type 2 reads no additional POC fields.

The count carries a non-fatal `@range(0, 255)` diagnostic and also controls
`repeat(..., 255)`. A value above 255 therefore preserves the source-backed
warning and then fails before an undeclared cycle entry can shift the remaining
SPS layout.

Conditional source fields cannot be context exports. Two unconditional
computed values therefore normalize them after the SPS branches:
`effective_log2_max_pic_order_cnt_lsb_minus4` falls back to zero outside type 0,
and `effective_delta_pic_order_always_zero_flag` falls back to one outside type
1. Slice guards ensure neither fallback is used to read a field from the wrong
POC branch.

Both IDR and non-IDR slice structures retain their existing type-0 field order.
Type 1 reads no delta when the effective always-zero flag is one. Otherwise a
computed count is one, plus one when the PPS enables the second delta and the
slice codes a frame; a bounded two-entry repeat then publishes
`delta_pic_order_cnt[0]` and optionally `[1]`. Type 2 reads no POC syntax before
the next slice-header field.

## Consequences

Package `0.1.24` advertises coverage depth
`picture-order-count-slice-header`. Type-0 streams keep their existing slice
field sequence. SPS presentation gains two computed normalization nodes, and
type-1 slices with explicit deltas gain one computed count node.

Tests cover all three SPS branches, a type-1 offset cycle (including an empty
cycle), always-zero and explicit one/two-delta slices, field-picture suppression
of the second delta, type-2 IDR and non-IDR field absence, a reserved POC type,
count overflow, truncation, exact source spans, and continued scanning after a
failed NAL.

## Non-goals

This decision does not derive `PicOrderCnt`, `TopFieldOrderCnt`, or
`BottomFieldOrderCnt`; track `FrameNumOffset`, wrap state, MMCO 5, field pairs,
or output order; validate offset sums or signed value domains; or introduce a
decoded-picture buffer. Those are ordering and DPB semantics, not clause 7.3
syntax, and remain deferred.
