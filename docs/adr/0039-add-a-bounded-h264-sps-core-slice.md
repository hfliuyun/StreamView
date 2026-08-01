# Add A Bounded H.264 SPS Core Slice

Status: Accepted
Date: 2026-08-02

## Context

The bounded RBSP trailing-bits terminal makes it possible to consume a
variable-length H.264 sequence parameter set exactly. The bundled rule still
dispatches only access-unit delimiter payloads, while the implementation plan
requires SPS structure before PPS, VUI/HRD, and slice-header work.

H.264 SPS syntax has profile-dependent scaling lists, several Exp-Golomb
branches, and optional VUI/HRD syntax. The stable DSL can express the core
fields and bounded repeat projections, but this first slice should not pretend
to parse optional syntax that has no complete rule representation yet.

## Decision

Add a type-7 `SequenceParameterSetRbsp` payload to the bundled Annex B rule.
The first accepted structure covers the common 8-bit Baseline/Main/Extended core and
the High subset with 4:2:0 chroma, eight-bit luma/chroma, transform bypass
disabled, and no scaling matrix:

- NAL header and profile/level identity, constraint flags, and SPS ID;
- frame-number and picture-order-count type 0;
- reference-frame, picture-size, frame-structure, and cropping fields; and
- the `vui_parameters_present_flag` boundary followed by
  `rbsp_trailing_bits;`.

The rule accepts `profile_idc` values 66, 77, 88, and 100 through an enum. Other
profile values are invalid at the profile field in this slice. A present
VUI flag, High profile values outside the supported 8-bit 4:2:0 subset, and
any unimplemented picture-order branch must not
be silently consumed; their resulting RBSP remains invalid until a later
slice adds the corresponding fields.

This first structural slice does not yet enforce the clause 7.4.2.1.1
`0..12` semantic bounds on `log2_max_frame_num_minus4` or
`log2_max_pic_order_cnt_lsb_minus4`. Their source-backed values are published,
but full range conformance remains the next SPS constraint slice. A
materialized structure therefore means exact consumption of the declared SPS
subset, not complete H.264 semantic conformance.

Unsigned Exp-Golomb fields may carry one `@equals` constraint. They remain
valid repeat controllers; this does not make them equality or switch
controllers. The compiler lowers the constraint to the existing
`assert-equals` instruction, and the VM reports a source-located invalid-syntax
diagnostic using the materialized Exp-Golomb range.

## Consequences

Type-7 NAL units now produce a source-mapped SPS structure for the supported
core and reject unsupported optional syntax without analyzer-specific H.264
branches. The rule can be extended in place for scaling lists, VUI/HRD, PPS,
and position-aware registration after each syntax slice has its own bounded
contract and tests.

## Non-goals

This decision does not add context-directory registration, PPS references,
VUI/HRD parsing, scaling-list values, or slice-header dispatch. It also does
not broaden `@equals` to signed Exp-Golomb fields or runtime-sized arrays, and
does not yet add general `ue` range constraints.
