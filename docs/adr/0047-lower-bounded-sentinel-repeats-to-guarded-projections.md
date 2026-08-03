# Lower Bounded Sentinel Repeats to Guarded Projections

Status: Accepted
Date: 2026-08-04

## Context

The bounded H.264 slice header needs to decode lists such as reference-picture
list modifications and memory-management control operations. Each list reads at
least one item, includes the terminating operation in the decoded syntax, and
continues until one unsigned field in the completed item equals a sentinel.

The existing count-controlled `repeat (count, maximum)` cannot express this
shape. General `while`, `break`, EOF termination, or mutable loop state would
also weaken the DSL's static projection, deterministic bytecode, and bounded
materialization model. The required syntax can stay much narrower.

## Decision

Add one post-tested sentinel form:

```cpp
repeat (64) {
    ue modification_of_pic_nums_idc;
    if (modification_of_pic_nums_idc == 0) {
        ue abs_diff_pic_num_minus1;
    }
} until (modification_of_pic_nums_idc == 3);
```

The positive literal in the `repeat` header is the maximum number of completed
iterations and is limited to `1..64`. The `until` clause is mandatory, follows
the body, and accepts only `sentinel_field == integer`. The sentinel field must
be an unconditional, top-level, non-array source scalar declared directly in
that body. The first slice accepts fixed-width `bits`, enum, and `ue`; it does
not accept `se`, computed fields, lazy regions, dynamic-width fields, fields in
nested control flow, or fields declared outside the body. The integer must fit
the fixed-width field or the supported `ue` domain.

Equality conditions and switches also accept a previously decoded scalar `ue`
controller in this slice. This is required for fields selected by an operation
code inside the sentinel body and compares the already decoded unsigned value;
it does not add a general condition expression.

The body always executes at least once. Each selected iteration completes
before its sentinel is tested, so fields after the sentinel still follow their
ordinary guards. The terminating sentinel field remains materialized. When an
iteration terminates, every later projection is skipped without source access,
node creation, expression evaluation, or constraint checks. There is no
`break`, `continue`, alternate comparison, expression sentinel, or EOF form.

The compiler validates the body once, counts `body projection * maximum`
against the existing 99,999-field limit, and projects it exactly `maximum`
times with the existing outer-to-inner `[index]` names. Iteration zero inherits
the enclosing guards. Each later iteration additionally requires every earlier
projected sentinel to differ from the terminating value. Repeat-local names do
not escape the body. Static alignment after the statement is unknown.

Typed IR records the ordered projected sentinel field indexes, termination
value, enclosing guards, assertion position, and source range. The compiler
emits one `assert-sentinel-terminated` instruction after all projected fields.
The VM validates the complete descriptor and projection-guard shape before
reading source. Malformed field indexes, types, conditions, values, ordering,
assertion positions, or more than 64 projected sentinels are invalid runtime
definitions.

At runtime the assertion succeeds when one selected sentinel equals the
termination value. If all `maximum` iterations complete without it, execution
returns `invalid-syntax`, located at the final sentinel field, while preserving
the materialized bounded prefix and consumed bits. Truncation and source errors
remain transactional at the current field. Instruction, node, cancellation,
mapping, and partial-result behavior otherwise follow existing projected
fields.

The bundled H.264 rule does not use this syntax in this slice, so its package
version and coverage token do not change. Slice dispatch and the final
compressed remaining-bit payload stay behind later decisions.

## Consequences

Rules gain the exact bounded post-tested shape required by H.264 operation
lists without adding a runtime loop or mutable control-flow VM. The generated
program remains linear and every possible field, guard, instruction, and node
is bounded before execution.

The maximum of 64 is intentionally language-wide for this first form. A future
format that needs a larger bound requires a new decision rather than silently
increasing compiler memory and runtime work.

## Non-goals

This decision does not add pre-tested loops, count expressions, general
conditions, `break` or `continue`, EOF or remaining-bit termination, collection
values, loop indexes in expressions, imported context values in loop bounds, a
compressed-payload terminal, or H.264 slice dispatch.
