# Add Source-Anchored Assertion Statements

Status: Accepted
Date: 2026-08-06

## Context

The DSL can reject one decoded field against a constant with `@equals`, and it
can report a non-fatal `ue` value-domain violation with `@range`. It cannot
express a fatal relationship between two already decoded fields without adding
a synthetic computed field or moving format-specific conformance logic into a
C++ analyzer.

H.264 clause 7.4.1 exposes that gap in the direct NAL header. A type-5 IDR NAL
must not have `nal_ref_idc == 0`, but payload dispatch selects only on
`nal_unit_type`. The prerequisite must run after the complete eight-bit header
is decoded and before that NAL's payload is mapped, while a failure diagnostic
must point at the encoded two-bit `nal_ref_idc` field. Later NAL units must
remain recoverable.

## Decision

Add an unannotated structure statement:

```cpp
assert(boolean_expression) at source_field;
```

An assertion is accepted only as an unconditional top-level structure item.
Its condition uses the existing bounded expression and pure-function contract,
must produce `bool`, and may reference only earlier scalar unsigned or computed
values guaranteed on the current path. Imported-context leaves, arrays, signed
Exp-Golomb values, unknown or future fields, and unavailable branch-local
values are rejected as condition dependencies.

The anchor must be an earlier source-backed, non-array scalar syntax field
guaranteed on the current path. Fixed or dynamic `bits`, enum, `ue`, and `se`
fields may be anchors even though `se` is not an expression value. Computed or
generated fields and region items cannot anchor an assertion. One structure contains at
most 1,024 assertions.

The compiler records each statement as a declaration-order
`DslTypedAssertion` containing the typed Boolean condition, anchor field index,
statement-position field index, and source range. It does not add a typed field
or presentation node. Each descriptor emits one `AssertExpression` instruction
whose operand is the descriptor index and whose immediate is zero. At one field
position, the stable order is sentinel completion, expression assertion,
repeat-count assertion, then the next field read. The VM verifies descriptor
cardinality, positions, operands, immediates, and this ordering before source
access for a structure containing expression assertions.

Executing the instruction evaluates the condition without reading source,
moving the reader, or creating a node. `true` continues. `false` is fatal
`invalid-syntax` with message `Assertion condition is false`; it retains the
materialized prefix, stops later fields, and uses the anchor's complete mapped
range for the diagnostic path and source spans. Checked expression failures use
the same anchor and their existing runtime status and message. The instruction
counts toward the instruction budget and remains a cancellation point.

The official H.264 rule declares:

```cpp
assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
```

Package version `0.1.11` advertises this additive rule change while retaining
coverage depth `idr-slice-header`. Regression coverage includes parser and
compiler rejection, the 1,024/1,025 boundary, deterministic positioned
bytecode, malformed typed-IR preflight, multi-span anchor locations,
instruction and cancellation boundaries, a type-5 zero-priority failure before
payload mapping, and continued scanning of a following legal NAL.

## Consequences

Rules can own relational conformance prerequisites without analyzer-specific
branches or synthetic presentation fields. Assertion diagnostics remain tied
to real encoded syntax, including disjoint mapped source spans, while successful
assertions add no source read, cursor movement, or analysis node.

A failed direct-header assertion prevents payload mapping for only that NAL.
The Annex B analyzer retains its complete header as a partial result and
continues at the next scanner record.

## Non-goals

This decision does not add warning assertions, custom messages, assertion
annotations, arrays of assertions, conditional or repeated assertions, imported
context expressions, assertion values, or context exports. It does not make
`se` an expression value and does not claim complete H.264 NAL-header
conformance beyond the type-5 `nal_ref_idc` prerequisite.

It also does not expand non-IDR, P/B/SP/SI, field-picture, reference-list,
weighted-prediction, adaptive-memory-management, slice-group, or entropy-coded
slice-data support.
