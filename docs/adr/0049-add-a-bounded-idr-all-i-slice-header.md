# Add A Bounded IDR All-I Slice Header

Status: Accepted
Date: 2026-08-04

## Context

The bundled H.264 rule publishes exact SPS and PPS generations, imports the PPS
selected by a later consumer, evaluates dynamic field widths from that PPS and
its bound SPS, and can preserve all remaining bits as a compressed payload. The
first rule increment can therefore connect those language slices into a real VCL
NAL without moving H.264 lookup or layout decisions into the Annex B analyzer.

The complete clause 7.3.3 slice header has many layout-changing branches. P and
B slices add reference-list modification, weighted prediction, and different
reference-picture marking. Field pictures, POC types 1 and 2, bottom-field POC,
redundant pictures, deblocking controls, slice groups, SP/SI syntax, and data
partitioning also require additional declared branches. Claiming all of them in
one step would be broader than the current rule and regression fixtures justify.

## Decision

Add `IdrSliceLayerWithoutPartitioningRbsp` and dispatch NAL unit type 5 to it.
This first bounded structure accepts only `slice_type == 2` and imports the PPS
named by `pic_parameter_set_id`. It decodes:

- `first_mb_in_slice`, `slice_type`, and `pic_parameter_set_id`;
- SPS-sized `frame_num` and `pic_order_cnt_lsb`;
- `idr_pic_id`;
- IDR reference-picture-marking flags; and
- `slice_qp_delta`.

Dynamic-width expressions also enforce the layout prerequisites before each
affected field reads source: `frame_mbs_only_flag == 1`, `pic_order_cnt_type ==
0`, `bottom_field_pic_order_in_frame_present_flag == 0`,
`redundant_pic_cnt_present_flag == 0`, and
`deblocking_filter_control_present_flag == 0`. Checked division by a required
one turns a false prerequisite into `invalid-syntax` before the field consumes
bits. The PPS already requires one slice group, so no slice-group-change field
is present. I slices do not carry CABAC-init, reference-list modification, or
prediction-weight syntax.

Follow-up: ADR-0071 and package `0.1.24` remove the POC-type-0 dynamic-width
prerequisite and replace it with declared type-0, type-1, and type-2 branches.

The final `compressed_payload slice_data;` maps every remaining RBSP bit into a
materialized `CompressedPayload` node. The field name follows the H.264 syntax,
but the remaining-bit terminal represents the complete opaque slice-layer
suffix, including entropy-coder alignment and slice trailing bits. It does not
claim to decode CAVLC or CABAC. This exact terminal lets the dispatched
structure satisfy the runner's complete-consumption contract.

Missing, future, or stale PPS/SPS generations remain
`dependency-unavailable` at the source-backed `pic_parameter_set_id`; no older
generation is selected. Failure is local to the VCL NAL and scanning continues.
Package version `0.1.8` advertises the rule change and the entry-point coverage
token becomes `idr-slice-header`.

## Consequences

A common progressive IDR I slice now exposes a source-mapped header and a
bit-precise opaque payload through the same rule execution session as SPS and
PPS. The Annex B analyzer remains format-neutral: the rule declares context
selection, dynamic widths, fatal layout prerequisites, fields, and payload
boundary.

The supported shape is intentionally narrower than the Phase 3 slice-header
goal, which remains incomplete. A rejected branch keeps its already decoded
prefix and source-located diagnostic rather than guessing the following layout.

## Non-goals

This decision does not support non-IDR type 1 slices, `slice_type == 7`, P/B/SP/SI
slices, field pictures, POC type 1 or 2, bottom-field POC, redundant pictures,
deblocking controls, slice groups, data partitioning, reference-list
modification, weighted prediction, adaptive memory-management operations, or
CAVLC/CABAC decoding. It does not claim full Baseline/Main/High slice-header
coverage or complete H.264 conformance.
