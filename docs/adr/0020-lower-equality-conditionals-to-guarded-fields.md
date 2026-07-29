# Lower Equality Conditionals To Guarded Fields

Status: Accepted
Date: 2026-07-27

## Context

The declaration-order VM has deterministic field indexes and no general
control-flow validation. The first conditional feature needs branch semantics
without weakening those invariants or prematurely defining a full expression
language.

## Decision

The first conditional slice will accept nested C-style
`if (previous_field == integer) { ... } else { ... }` blocks, with an optional
`else`. The controlling name must resolve to an earlier scalar `bits` or enum
field that is guaranteed to be materialized on every path reaching the
condition. Arrays, `ue`/`se`, general expressions, and branch-local values that
are not guaranteed on the current path are rejected. Field names remain unique
across the complete structure, and both branches are statically validated.

[ADR-0023](0023-inline-pure-scalar-functions-into-computed-fields.md) later
extended equality controllers to earlier guaranteed `computed<u64>` fields and
added `if (computed_bool)` shorthand for earlier guaranteed `computed<bool>`
fields. The current normative rules live in the
[format-language reference](../format-language/README.md).

The compiler will lower conditional blocks into declaration-order typed fields
carrying resolved presence guards. It will not add general jump bytecode. The
VM validates and evaluates those guards before a field read: an absent field
consumes no source bits, creates no analysis node, and performs no enum or
equality value check. Its read and optional assertion instructions still count
toward the instruction budget and remain cancellation points. This preserves
the existing monotonic field-index and malformed-bytecode checks while keeping
unselected input inaccessible.

Static alignment is analyzed per branch. A conditional exit retains a known
offset only when the then and else paths (with an omitted `else` treated as an
empty path) end at the same known offset. All possible branch fields count
toward the structure expansion limit, while only selected fields consume the
materialized-node budget.

## Consequences

Rules can express nested equality-selected layouts while the bytecode remains
linear and deterministic. All possible fields increase typed-IR size and the
static field limit even when a particular execution selects only a small
branch. Skipped read and assertion instructions still consume instruction
budget, so large alternatives do not bypass cancellation or work accounting.

## Considered Options

- General jump opcodes: compact for large skipped blocks, but they would break
  the current one-pass field-index invariant and substantially expand control
  flow validation before the expression language exists.
- A field-level `@if` annotation: smaller parser work, but not C-style block
  syntax and awkward for nested alternatives, arrays, and future statements.
- A complete expression VM first: eventually useful, but unnecessarily widens
  the initial condition, type, overflow, and sandbox contracts.
