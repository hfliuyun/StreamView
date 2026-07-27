# Lower Equality Switches To Guard Conjunctions

Status: Accepted
Date: 2026-07-27

## Context

Equality conditionals already lower structured alternatives to linear,
declaration-order typed fields with presence guards. The next DSL slice needs
multi-way selection without adding jump bytecode or an expression evaluator,
and must preserve the existing field-index, validation, and budget invariants.

## Decision

The first switch slice will accept nested C-style
`switch (previous_field) { case integer: { ... } default: { ... } }` blocks.
The controller has the same requirements as an equality conditional: it must
name an earlier scalar `bits` or enum field guaranteed to be present on every
path reaching the switch. A switch requires at least one `case`; each case has
one distinct unsigned integer literal that fits the controller width.
`default` is optional, may appear once, and must be the final arm. Fallthrough,
`break`, multiple labels for one body, ranges, enum member names, and general
expressions are not part of this slice.

Each case body lowers to fields guarded by equality with its case value. A
default body lowers to fields carrying the conjunction of all case equalities
negated. Existing outer guards are retained, so nested switches and equality
conditionals compose without new opcodes. All arms are statically validated,
field names remain unique across the complete structure, and all possible arm
fields count toward the structure expansion limit.

Static alignment is analyzed independently from the same incoming offset for
every arm. A switch exit retains a known offset only when every arm ends at the
same known offset. When `default` is omitted, the unmatched empty path also
participates in that merge.

## Consequences

Switches retain deterministic linear bytecode and the VM's existing selected-
only source and node behavior. Skipped arm instructions still count toward the
instruction budget, while skipped fields do not consume source bits or node
budget. A default arm with many cases carries one negated guard per case,
increasing typed-IR size but keeping the semantics explicit and locally
validatable.

Multiple labels sharing one body are deferred because the current field guard
list is a conjunction and cannot represent the required disjunction without a
new condition form or control-flow bytecode.

## Considered Options

- Jump-table or branch opcodes: more compact for large switches, but they would
  expand bytecode control-flow validation before the expression language and
  general statement runtime exist.
- Lowering a switch to nested `if`/`else`: case arms are representable, but a
  multi-case default still needs a conjunction of negated comparisons and the
  source AST would lose switch-specific duplicate/default diagnostics.
- Multiple case labels per body: familiar C syntax, but it requires OR guards;
  deferring it keeps this slice exact rather than giving labels accidental AND
  semantics.
