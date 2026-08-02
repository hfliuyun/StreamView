# Add A Bounded H.264 PPS Core Slice

Status: Accepted
Date: 2026-08-02

## Context

The bundled H.264 rule now decodes a bounded sequence parameter set core and
reports its declared semantic ranges without losing later fields. The next M6
increment is the picture parameter set carried by NAL unit type 8.

The base PPS syntax can be decoded without looking up its referenced SPS, but
two optional areas cannot. Flexible macroblock ordering introduces slice-group
branches, including a dynamically sized `slice_group_id` field that the stable
DSL cannot express. The PPS extension contains scaling-list syntax whose field
count depends on the referenced SPS `chroma_format_idc`. Treating either area as
opaque inside a structure would lose exact RBSP consumption.

## Decision

Add a type-8 `PictureParameterSetRbsp` payload to the bundled Annex B rule. The
first accepted structure covers the clause 7.3.2.2 base fields when
`num_slice_groups_minus1 == 0` and no PPS extension is present:

- PPS and referenced SPS identifiers;
- entropy coding and bottom-field picture-order flags;
- default reference-index counts and weighted-prediction controls;
- initial QP/QS and chroma QP offsets;
- deblocking, constrained-intra, and redundant-picture controls; and
- `rbsp_trailing_bits;` immediately after the base syntax.

Apply non-fatal `@range` constraints to `pic_parameter_set_id` (`0..255`),
`seq_parameter_set_id` (`0..31`), and both
`num_ref_idx_l*_default_active_minus1` fields (`0..31`). Declare the three
accepted `weighted_bipred_idc` values in an enum so reserved value 3 is not
silently accepted. Require `num_slice_groups_minus1 @equals(0)`; a nonzero
value is layout-critical because the following syntax changes, so it retains
the decoded prefix and stops this PPS as `invalid-syntax`.

The declared structure must consume the entire RBSP. Extra PPS extension bits
therefore fail the existing trailing-bits or exact-consumption checks instead
of being mistaken for supported fields. Package version `0.1.4` advertises the
new rule asset.

A materialized PPS in this slice means that the declared base structure was
consumed exactly. `seq_parameter_set_id` remains a source-backed identifier;
the analyzer does not yet look up an SPS generation, register a PPS generation,
or prove that a later slice header may use this parameter set.

## Consequences

Type-8 NAL units expose a source-mapped PPS base structure through the same
rule-defined payload dispatch as SPS and AUD. Unsupported slice groups,
reserved weighted biprediction, PPS extension syntax, truncation, and malformed
trailing bits remain explicit diagnostics, while semantic ID/count range
violations retain the complete structure as warnings.

The base structure creates a stable field surface for later context
registration and slice-header work without coupling the Annex B analyzer to
H.264-specific branches.

## Non-goals

This decision does not add flexible macroblock ordering, PPS extension or
scaling-list syntax, SPS lookup, PPS registration, dependency generations, or
slice-header dispatch. It does not add signed range constraints; the semantic
bounds for the signed QP fields, including the SPS-bit-depth-dependent
`pic_init_qp_minus26` bound, remain unchecked. It does not claim complete H.264
PPS conformance.
