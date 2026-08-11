# Bound The IDR Picture Identifier

Status: Accepted
Date: 2026-08-11

## Context

The bundled H.264 rule decodes `idr_pic_id` as an unsigned Exp-Golomb field in
every supported type-5 slice header, but it does not yet represent the clause
7.4.3 requirement that the value be in the inclusive range `0..65535`.

This is a semantic value domain rather than a layout selector. The complete
Exp-Golomb codeword still determines the start of the following picture-order
syntax even when its decoded value is out of range. Rejecting the slice would
discard a header whose remaining field boundaries are still known.

## Decision

Add `@range(0, 65535)` to the existing `ue idr_pic_id` declaration. Reuse the
stable unsigned Exp-Golomb range contract: values inside the range produce no
diagnostic, while a larger value attaches a non-fatal `invalid-syntax` warning
to the source-backed field and keeps the enclosing slice materialized.

The warning spans the complete codeword. Decoding continues at its actual end,
so picture-order fields, IDR marking flags, `slice_qp_delta`, optional
deblocking fields, and the opaque `slice_data` boundary are unchanged.

## Consequences

Package `0.1.26` retains coverage depth `picture-order-count-slice-header` and
adds the declared IDR identifier value domain. Regression fixtures cover zero,
the upper boundary `65535`, and the first invalid value `65536`; the invalid
case verifies the 33-bit warning span, materialized slice, unchanged following
field and payload locations, and continued scanning of the next NAL.

## Non-goals

This decision does not bound `first_mb_in_slice`, derive picture size, require
IDR `frame_num` to be zero, validate signed QP or deblocking domains, or add
picture-order, decoded-picture-buffer, MMCO, or output-order semantics.

## Follow-up

ADR-0075 later extended non-fatal `@range` checks to unsigned bit fields and
used that capability to require IDR `frame_num` to be zero without stopping the
remaining slice header.
