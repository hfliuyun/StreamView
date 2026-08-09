# Decode the Field-Picture Slice Header

Status: Accepted
Date: 2026-08-09

## Context

ADR-0049 established an idiom every slice-header increment since has carried:
dividing a dynamic bit width by a flag that must be one turns a false layout
prerequisite into `invalid-syntax` before the field consumes bits. Both slice
structures still open with

```
bits<(context_value(pic_parameter_set_id,
                    h264_sps,
                    log2_max_frame_num_minus4) + 4) /
     context_value(pic_parameter_set_id, h264_sps, frame_mbs_only_flag)> frame_num
```

The divisor is not part of the field's width. Clause 7.4.3 gives `frame_num`
exactly `log2_max_frame_num_minus4 + 4` bits whether the picture is frame- or
field-coded; the division is there only to reject `frame_mbs_only_flag == 0`.
That rejection is the last one standing between the bundled profile and the
Baseline/Main/High slice header, and it is unusually broad — it fails every
interlaced and every MBAFF stream at the first slice header regardless of what
the rest of that header contains.

Clause 7.3.3 reads, immediately after `frame_num`:

```
if (!frame_mbs_only_flag) {
    field_pic_flag
    if (field_pic_flag)
        bottom_field_flag
}
```

The same clause also makes the `delta_pic_order_cnt_bottom` condition compound:
the field is present when the imported
`bottom_field_pic_order_in_frame_present_flag` is one **and** `field_pic_flag` is
zero. A field picture carries no bottom-field delta, because it *is* one field.

Two language restrictions meet at that condition. An `if` on an imported leaf
must be the exact form `if (context_value(a, b, c) == <integer>)`, so a compound
condition cannot be written as a condition at all. And `field_pic_flag` is itself
conditional, so naming it from an unconditional position is a branch-guarantee
error. Folding the pair into a `computed<bool>` answers the first restriction;
ADR-0066's `optional_value` answers the second.

## Decision

Remove the `/ frame_mbs_only_flag` divisor from `frame_num` in both slice
structures, and decode the field-picture fields immediately after it:

```
    if (context_value(pic_parameter_set_id,
                      h264_sps,
                      frame_mbs_only_flag) == 0) {
        bits<1> field_pic_flag;
        if (field_pic_flag == 1) {
            bits<1> bottom_field_flag;
        }
    }
```

Replace the imported-equality guard on `delta_pic_order_cnt_bottom` with a
computed guard carrying the clause's full condition:

```
    computed<bool> has_delta_pic_order_cnt_bottom =
        context_value(pic_parameter_set_id,
                      h264_pps,
                      bottom_field_pic_order_in_frame_present_flag) == 1 &&
        optional_value(field_pic_flag, 0) == 0;
    if (has_delta_pic_order_cnt_bottom) {
        se delta_pic_order_cnt_bottom;
    }
```

The `optional_value` fallback is `0`, which is the value clause 7.4.3 infers for
`field_pic_flag` when it is absent: a stream with `frame_mbs_only_flag == 1`
codes frames only. The fallback therefore restates the specification's inference
rather than choosing a convenient default, and progressive streams keep exactly
the behaviour they had before this increment.

A probe settled three uncertain points rather than leaving them to reasoning.
Imported-equality conditions accept `== 0` and not only `== 1`. A `bits<1>` field
nests inside an imported-equality block together with a further inner `if`. And a
`computed<bool>` may combine an imported leaf with an `optional_value` under
`&&` — ADR-0066 exempts only the first argument from the branch-guarantee rule,
and this expression relies on precisely that exemption.

`pic_order_cnt_lsb` keeps its `/ (1 - pic_order_cnt_type)` divisor. POC types 1
and 2 remain out of scope, and that ADR-0049 guard is independent of this one.
This increment removes one of the five prerequisites that ADR-0049 listed, not
the idiom.

