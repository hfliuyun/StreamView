# Add Bounded H.264 HRD Parameters

Status: Accepted
Date: 2026-08-03

## Context

The bundled H.264 VUI core currently requires both HRD presence flags to equal
zero. Annex E.1.2 defines each HRD branch as a bounded schedule: one
`cpb_cnt_minus1`, two scale fields, one through 32 CPB schedule entries, and
four delay-length fields. Annex E.1.1 then includes `low_delay_hrd_flag` when
either the NAL or VCL HRD branch is present.

The stable DSL can express the schedule with a computed count and bounded
`repeat`. It does not invoke one structure from another, and field names remain
unique across all branches of a structure. The two possible HRD instances must
therefore be represented inline with distinct field names rather than by
adding H.264-specific behavior to the analyzer.

## Decision

Remove the `@equals(0)` constraints from the NAL and VCL HRD presence flags.
For each present branch, decode the complete Annex E.1.2 syntax inline with a
`nal_hrd_` or `vcl_hrd_` field prefix:

- `cpb_cnt_minus1`, `bit_rate_scale`, and `cpb_size_scale`;
- the repeated `bit_rate_value_minus1`, `cpb_size_value_minus1`, and `cbr_flag`
  schedule entries; and
- `initial_cpb_removal_delay_length_minus1`,
  `cpb_removal_delay_length_minus1`, `dpb_output_delay_length_minus1`, and
  `time_offset_length`.

Apply non-fatal `@range(0, 31)` to each `cpb_cnt_minus1`. Compute its schedule
count as `cpb_cnt_minus1 + 1` and use it as the controller of
`repeat(..., 32)`. A value above 31 therefore retains a source-located warning
on the decoded count, then stops before schedule entries because the
layout-critical repeat bound is exceeded. The two scale fields and the
computed count may already be materialized at that boundary; no schedule entry
is consumed.

After both optional HRD branches, materialize a computed Boolean that is true
when either presence flag is one. Use it only to conditionally decode the
source-backed `low_delay_hrd_flag`, after which the existing
`pic_struct_present_flag`, bitstream-restriction branch, and SPS trailing bits
continue unchanged.

Keep the package coverage token at `parameter-sets` and publish the added
syntax as package version `0.1.6`.

## Consequences

NAL HRD, VCL HRD, and streams carrying both branches become structurally
visible with bounded schedule projection and exact source spans. Repeated
fields use zero-based materialized indexes matching `SchedSelIdx`. The derived
schedule counts and combined-presence Boolean are visible computed fields with
no source location; every H.264 syntax field remains source-backed.

An out-of-range CPB count produces both kinds of evidence needed by this slice:
the field keeps its non-fatal value-domain warning, while the repeat boundary
prevents an unsupported count from changing the declared layout. Failure of
one SPS remains local and does not prevent scanning later NAL units.

## Non-goals

This decision does not parse buffering-period or picture-timing SEI, use HRD
values to interpret later timing syntax, or register the SPS in the context
directory. It does not claim level-dependent conformance for bitrate, CPB size,
delay lengths, or schedule relationships beyond the declared count bound. It
does not add reusable structure invocation, fatal range annotations, or new
H.264 behavior to the Annex B analyzer.
