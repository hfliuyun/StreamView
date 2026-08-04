# Parse PPS-Controlled IDR Slice-Header Branches

Status: Accepted
Date: 2026-08-06

## Context

The bundled H.264 package `0.1.9` decodes a bounded progressive, POC-type-0,
all-I IDR slice header. It uses checked dynamic-width division to reject three
optional syntax groups that the selected PPS can enable: bottom-field picture
order, redundant-picture count, and deblocking-filter control. ADR-0052 now
lets the rule guard fields with exact values exported by the imported PPS, so
those layout decisions no longer need rejection placeholders.

The complete slice header remains much broader. Non-IDR pictures, P and B
slices, field pictures, other POC types, reference-list modification, weighted
prediction, adaptive memory management, and slice groups each require separate
bounded rule increments and fixtures.

## Decision

Remove the three PPS-presence divisors from
`IdrSliceLayerWithoutPartitioningRbsp` and declare the corresponding clause
7.3.3 syntax in field order:

- after `pic_order_cnt_lsb`, read signed `delta_pic_order_cnt_bottom` only when
  `bottom_field_pic_order_in_frame_present_flag == 1`;
- before the IDR reference-picture-marking flags, read `redundant_pic_cnt` only
  when `redundant_pic_cnt_present_flag == 1`, with the clause 7.4.3 value domain
  `0..127` represented as a non-fatal `@range` warning; and
- after `slice_qp_delta`, read deblocking-filter control only when
  `deblocking_filter_control_present_flag == 1`.

Declare `disable_deblocking_filter_idc` as unsigned Exp-Golomb syntax over the
named enum domain `enabled = 0`, `disabled = 1`, and `enabled_within_slice = 2`.
A reserved value is fatal at its complete codeword because it controls whether
following fields exist. Value 1 omits both signed offsets; values 0 and 2 read
`slice_alpha_c0_offset_div2` and `slice_beta_offset_div2`. The DSL has no `!=`
condition, so the rule expresses this as an empty `== 1` branch with the two
offsets in `else`. Signed semantic bounds that the current DSL cannot express
remain documented but unenforced.

Each presence decision uses the exact imported-context equality form from
ADR-0052. A false guard consumes no source bits and creates no field node. The
remaining `compressed_payload slice_data` therefore begins immediately after
the last selected optional field and continues to include every remaining RBSP
bit, including slice trailing bits.

Package version `0.1.10` advertises this additive rule change while retaining
the `idr-slice-header` coverage token. Regression fixtures cover all three
enabled presence flags, deblocking values with and without offsets, false-guard
field absence, exact opaque-payload boundaries, and a reserved deblocking value
followed by a NAL unit that still materializes.

## Consequences

The official rule now decodes the three PPS-controlled optional groups for its
existing progressive all-I IDR shape without moving H.264-specific decisions
into the analyzer. Imported guards compose with the local deblocking enum guard,
and only selected fields affect the source-mapped header boundary.

The Phase 3 Baseline/Main/High slice-header item remains incomplete. This
increment removes three deliberate prerequisites but does not broaden the NAL,
slice-type, frame-picture, or POC-type domains.

## Non-goals

This decision does not support non-IDR NAL units, P/B/SP/SI syntax, field
pictures, POC type 1 or 2, data partitioning, reference-list modification,
weighted prediction, adaptive memory-management operations, slice groups, or
CAVLC/CABAC decoding. It does not add general imported expressions or signed
range constraints to the DSL.
