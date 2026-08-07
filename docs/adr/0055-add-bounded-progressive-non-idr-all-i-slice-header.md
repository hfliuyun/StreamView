# Add A Bounded Progressive Non-IDR All-I Slice Header

Status: Accepted
Date: 2026-08-07

## Context

The bundled H.264 rule now decodes a bounded progressive all-I IDR slice
header, including PPS-controlled picture-order, redundant-picture, and
deblocking branches. NAL unit type 1 is still left as an opaque payload. A
non-IDR all-I header shares the SPS/PPS-dependent frame and picture-order
fields, but it does not carry `idr_pic_id` or IDR reference-picture-marking
flags.

The complete non-IDR syntax also has reference-picture marking whenever
`nal_ref_idc` is nonzero. That branch is not yet modeled, and payload dispatch
selects by NAL type rather than by the direct header's reference priority. The
next increment therefore needs an explicit, source-located prerequisite instead
of silently interpreting reference-marking bits as the next field.

## Decision

Add a second direct-header assertion:

```cpp
assert(nal_unit_type != 1 || nal_ref_idc == 0) at nal_ref_idc;
```

This bounds the first type-1 payload slice to non-reference pictures. A type-1
NAL with nonzero `nal_ref_idc` fails after its complete header is decoded and
before EBSP/RBSP payload mapping; its header remains a partial result and later
NAL units continue scanning.

Add `NonIdrAllISliceLayerWithoutPartitioningRbsp` and dispatch NAL type 1 to it.
The structure accepts only `slice_type` values 2 and 7, requires the existing
progressive SPS and POC-type-0 dynamic-width prerequisites, imports the exact
PPS/SPS generations, and reads in clause order:

- `first_mb_in_slice`, `slice_type`, `pic_parameter_set_id`, `frame_num`, and
  `pic_order_cnt_lsb`;
- PPS-controlled `delta_pic_order_cnt_bottom` and `redundant_pic_cnt`;
- `slice_qp_delta` and PPS-controlled deblocking-filter control; and
- opaque `compressed_payload slice_data` for every remaining RBSP bit.

The IDR-only `idr_pic_id`, `no_output_of_prior_pics_flag`, and
`long_term_reference_flag` fields are absent. Because the direct assertion
rejects nonzero reference priority, the structure does not claim to parse
`dec_ref_pic_marking`.

Package version `0.1.12` advertises the additive rule change with coverage
depth `all-i-slice-header`. Regression fixtures cover a valid type-1 all-I
header, exact field/payload spans, the unsupported nonzero-reference boundary,
and continued scanning of a following NAL. Existing opaque-fixture NALs move
to type 12 so they continue to test the undispatched payload path.

## Consequences

The first non-IDR slice header becomes source-mapped and recoverable without
pretending to decode reference-list or picture-management syntax. The same
PPS/SPS generation and mapped-payload machinery serves both bounded all-I
shapes, while unsupported reference pictures fail before their payload is
misinterpreted.

## Non-goals

This decision does not add P/B/SP/SI slice types, nonzero-reference non-IDR
headers, field pictures, POC types 1 or 2, reference-list modification,
weighted prediction, adaptive memory-management operations, slice groups,
partitioned data, or CAVLC/CABAC decoding. It does not add a new header-context
publication mechanism or change the semantics of opaque NAL types.
