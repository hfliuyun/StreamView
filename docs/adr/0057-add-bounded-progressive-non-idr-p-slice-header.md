# Add A Bounded Progressive Non-IDR P-Slice Header

Status: Accepted
Date: 2026-08-07

## Context

The bundled H.264 rule decodes bounded progressive all-I IDR and non-IDR
slice headers. ADR-0055 limits NAL unit type 1 to non-reference pictures, so
that path does not need `dec_ref_pic_marking()`. ADR-0056 now lets a
source-anchored assertion combine local syntax with an exact imported PPS
value.

The next useful type-1 increment is a non-reference P-slice. Its header adds
reference-list syntax before `slice_qp_delta`, and the selected PPS may also
enable `pred_weight_table()` or `cabac_init_idc`. Treating any of those bits as
an opaque suffix would move the slice-data boundary, while silently omitting
them would mislabel syntax as `slice_qp_delta`.

The first increment therefore needs to read the two mandatory P-slice control
flags and reject every branch whose variable syntax is not yet modeled. The
existing all-I path must remain valid in the same type-1 dispatch.

## Decision

Rename the type-1 enum and structure to reflect their broader package-visible
surface:

- `NonIdrAllISliceType` becomes `NonIdrSliceType`;
- `NonIdrAllISliceLayerWithoutPartitioningRbsp` becomes
  `NonIdrSliceLayerWithoutPartitioningRbsp`.

`NonIdrSliceType` declares the supported H.264 coded values:

```cpp
enum NonIdrSliceType {
    p = 0;
    i = 2;
    all_p = 5;
    all_i = 7;
}
```

Immediately after `slice_type`, publish the presentation-visible computed
field:

```cpp
computed<bool> is_p_slice = slice_type == 0 || slice_type == 5;
```

For P values 0 and 5, read these clause 7.3.3 fields after the existing
picture-order and redundant-picture branches and before `slice_qp_delta`:

```cpp
bits<1> num_ref_idx_active_override_flag @equals(0);
bits<1> ref_pic_list_modification_flag_l0 @equals(0);
```

The bits are mandatory P-slice syntax even when their values are zero. A one
value fails at the complete controlling bit, before the unsupported reference
count or modification loop can affect layout.

After those flags, require the exact selected PPS to disable both omitted
branches:

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, entropy_coding_mode_flag) == 0)
    at pic_parameter_set_id;
```

Boolean short-circuiting leaves the all-I values unchanged. For P values, a
violated PPS prerequisite is fatal `invalid-syntax` anchored to the complete
`pic_parameter_set_id` codeword. Missing, future, or stale generations retain
the existing `dependency-unavailable` behavior. The type-1 direct-header
assertion still requires `nal_ref_idc == 0`, so no reference-picture marking
syntax is present.

The common picture-order, redundant-picture, QP, deblocking-control, and
opaque `slice_data` fields retain their current order and semantics. Package
version `0.1.13` advertises the additive rule and presentation-name change with
coverage depth `i-p-slice-header`.

Regression fixtures cover P slice types 0 and 5, the unchanged all-I path,
both nonzero control flags, both imported PPS prerequisites, exact field and
payload spans, and continued scanning of a following NAL unit.

## Consequences

The bundled rule can expose the first bounded P-slice header without claiming
to parse the variable reference-list, weighted-prediction, reference-picture
marking, or CABAC branches. Supported P headers place the opaque slice-data
boundary after the two mandatory zero flags and `slice_qp_delta`.

The generic enum and structure names replace the previous all-I presentation
names. Consumers pinned to package `0.1.12` retain the old surface; consumers
of `0.1.13` see the new names and the additional visible `is_p_slice` node.

## Non-goals

This decision does not add B/SP/SI slice types, nonzero-reference type-1
headers, field pictures, POC types 1 or 2, reference-index override syntax,
the reference-list modification loop, weighted prediction or
`pred_weight_table()`, CABAC or `cabac_init_idc`, reference-picture marking,
adaptive memory-management operations, slice groups, partitioned data, or
CAVLC/CABAC slice-data decoding. It does not add a new context mechanism or
change the semantics of opaque NAL types.
