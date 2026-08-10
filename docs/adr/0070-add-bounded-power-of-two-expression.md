# Add a Bounded Power-of-Two Expression

Status: Accepted
Date: 2026-08-10

## Context

H.264 clause 7.4.3.3 bounds `difference_of_pic_nums_minus1` by `MaxPicNum`.
For the supported SPS subset, `MaxPicNum` is `2^(log2_max_frame_num_minus4 + 4)`
for a frame picture and twice that value for a field picture. The DSL already
supports checked unsigned arithmetic and imported context leaves, but it has no
bounded exponentiation or shift operation. A hand-written lookup would be
format-specific and would not scale to other width-derived relations.

## Decision

Add the reserved expression leaf
`power_of_two(unsigned_expression)`. It accepts exactly one `u64` operand,
returns `u64`, and is available anywhere the full bounded expression grammar is
accepted, including pure functions, computed fields, dynamic widths, lazy byte
counts, and assertions. The exponent is evaluated with the existing checked
expression semantics; an exponent of `0..63` returns `1 << exponent`, while an
exponent of `64` or greater is a fatal `invalid-syntax` evaluation failure.

The parser/compiler validate the arity and unsigned operand before source
execution. The typed IR stores one `PowerOfTwo` expression node, and the VM
validates its operand before evaluating it. The operation adds no source read,
presentation node, or bytecode instruction beyond the enclosing expression's
existing instruction.

The H.264 rule uses the leaf in the repeat-local assertion for operation 1/3
`difference_of_pic_nums_minus1`, multiplying by `optional_value(field_pic_flag,
0) + 1` to account for frame versus field `MaxPicNum` in the supported slice
shapes. The assertion remains local to the current marking iteration.

## Consequences

Width-derived powers can be expressed without a general shift operator or a
format-specific helper. The operation remains bounded by the `u64` domain and
the existing expression node/depth/work limits. DPB state, picture-number wrap
and operation ordering remain outside this increment.

## Non-goals

This decision does not add bitwise operators, general exponentiation, mutable
state, array indexing, or complete decoded-picture-buffer conformance.
