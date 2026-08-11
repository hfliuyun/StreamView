# Observe Remaining RBSP Data in Expressions

Status: Accepted
Date: 2026-08-11

## Context

H.264 picture parameter set extensions are optional even when the referenced
sequence parameter set uses the High profile. Clause 7.3.2.2 therefore guards
the extension with `more_rbsp_data()`: profile selection alone cannot
distinguish a legal base-only High-profile PPS from one that carries extension
syntax.

The DSL already has a terminal `rbsp_trailing_bits;` item, but it can occur only
once as the final unconditional top-level item. It has no expression that can
observe whether the current RBSP cursor has reached that terminal pattern.
Adding a remaining-bit count or general source lookahead would expose more
source state than this format decision needs and would enlarge the language's
control surface.

## Decision

Add the reserved zero-argument expression leaf `more_rbsp_data()`. It returns
`bool` and is accepted only in structure execution expressions, including
computed-field initializers and assertion conditions. It is rejected in a pure
function body because its result depends on the current reader cursor. A pure
function declaration may not use the reserved name.

Evaluation does not advance the current reader. With no remaining logical bits
the result is false. With more than eight remaining bits the result is true,
because an H.264 `rbsp_trailing_bits()` pattern occupies one stop bit followed
by at most seven alignment zero bits. With one through eight remaining bits,
the VM probes a copy of the reader and returns false exactly when the complete
remainder is `1` followed only by zero bits; every other pattern returns true.
A probe read failure preserves the original cursor and propagates through the
existing truncated-source or source-error status.

The parser validates the zero arity and Boolean type before execution. The
compiler lowers the leaf to a zero-operand `MoreRbspData` typed-expression node.
The VM validates that descriptor and evaluates it inside the enclosing
expression instruction. The feature adds no opcode and no presentation node
beyond a computed field that the rule explicitly declares.

## Consequences

Rules can distinguish optional RBSP syntax from `rbsp_trailing_bits` without a
profile guess, an unbounded scan, or a consuming peek. The probe is bounded to
one read of at most eight bits and remains covered by the enclosing expression's
existing instruction, node, depth, and cancellation contracts.

The High-profile PPS extension is the first consumer, but it remains a separate
rule increment with its own accepted syntax boundary and fixtures.

## Non-goals

This decision does not add an EOF predicate, a remaining-bit count, arbitrary
lookahead, a general alignment expression, source-state pure functions, or a
repeat termination form. It does not move, duplicate, or make conditional the
terminal `rbsp_trailing_bits;` item.
