# Extend Non-Fatal Ranges To Signed Fields

Status: Accepted
Date: 2026-08-14

## Context

ADR-0040 introduced non-fatal `@range(minimum, maximum)` checks for `ue` fields
and ADR-0075 extended them to unsigned fixed-width and dynamic-width `bits`
fields. Both decisions explicitly excluded `se` fields and signed bounds.

H.264 clause 7.4.3 requires `slice_alpha_c0_offset_div2` and
`slice_beta_offset_div2` to lie in `-6..6`, because each one is doubled to form
the `FilterOffsetA` and `FilterOffsetB` deblocking offsets. The bundled rule
reads both as `se` and currently accepts every value the signed Exp-Golomb
mapping can represent. An out-of-range offset is non-conformant, yet the
complete codeword still determines where the following field and the opaque
`slice_data` payload begin.

The annotation grammar cannot express such a bound. Annotation arguments accept
only a bare integer literal, so a leading `-` is a parse error rather than a
negative bound. The existing source-anchored `assert` statement is fatal and
would stop the structure, and a one-off warning assertion would duplicate the
stable range diagnostic and bytecode behavior.

## Decision

Extend `@range(minimum, maximum)` from unsigned encodings to `se` fields.

Annotation arguments accept an optional leading `-` before an integer literal.
The annotation value keeps an unsigned magnitude and a separate negative flag
rather than a signed integer, because unsigned `@range` bounds must still span
the full `ue` domain up to `2^64 - 2`, which no signed 64-bit type can hold. A
negative bound remains invalid on `bits` and `ue` fields.

The static domain for a signed bound is `-(2^63 - 1)` to `2^63 - 1`. That
interval is symmetric because the signed Exp-Golomb mapping derives its value
from `magnitude = (codeNumber + 1) / 2` over a code number of at most
`2^64 - 2`, so neither sign can reach `-2^63`. The parser and compiler retain
the existing arity, uniqueness, and ordered-bound rules, and compare the bounds
of a signed field in signed space.

No new opcode is added. A signed field carries a `DslTypedSignedRange`
descriptor and reuses `assert-range-minimum` and `assert-range-maximum`, whose
immediate holds the two's-complement pattern of the bound. The VM selects the
signed or unsigned comparison from the field encoding, and its bytecode
preflight requires the descriptor to match that encoding: a signed constraint on
an unsigned field, an unsigned constraint on a signed field, or both constraints
at once is a malformed typed IR that consumes no bits, moves no reader, and
creates no node. A violation attaches a warning-severity, source-located
`invalid-syntax` diagnostic to the field, preserves the complete field span,
keeps the enclosing structure materialized, and continues with later fields.

Apply `@range(-6, 6)` to `slice_alpha_c0_offset_div2` and
`slice_beta_offset_div2` in both the IDR and the non-IDR slice header, with an
ITU-T H.264 clause 7.3.3 and 7.4.3 reference. Package `0.1.28` retains coverage
depth `picture-order-count-slice-header`.

## Consequences

The bundled profile reports an out-of-range deblocking offset without hiding the
remaining header or the opaque payload. Regression coverage includes negative
annotation literals, the legal extremes `-6` and `6`, the first illegal values
`-7` and `7`, a violation on each of the two offsets, unchanged following field
and payload locations, malformed typed IR for every rejected descriptor pairing,
and continued scanning of the next NAL.

Signed semantic domains can now reuse the same annotation without inventing
format-specific warning statements. The DSL reference must describe `@range` as
covering signed as well as unsigned encodings, and must state the signed static
domain alongside the existing type-specific unsigned maxima.

## Non-goals

This decision does not extend `@range` to computed, lazy, compressed payload, or
generated trailing-bit fields. It does not add expression-valued bounds, a
general warning assertion, rule-selected severity, or a custom diagnostic
message. It does not bound `slice_qp_delta`, whose conformant domain depends on
the active SPS and PPS, and it does not validate `first_mb_in_slice`, derived
picture order, decoded-picture-buffer state, MMCO-5, or output order.

## Follow-up

ADR-0077 and package `0.1.29` subsequently applied signed `@range` bounds to the
literal-domain PPS QP offsets: `pic_init_qs_minus26` (`-26..25`),
`chroma_qp_index_offset` (`-12..12`), and `second_chroma_qp_index_offset`
(`-12..12`).
