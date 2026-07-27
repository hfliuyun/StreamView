# Inline Pure Scalar Functions Into Computed Fields

Status: Accepted
Date: 2026-07-27

## Context

Format authors need derived flags and bounded arithmetic values without
consuming source bits or pretending that derived values have source locations.
The accepted language already has declaration-order values, guarded field
projections, bounded repeats, and a linear VM, but it has no general expression
model, runtime call stack, or mutable state. Introducing unrestricted functions
would expand both the language and sandbox substantially.

## Decision

The first computed-value slice will accept top-level expression-bodied pure
scalar functions and structure-local computed fields:

```cpp
pure bool between(u64 value, u64 low, u64 high) {
    return value >= low && value <= high;
}

struct NalUnitHeader {
    bits<5> nal_unit_type;
    computed<bool> is_vcl = between(nal_unit_type, 1, 5);
}
```

The public scalar types are `bool` and `u64`. A pure function has a typed return
value, at most 16 uniquely named typed parameters, and exactly one `return`
expression. It may reference only its parameters and previously declared pure
functions. Function names share the top-level declaration namespace, overloads
and forward calls are rejected, annotations are not accepted, and declaration
order makes direct or indirect recursion impossible. Every function body is
type-checked and must independently satisfy the expression size and depth limits
even when the function is unused.

A computed field has a declared scalar type and one expression. It may reference
earlier scalar unsigned `bits`, enum, `ue`, or computed fields that are
guaranteed to be materialized on every path reaching the declaration. Arrays,
`se`, future or unknown declarations, and unavailable branch-local values are
rejected. Computed and syntax field names remain unique across the complete
structure. Computed fields may carry `@description` and `@spec` metadata, but
not `@equals`, `@enum`, or an array suffix.

Expressions accept unsigned integer and Boolean literals, identifiers, pure
function calls, parentheses, unary `!`, checked `*`, `/`, `%`, `+`, and `-`,
same-type `==` and `!=`, unsigned `<`, `<=`, `>`, and `>=`, and short-circuit
Boolean `&&` and `||`, with conventional C precedence. There are no implicit
conversions: arithmetic and ordering use `u64`, logical operators use `bool`,
and equality operands must have the same type. Unsigned overflow, underflow,
division by zero, and remainder by zero are runtime `invalid-syntax` failures at
the computed field path. A source enum contributes its decoded unsigned `u64`
value; enum member names are not expression values in this slice.

The compiler expands pure function calls into each computed expression. An
expanded expression contains at most 256 nodes and has depth at most 64. Pure
functions therefore introduce no runtime call opcode, recursion, dynamic
dispatch, or access to the source, analysis tree, host, time, randomness, or
mutable state. These fixed bounds keep one computed evaluation bounded in the
same way that one Exp-Golomb read is internally bounded.

Computed declarations join the existing declaration-order typed-field stream.
They inherit enclosing conditional, switch, and repeat guards, count toward the
99,999-field structure projection limit, and append repeat indexes to their
materialized names. A repeat-local computed value is visible only to later
items in the same iteration. It consumes zero source bits and does not change
static alignment.

Controller validation is extended so an earlier `computed<u64>` may control the
existing equality conditional, switch, or bounded repeat forms. Conditional and
switch literals use the complete `u64` range. An earlier `computed<bool>` may
control the new restricted `if (flag) { ... } else { ... }` form, with the
`else` remaining optional as in the existing conditional form. It lowers to
equality with `true`; it is not a switch or repeat controller. The existing
literal maximum still bounds a repeat whose controller is computed, and the
normal guarded repeat assertion rejects a reached value above that maximum
before body input. That failure is attached to the computed controller field
path with no `FieldLocation`; source-backed controllers retain their exact
source location under ADR-0022.

Each computed field emits one `evaluate-computed` instruction. Its typed
expression references only previous typed-field indexes and is validated before
execution. A false presence guard skips evaluation and node creation while the
instruction still counts toward the instruction budget and remains a
cancellation point. A successful evaluation creates one
`AnalysisNodeKind::ComputedField` with a `bool` or unsigned 64-bit value,
metadata, and no `FieldLocation`; it consumes one materialized-node slot. A
failed evaluation creates no computed node, retains earlier nodes, marks the
structure invalid, and attaches a no-location diagnostic to the computed field
path. Malformed expression indexes, types, dependencies, guards, or opcode
placement are invalid typed definitions. Subexpression evaluation is charged as
part of the one instruction and adds no cancellation point beyond that
instruction boundary; the 256-node and depth-64 limits bound that internal work.

Malformed `pure`, parameter, `return`, call, expression, or `computed<...>`
syntax produces source-ranged diagnostics. Statement recovery stops at the next
semicolon or enclosing closing brace, while expression recovery also recognizes
the current call's comma or right parenthesis. The format-language reference
will enumerate valid and invalid forms before this slice is treated as stable.

## Consequences

Format definitions can express common derived flags and count adjustments while
preserving source traceability, linear bytecode, selected-only materialization,
and the existing partial-result and cancellation model. Pure functions provide
reuse without adding a runtime call stack because their bodies are statically
inlined and bounded.

Large helper expansions are rejected even if a particular runtime path would
short-circuit them. Computed fields consume static projection, instruction, and
node capacity despite consuming no source bits. Runtime arithmetic diagnostics
have a field path but no source location because assigning one would violate the
computed-field model.

This slice does not accept signed scalar expressions, references to `se`, casts,
bitwise or shift operators, a ternary operator, computed arrays, local variables,
function statements beyond one return expression, recursion, forward calls,
runtime function values, source-location introspection, or arbitrary native or
host helpers. General expressions directly in array lengths, case labels,
repeat maxima, and repeat counts remain separate decisions; a computed `u64`
identifier is the only new bridge into existing control forms.

This slice deprecates no accepted 0.1 syntax. Existing syntax-field reads,
arrays, equality conditionals, switches, and bounded repeats keep their behavior
except that earlier computed scalar identifiers become valid controllers only
in the forms listed above.

## Considered Options

- A general expression VM with runtime calls: extensible, but it introduces call
  frames, recursion validation, more cancellation accounting, and a much larger
  malformed-bytecode surface before the format language needs them.
- Built-in helpers only: keeps implementation small, but makes ordinary
  format-specific predicates depend on an expanding engine-owned catalog.
- Computed fields without reusable functions: enough for one-off values, but
  duplicates common specification predicates and leaves the planned pure-helper
  feature unresolved.
