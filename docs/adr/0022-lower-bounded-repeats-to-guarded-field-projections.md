# Lower Bounded Repeats To Guarded Field Projections

Status: Accepted
Date: 2026-07-27

## Context

Fixed arrays cover repeated scalar declarations whose count is known at compile
time. Media syntax also contains repeated multi-field entries whose count was
decoded from an earlier field. The declaration-order VM has no back edges,
general expression evaluator, or mutable loop index, and its field indexes and
work accounting must remain statically bounded and locally validatable.

## Decision

The first bounded-loop slice will accept nested
`repeat (count_field, maximum) { ... }` blocks. `count_field` must resolve to an
earlier scalar unsigned `bits`, enum, or `ue` field that is guaranteed to be
materialized on every path reaching the repeat. Arrays and signed `se` fields
are rejected. `maximum` is a positive unsigned integer literal, must fit a
fixed-width controller when one is used, and statically bounds the complete
body projection. A repeat body must contain at least one field and may contain
fields, equality conditionals, equality switches, and nested repeats.

The compiler projects the body `maximum` times into the existing linear typed
field stream. Fields in iteration `i` receive a positive `count_field > i`
presence guard in addition to their enclosing guards. Materialized names append
the enclosing repeat indexes before an optional fixed-array index: a scalar is
named `value[i]`, an array element is named `value[i][j]`, and nested repeats
continue the same suffix order. Source field declaration names remain unique
across the complete structure. A body declaration is visible to later items in
the same iteration, but repeat-local declarations are not visible in another
iteration or after the repeat.

Each projected repeat also emits one guarded bound assertion at the statement
position. A reached repeat whose decoded count exceeds `maximum` reports
`invalid-syntax` at the count field before consuming body input; the runtime
never clamps the count. The assertion and every projected read or equality
assertion count toward the instruction budget and remain cancellation points,
including instructions for absent iterations. Only fields from iterations
whose guard succeeds consume source bits or materialized-node budget. All
projected fields count toward the existing 99,999-field structure limit.

This lowering adds a greater-than presence comparison and a repeat-bound
assertion, but no jump, back edge, or mutable loop state. Typed repeat metadata,
its controller, enclosing guards, and bytecode placement are validated before
or during execution like the existing typed field and assertion metadata.
Unsigned Exp-Golomb values are retained as possible repeat controllers without
making them valid equality-conditional or switch controllers.

Static offsets are checked while projecting each possible iteration so fixed
alignment errors inside the body remain diagnosable. Because the runtime count
may select any number of iterations from zero through `maximum`, the offset
after a repeat is not statically known. A later little-endian field is therefore
rejected until a future constant or expression analysis can prove its
alignment.

## Consequences

Rules can express bounded count-prefixed entry lists while preserving linear
bytecode, deterministic field indexes, selected-only source access, and the
existing cancellation and partial-result model. The typed program grows with
the declared maximum rather than the decoded count, so conservative maxima
consume static field and instruction capacity even for short inputs.

This slice does not expose a loop index and does not accept arithmetic counts
such as `count_minus_one + 1`, sentinel or EOF termination, `break`, mutable
state, structure composition, or paged/lazy list materialization. Those forms
require computed fields, general expressions, statement control flow, or lazy
runtime work and remain separate language decisions.

## Considered Options

- A literal-only `repeat (3)`: easy to expand, but substantially duplicates the
  accepted fixed-array slice and does not express count-prefixed media syntax.
- Runtime loop and jump opcodes: compact for large maxima, but introduce program
  counters, back-edge validation, mutable iteration state, and new cancellation
  accounting before the general statement runtime exists.
- Encoding `count > i` as `count != 0 && ... && count != i`: representable with
  equality guards, but produces quadratic guard storage and evaluation that is
  not charged by the current instruction budget.
