# Decode The Bounded High-Profile PPS Extension

Status: Accepted
Date: 2026-08-11

## Context

The bundled H.264 rule currently accepts the clause 7.3.2.2 picture parameter
set base syntax and requires `rbsp_trailing_bits` immediately after
`redundant_pic_cnt_present_flag`. High-profile streams may instead carry an
optional PPS extension containing transform, scaling-matrix, and second chroma
QP-offset syntax. The extension is present only when `more_rbsp_data()` is true;
the referenced SPS profile alone cannot distinguish it from a legal base-only
High-profile PPS.

ADR-0072 added the non-consuming source-state expression needed for that
presence test. The remaining stable DSL can import the referenced SPS profile,
anchor a fatal assertion at the source-backed SPS identifier, and decode the
bounded extension fields. Scaling matrices remain layout-critical: when their
presence flag is set, one or more scaling lists occur before the final signed
offset, and their count depends on SPS state outside this increment.

## Decision

Export the unconditional SPS `profile_idc` field. Keep the PPS dependency on
the referenced SPS generation and also import that generation while decoding
the PPS. The dependency preserves later PPS-consumer invalidation; the import
provides the profile value needed by the extension gate.

After the existing base PPS fields, publish
`computed<bool> has_pps_extension = more_rbsp_data()`. A source-anchored
assertion requires either no extension or a referenced `profile_idc` of 100.
The assertion is anchored at `seq_parameter_set_id`, so Baseline, Main, or
Extended streams that carry extra PPS syntax fail before any extension field is
read. A base-only PPS short-circuits the imported lookup and keeps its existing
behavior.

When `has_pps_extension` is true, decode these fields in bitstream order:

- `transform_8x8_mode_flag`;
- `pic_scaling_matrix_present_flag @equals(0)`; and
- signed Exp-Golomb `second_chroma_qp_index_offset`.

A scaling-matrix flag of one is a fatal `invalid-syntax` failure at that flag's
single source bit. The rule does not read the offset through an unsupported
scaling-list layout. `rbsp_trailing_bits;` remains the final, unconditional
top-level item and consumes either the base-only or extended terminal pattern.

Context lookup keeps the established source-order generation policy. An
extension cannot import an SPS first published later in the stream, a failed
SPS redefinition does not hide the latest prior valid generation, and a PPS
registered against one generation becomes stale when a later valid SPS
generation replaces it.

## Consequences

Package `0.1.25` adds the bounded High-profile PPS extension while retaining
coverage depth `picture-order-count-slice-header`, because the deepest supported
slice-header surface does not change. Existing base-only PPS output gains one
computed presence node; an accepted extension gains that node and three
source-backed fields.

Regression fixtures cover base-only and extended High-profile PPS forms, both
transform flag values, positive and negative second chroma offsets with exact
source spans, scaling-matrix rejection, field truncation, non-High profile
rejection, missing/future/stale SPS generations, failed redefinition recovery,
and continued scanning after a failed NAL.

## Non-goals

This decision does not decode PPS scaling lists, flexible macroblock ordering,
or other unsupported SPS profiles and chroma formats. It does not validate the
signed QP-offset value domains, derive transform or quantization semantics, or
add picture-order, decoded-picture-buffer, or output-order state.
