# Bound PPS Quantization Parameter Offsets

Status: Accepted
Date: 2026-08-14

## Context

ADR-0041 introduced the base `PictureParameterSetRbsp` payload and ADR-0073
added the High-profile extension. Both decisions decoded `pic_init_qs_minus26`,
`chroma_qp_index_offset`, and `second_chroma_qp_index_offset` as signed
Exp-Golomb fields (`se`), but accepted every value representable by the mapping
without range checks.

ADR-0076 extended non-fatal `@range(minimum, maximum)` constraints to `se`
fields and bounded the slice deblocking offsets.

ITU-T H.264 clause 7.4.2.2 defines the conformant semantic domains for the PPS
QP offsets:

- `pic_init_qs_minus26` shall be in the range `-26` to `25`, inclusive;
- `chroma_qp_index_offset` shall be in the range `-12` to `12`, inclusive;
- `second_chroma_qp_index_offset` (when the PPS extension is present) shall be
  in the range `-12` to `12`, inclusive.

These are semantic value domains rather than layout selectors. The complete
signed Exp-Golomb codeword still determines the start offset of subsequent
fields (such as `deblocking_filter_control_present_flag` or `rbsp_trailing_bits`)
and the opaque payload boundary. An out-of-range value is non-conformant, but
rejecting the PPS or terminating parsing would discard a structure whose
subsequent field offsets and following NAL units remain unambiguous.

In contrast, `pic_init_qp_minus26` has a conformant range of
`-(26 + QpBdOffsetY)` to `25 + QpBdOffsetY`, which depends dynamically on the
referenced SPS `bit_depth_luma_minus8` (`QpBdOffsetY = 6 * bit_depth_luma_minus8`).
Because `@range` accepts only literal integer constants and cannot express
relational or context-dependent bounds, `pic_init_qp_minus26` remains
unconstrained at this layer and deferred.

## Decision

Apply static non-fatal `@range` annotations and clause 7.4.2.2 references to the
literal-domain PPS QP offsets in `PictureParameterSetRbsp`:

- `se pic_init_qs_minus26 @range(-26, 25)`
- `se chroma_qp_index_offset @range(-12, 12)`
- `se second_chroma_qp_index_offset @range(-12, 12)` inside the PPS extension
  branch.

Reuse the signed Exp-Golomb `@range` contract established in ADR-0076: values
within range generate no diagnostics; values outside attach a warning-severity,
source-located `invalid-syntax` diagnostic to the field, preserve the complete
codeword span, keep the PPS structure materialized, and continue decoding
subsequent fields and following NAL units.

Bump the package version to `0.1.29` while retaining coverage depth
`picture-order-count-slice-header`.

## Consequences

The bundled H.264 profile reports out-of-range PPS QP offsets without stopping
analysis or drifting downstream bit offsets. Regression coverage verifies:

- Legal extreme values (`-26` and `25` for QS, `-12` and `12` for chroma
  offsets) produce zero diagnostics;
- The first illegal values (`-27` and `26` for QS, `-13` and `13` for chroma
  offsets) produce exactly one `invalid-syntax` warning diagnostic with the
  correct field path and message;
- Equal-length Exp-Golomb codeword pairings prove that subsequent field starts
  (`deblocking_filter_control_present_flag`, `rbsp_trailing_bits`) and
  following NAL units do not shift between legal and illegal probes;
- Sibling fields in `PictureParameterSetRbsp` remain unaffected.

## Non-goals

This decision does not bound `pic_init_qp_minus26` or `slice_qp_delta`, whose
conformant domains depend on SPS `QpBdOffsetY`. It does not add
expression-valued bounds or relational range constraints. It does not validate
slice-group parameters, PPS scaling matrices, POC derivation, DPB management,
or output ordering.

## Follow-up

- ADR-0041: Add A Bounded H.264 PPS Core Slice
- ADR-0073: Decode The Bounded High Profile PPS Extension
- ADR-0076: Extend Non-Fatal Ranges To Signed Fields
