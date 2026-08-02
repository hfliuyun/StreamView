# Add A Bounded H.264 VUI Core Slice

Status: Accepted
Date: 2026-08-02

## Context

The bundled H.264 SPS currently requires `vui_parameters_present_flag == 0`.
With bounded SPS and PPS base structures in place, the next M6 increment is the
VUI syntax that describes sample aspect ratio, video signal properties, timing,
and decoder restrictions.

Most VUI branches have fixed fields or bounded Exp-Golomb values and can be
represented by the stable DSL. HRD parameters are different: they introduce a
counted schedule followed by arrays of delay-length and bitrate values, and are
used by later buffering-period and picture-timing SEI syntax. They need their
own contract and fixtures rather than a partial interpretation inside this
slice.

## Decision

Replace the SPS `vui_parameters_present_flag @equals(0)` boundary with an
optional inline VUI core following Annex E.1.1. When the flag is one, decode:

- aspect-ratio information, including the Extended SAR width and height;
- overscan information;
- video format, full-range, colour primaries, transfer characteristics, and
  matrix coefficients;
- top- and bottom-field chroma sample locations;
- timing information;
- the NAL and VCL HRD presence boundaries;
- `pic_struct_present_flag`; and
- the complete bitstream-restriction branch.

Require both HRD presence flags to equal zero. A present HRD branch changes the
following layout, so it retains the decoded prefix and stops the SPS as
`invalid-syntax`. The absence of both HRD branches means
`low_delay_hrd_flag` is not present and `pic_struct_present_flag` follows them
directly.

Apply non-fatal `@range(0, 5)` constraints to both chroma sample-location
types. Apply `@range(0, 16)` to `max_bytes_per_pic_denom`,
`max_bits_per_mb_denom`, and both `log2_max_mv_length_*` fields. Other VUI
values remain source-backed but are not claimed semantically conformant when
the stable DSL lacks the required fixed-width or relational constraint.

The existing SPS `rbsp_trailing_bits;` remains the exact terminal after the
optional VUI fields. Package version `0.1.5` publishes the added syntax without
changing the `parameter-sets` coverage token.

## Consequences

Common VUI metadata is now visible with precise source spans without adding
H.264-specific parsing to the Annex B analyzer. Unsupported HRD syntax is
explicit at its presence flag, and a failed SPS does not prevent later NAL
units from being scanned.

The inline field surface can later be extended with HRD parameters and then
reused by SEI work. A materialized VUI core means exact consumption of the
declared branches, not complete Annex E semantic conformance.

## Non-goals

This decision does not parse NAL or VCL HRD parameters, buffering-period or
picture-timing SEI, or SPS extension syntax. It does not validate reserved
fixed-width table values, nonzero SAR/timing values, timing ratios, or the
relationship between `max_num_reorder_frames`, `max_dec_frame_buffering`, and
SPS-derived decoder limits. It does not add SPS context registration or change
the bounded PPS slice.
