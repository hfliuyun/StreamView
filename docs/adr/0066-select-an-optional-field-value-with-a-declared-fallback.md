# Select An Optional Field Value With A Declared Fallback

Status: Accepted
Date: 2026-08-08

## Context

ADR-0065 admitted the reserved external leaves in computed initializers and
recorded that `pred_weight_table()` still could not land. The remaining blocker
is count selection.

Clause 7.3.3.2 iterates `num_ref_idx_l0_active_minus1 + 1` times. Clause 7.4.3
defines that value as the slice-level override when
`num_ref_idx_active_override_flag` is set, and otherwise the PPS default
`num_ref_idx_l0_default_active_minus1`. The override field is therefore
declared inside `if (num_ref_idx_active_override_flag == 1)`, which is itself
inside `if (uses_reference_lists)`, while the default arrives through the
already-declared `h264-pps` import.

A probe reduced the blocker to its minimal shape:

```
bits<1> override_flag;
if (override_flag == 1) {
    bits<4> override_count_minus1;
}
computed<u64> effective_count = override_count_minus1 + 1;
```

The compiler rejects the computed field with "Computed field dependency is not
guaranteed on the current branch". That rule is correct and load-bearing: a
plain field reference must never read a value the current path did not
materialize. What the language lacked was any way to say "this field may be
absent here, and this is the value to use when it is".

The duplication alternative was probed as well, and it compiles:

```
if (override_flag == 1) {
    bits<4> override_count_minus1;
    computed<u64> effective_count_override = override_count_minus1 + 1;
    repeat (effective_count_override, 16) { bits<1> weight_flag_override; }
}
if (override_flag == 0) {
    computed<u64> effective_count_default = 3;
    repeat (effective_count_default, 16) { bits<1> weight_flag_default; }
}
```

Because the flat field namespace forbids redeclaring a name across mutually
exclusive branches, every field inside the duplicated body needs a distinct
name. For `pred_weight_table()` that means the list 0 body appears under both
the override and default branches, the list 1 body appears under both again
inside `if (is_b_slice)`, and clause 7.3.3.2's own names — `luma_weight_l0`,
`chroma_offset_l0` — never appear in the rule at all. The cost is four
near-identical bodies whose field names encode an unrelated flag, and it
compounds on every later table with the same defaulting shape.

Three facts about the existing runtime made the alternative approach cheap, and
each was confirmed in the source rather than assumed.

Field presence is already modelled exactly. The interpreter holds
`std::vector<std::optional<quint64>>` field values, initialized empty, and a
guard is a per-field condition chain rather than a jump: when a guard is false
the field is skipped and its slot stays `nullopt`. A taken branch's value
persists for the rest of the structure execution, and the only `.reset()` calls
are for lazy and compressed regions, which carry no scalar value.

An absent field is currently a hard failure by construction. Evaluating a
`FieldReference` whose slot is `nullopt` reports "Computed expression dependency
is unavailable", which is unreachable today precisely because the static rule
promises it cannot happen.

Repeat bodies are scoped. Each iteration resizes the declared-field list back to
its entry length, so a repeat-body field is out of scope after the repeat and is
still rejected as "must be declared earlier". A conditional body is not scoped,
which is exactly why the override field is nameable yet rejected.

So the restriction is purely static. The runtime already knows whether a field
was materialized.

## Decision

Add a third reserved leaf form, `optional_value(field_identifier,
fallback_expression)`, of type `u64`.

The first argument must be an identifier naming a field declared earlier in the
same structure. It is exempt from the branch-guarantee rule, and from that rule
only. Every other dependency restriction continues to apply: the field must be
declared earlier, must carry a typed index, must be a scalar unsigned `bits`,
enum, `ue`, or `computed<u64>`, and arrays and `se` fields are rejected. An
out-of-scope repeat-body field remains rejected as undeclared, so a projected
per-iteration index can never be aliased.

The second argument is a full `u64` expression and is subject to every rule
including branch-guarantee, so the fallback itself can only read values the
current path guarantees. Nesting composes: the fallback may be another
`optional_value(...)`.

The form is admitted wherever a field-dependent expression is already
evaluated — a computed field initializer, an assertion condition, a dynamic
`bits` width, and a lazy byte count. It is rejected in a pure-function body,
which resolves parameters rather than fields, and in every fixed-shape position:
the imported equality conditional, switch and repeat controllers, array lengths,
case labels, and annotations.

The typed IR gains one expression kind, `OptionalFieldReference`, carrying the
resolved field index and the lowered fallback as its single operand. No new
struct member and no new opcode are required. At runtime the leaf yields the
field value when the slot holds one and evaluates the fallback when it does not,
reusing the presence model already described. A plain `FieldReference` to an
absent slot remains a hard failure; only this kind treats absence as meaningful.

The leaf counts as one expression node plus its fallback subtree and is subject
to the existing node, depth, and shared expansion-work limits unchanged.

## Consequences

`pred_weight_table()` becomes expressible with clause 7.3.3.2's own field names:

```
computed<u64> effective_l0_count =
    optional_value(num_ref_idx_l0_active_minus1,
                   context_value(pic_parameter_set_id,
                                 h264_pps,
                                 num_ref_idx_l0_default_active_minus1)) + 1;
```

This reads as the clause reads, and it is the first construct in the language
that expresses a syntax element's inferred default. Several later H.264
elements share that shape, so the cost is paid once.

The compiler still catches the mistake this form exists to handle. Writing a
plain reference to a branch-local field keeps failing with the branch-guarantee
diagnostic, so absence must be acknowledged deliberately rather than by
omission.

The first argument is not required to be branch-local. Requiring it would make
validity depend on unrelated guards elsewhere in the structure, so that adding a
conditional around an existing field would retroactively invalidate a distant
`optional_value` that had been correct. A redundant `optional_value` naming an
always-present field is accepted and simply never takes its fallback.

This increment changes no bundled rule. The capability lands with its own tests
and documentation, matching the ADR-0063 and ADR-0065 precedent, and the
bounded `pred_weight_table()` that consumes it follows as the next increment
with its own analyzer coverage.

## Non-goals

A Boolean form is not part of this decision. Both existing reserved leaves are
`u64`, the H.264 need is `u64`, and a narrow first cut is easier to widen later
than to narrow. `optional_value` naming a `computed<bool>` is a type error.

No general conditional or ternary expression is introduced. That alternative
would need flow-sensitive analysis proving each arm's dependencies against the
guard implied by the condition, plus new grammar and precedence, to reach the
same result. This form needs no proof because it declares the absence handling
explicitly, and it works at any nesting depth without the guard's controlling
field having to be in scope.

Presence is not exposed as a Boolean. There is no `has_value(field)`; the only
way to observe absence is to supply the value that replaces it.
