# Report Ue Range Violations Without Stopping Decoding

Status: Accepted
Date: 2026-08-02

## Context

The bounded H.264 SPS core slice publishes `log2_max_frame_num_minus4` and
`log2_max_pic_order_cnt_lsb_minus4` without enforcing the clause 7.4.2.1.1
`0..12` bounds. ADR-0039 deferred those bounds to this slice.

Every checked constraint the DSL has so far is layout-critical. A failed
`@equals(0)` on `forbidden_zero_bit` or `reserved_zero_2bits`, and an
undeclared enum member, all mean the rule's bit-position assumptions are
wrong, so no later field in the structure can be trusted. Those constraints
therefore mark the structure invalid and stop it.

A semantic range bound is different. When `log2_max_frame_num_minus4`
decodes as 13, the Exp-Golomb codeword itself was read correctly and every
following field is still at exactly the offset the rule predicts. The value
is non-conformant, not unparseable. Stopping the structure would discard a
fully recoverable SPS and hide the picture size, cropping, and frame
structure that an analyst needs in order to understand the stream.

## Decision

Add a `@range(minimum, maximum)` field annotation for unsigned Exp-Golomb
fields, and report violations as non-fatal.

The annotation is static: it appears at most once per field, only on a `ue`
field, and takes exactly two integer arguments with
`minimum <= maximum <= 2^64 - 2`. The parser and the compiler both reject
violations of those rules, so a malformed constraint is a definition error
rather than a runtime surprise.

A violation retains the materialized field node, attaches a
source-located `invalid-syntax` diagnostic with `warning` severity to that
field, and continues executing the structure. The structure's own state is
untouched: a structure whose only problem is a range violation still reaches
`materialized`, and the enclosing NAL unit is not marked invalid. Execution
status stays successful, so the analyzer keeps dispatching later payloads
exactly as before.

This deliberately diverges from `@equals`. `@equals` answers "is my
framing correct"; `@range` answers "is this value conformant". Only the
first is a reason to stop.

The compiler lowers one constraint to two instructions,
`assert-range-minimum` and `assert-range-maximum`, each carrying the
respective bound as its immediate. Two instructions keep every immediate a
plain bound value and let each diagnostic name the bound that was actually
violated. Both instructions are reserved in the structure's budget whether
or not the field is selected, matching existing guarded-field accounting.

The bundled rule applies `@range(0, 12)` to
`log2_max_frame_num_minus4` and `log2_max_pic_order_cnt_lsb_minus4`, and the
package version becomes `0.1.3`.

## Consequences

An analyst sees a complete SPS together with an explicit, source-located
conformance warning on the offending field, instead of a truncated structure.
Later semantic bounds in H.264 SPS, PPS, VUI, and slice-header syntax can
reuse the same annotation and the same non-fatal reporting contract.

Diagnostic severity now carries meaning that consumers must respect: a field
diagnostic no longer implies its structure failed. The field inspector and
diagnostics panel already render severity from the core model, so warnings
surface without presentation changes.

## Non-goals

This decision does not add `@range` to `bits`, `se`, computed, or lazy
fields, and does not make a range-constrained field ineligible as a repeat
controller. It does not introduce a general expression-valued constraint, a
rule-selectable severity, or a way to promote a range violation to a fatal
error. It does not revisit `@equals` semantics, and it does not add the
remaining SPS optional syntax deferred by ADR-0039.

## Follow-up

ADR-0075 later extended this same non-fatal contract to unsigned fixed-width
and dynamic-width `bits` fields. The original `ue` scope above records the
boundary of this first slice rather than the final language capability.