Accepting `frame_mbs_only_flag == 0` admits MBAFF frames as well as field
pictures, since an MBAFF stream sets that flag to zero and then codes
`field_pic_flag == 0`. This is deliberate and needs no additional syntax: the two
cases share an identical slice-header layout, and macroblock-adaptive
frame/field coding changes only how the opaque `slice_data` is interpreted, which
an analyzer at this depth does not decode. Naming MBAFF as separately deferred
would misdescribe the rule, because its headers now decode.

## Consequences

`rule.toml` moves to `0.1.21` and advertises coverage depth
`field-picture-slice-header`. Decoded output changes for progressive streams too,
because a computed field always materializes as a visible tree node.

Thirty-four analyzer tests failed, measured by applying the change and running
the suite rather than estimated. Every one was the same +1 child shift, and none
was a semantic change — this increment only adds syntax that was previously
rejected outright, so no existing expectation about a decoded stream became
wrong. Thirty of the thirty-four asserted a literal child count; eleven ordered
name vectors gained the `has_delta_pic_order_cnt_bottom` entry.

The two failure modes were not equally safe, which is worth recording. Tests
asserting an ordered name vector failed with a readable name mismatch. Tests
indexing children positionally instead **crashed with SIGSEGV**: `at(8)` still
returned a valid node, but it was now the computed guard, whose `location()` is
empty, so the following `->location()->sourceSpans()` dereferenced an empty
optional. A positional index does not merely go stale here, it goes stale
silently and then faults somewhere unrelated. This is the concrete argument for
the ADR-0067 convention of asserting the full ordered name list.

The insertion index was measured through `svtool analyze` on a decoded stream of
each kind rather than derived by reading the rule: index 8 for a progressive
non-IDR slice, index 7 for an IDR slice, the difference being `idr_pic_id` and
the three non-IDR `computed<bool>` slice-type fields.

Five tests were added:

- `decodesTheBottomFieldPictureNonIdrSliceHeader` — a bottom field whose PPS sets
  `bottom_field_pic_order_in_frame_present_flag`, so it proves a field picture
  suppresses `delta_pic_order_cnt_bottom` rather than merely not having one.
- `decodesTheMbaffFrameNonIdrSliceHeaderWithBottomFieldPictureOrderDelta` — an
  MBAFF frame, `field_pic_flag == 0` with the delta present, covering the other
  side of the same guard.
- `decodesTheTopFieldPictureIdrSliceHeader` — the IDR structure, where the field
  flags land before `idr_pic_id`.
- `reportsTruncationBetweenFieldPictureFlagAndBottomFieldFlag` — a twelve-bit
  `frame_num` places `field_pic_flag` on the final bit of the payload, so
  `bottom_field_flag` has no source left; the partial prefix stays materialized
  and the diagnostic anchors on the unread field.
- `omitsFieldPictureFlagsForAProgressiveSequence` — the progressive regression:
  no field flags materialize and the `optional_value` fallback holds.

Fixtures were emitted by a generator that first reproduces two already-committed
fixtures byte-for-byte as a self-check, so a generator bug surfaces there rather
than as a hand-tuned expectation inside a test.

The analyzer suite finishes at 102 passing; `dev`, `ci`, and `sanitize` are each
32/32.

## Non-goals

Field-pair and complementary-field-pair semantics are outside an analyzer's
scope at this depth: the increment decodes and presents `field_pic_flag` and
`bottom_field_flag`, and does not derive picture-order counts, pair up
consecutive fields, or validate that a stream's fields alternate. MBAFF
macroblock layout stays inside the opaque `slice_data`. POC types 1 and 2 remain
rejected at `pic_order_cnt_lsb`.

`num_ref_idx_l0_active_minus1` keeps its `@range(0, 31)` bound rather than being
tightened to the frame-picture bound of 15. Clause 7.4.3 caps the value at 15 for
frame pictures and 31 for field pictures, but that is a semantic constraint on a
fixed-width interpretation, not a layout prerequisite — the field reads the same
bits either way, and a violation would not desynchronize the header. Splitting
the bound by picture type is left to the marking- and reference-semantics
validation already deferred to clause 7.4.3.3.

SP/SI slice types remain deferred and remain Extended-profile only, so they are
not part of the Baseline/Main/High slice-header milestone this increment
advances.
