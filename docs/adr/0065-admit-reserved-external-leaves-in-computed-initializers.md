# Admit Reserved External Leaves In Computed Initializers

Status: Accepted
Date: 2026-08-08

## Context

ADR-0064 completed bounded reference-picture marking. The next bundled-profile
increment is `pred_weight_table()`, which clause 7.3.3 reads when a P or SP
slice uses a PPS with `weighted_pred_flag` set, or when a B slice uses a PPS
with `weighted_bipred_idc == 1`.

A first draft of this decision proposed adding the table immediately, narrowing
the accepted profile with a source-anchored assertion so that the entry count
would always be the imported PPS default. Probing the compiler disproved that
draft, and the probe results are recorded here because they constrain every
later attempt.

Four language limits were confirmed against the bundled rule.

An imported-context leaf is not admitted in a computed initializer. The parser
validates a computed expression with external references disabled, so
`context_value(...)` there is reported as an undeclared pure function rather
than resolved.

A computed field cannot reference a declaration made inside a conditional. The
compiler reports that the dependency "is not guaranteed on the current branch",
which is the rule that forced the draft toward narrowing in the first place.

The narrowing assertion itself has no legal position. At top level it fails the
same branch-guarantee rule, because `num_ref_idx_active_override_flag` is
declared inside `if (uses_reference_lists)`. Moved into that conditional, where
the flag is available, it fails because "assertions must be unconditional
top-level items".

A fixed two-iteration repeat is not expressible. The single-argument
`repeat (integer)` form is always the sentinel form and requires an `until`
clause, so the chroma pair must be named explicitly rather than projected as a
two-element loop.

Two capabilities the draft assumed were confirmed present. A `computed<u64>`
carrying arithmetic is accepted as a count-repeat controller, and a
`computed<bool>` declared inside a conditional can guard a later conditional in
the same branch.

The conclusion is that `pred_weight_table()` needs the effective entry count,
the count is either a locally overridden field or the imported PPS default, the
language has no expression that selects between them, and the restriction that
would avoid the choice cannot be written. The table therefore cannot land in
this increment. What every candidate design does need is the ability to read an
external value inside a computed initializer: the table's presence guard is
exactly the spec's condition on two imported PPS fields, and the `if` grammar
accepts only a computed boolean identifier, a field equality, or a
`context_value(...)` equality, so the two-clause guard has nowhere else to live.

## Decision

Admit the two reserved external leaves, `context_value(...)` and
`header_value(...)`, in computed field initializers. This adds a position to an
existing mechanism; it introduces no new syntax, no new expression kind, and no
new opcode.

```cpp
computed<bool> uses_explicit_weighting =
    (is_p_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 1) ||
    (is_b_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) == 1);

computed<u64> effective_l0_count =
    context_value(pic_parameter_set_id,
                  h264_pps,
                  num_ref_idx_l0_default_active_minus1) + 1;

computed<bool> is_reference_picture = header_value(nal_ref_idc) != 0;
```

Both leaves are admitted together rather than one at a time. They are the same
mechanism reaching two different external sources, they are enabled and
disabled at the same three existing sites, and admitting one while rejecting the
other would be an arbitrary asymmetry for a rule author to remember.

Every constraint already attached to each leaf continues to apply unchanged. An
imported-context leaf still requires the enclosing structure to declare a
matching `@context_import`, still resolves its key to an earlier
context-eligible field, and still reaches only an exported field of a single
publishing structure. A sequence-element leaf still resolves against the
program's sole scan element structure, still rejects a self-reference or a
missing scan, and still reaches only an unconditional, top-level, non-array
unsigned scalar. A computed initializer in a structure that declares no import
is rejected exactly as the dynamic-width position already rejects it.

The leaves stay `u64` and count against the same expression node and depth
budgets. They remain absent from a pure-function body and a lazy byte count.

The runtime needs no change. The virtual machine already passes both the
imported-context resolver and the element value vector when it evaluates a
computed expression, so the evaluation semantics are the ones the assertion and
dynamic-width positions have used since ADR-0063. Only the static gates move:
the parser's computed-expression validation, the typed-IR computed lowering, and
the virtual machine's computed validation each now allow what the surrounding
positions already allowed.

Because a computed field may control a count repeat, this decision also makes an
imported or header-derived repeat count expressible for the first time. That
composition is covered by an end-to-end session case in which a computed count
read from a header field admits exactly `nal_ref_idc + 1` iterations and
consumes exactly the predicted bits.

Regression coverage spans parser acceptance of both leaves in a computed
initializer, typed-IR lowering that produces the existing
`ImportedContextReference` and `SequenceElementReference` kinds with no new
opcode, the missing-import rejection at the compiler stage, and the runtime
composition described above.

## Consequences

A computed field can now depend on a parameter set or on its own NAL unit
header. This unblocks the `pred_weight_table()` presence guard, whose condition
reads two imported PPS fields, and it makes a dependent entry count expressible
in the position that a count repeat requires.

The bundled rule is deliberately unchanged. The intended cleanup was validated
on a scratch copy of the H.264 rule, which compiles cleanly with the two
assertions collapsed into a single `uses_explicit_weighting` guard and the
inverted empty-`then` from ADR-0064 replaced by an `is_reference_picture`
computed. Applying it now would add two materialized computed nodes near the end
of the non-IDR slice structure, shifting child indices in 24 analyzer tests
across 106 hardcoded positions, in exchange for no decoding change. That churn
belongs in the increment that also adds the table, where the same nodes are
required rather than merely tidier.

The count-selection question is still open and is the next decision to make. The
two viable shapes are duplicating the table body under the override and default
branches with distinct field-name suffixes, which is expressible today at the
cost of four near-identical copies and field names that vary with an unrelated
flag, or adding a defaulting expression whose dependency analysis is
flow-sensitive, which keeps the spec's own names at the cost of a further
capability decision. The second is preferable because the duplication compounds
in every later table, but it is a separate decision and is not made here.

Admitting a leaf in one more position widens the surface where a context lookup
can fail at execution rather than compile time. The exposure is the one
ADR-0063 already accepted: a `header_value` call in a structure that is never
dispatched as a payload compiles and then fails as an invalid definition when no
value vector is supplied.

## Non-goals

This decision does not add `pred_weight_table()` or any bundled rule syntax,
does not change the bundled package version, and does not resolve the
count-selection question. It does not add a conditional or defaulting
expression, does not make computed-field dependency analysis flow-sensitive, and
does not let a computed field reference a declaration made inside a conditional.
It does not admit either leaf in a pure-function body or a lazy byte count, does
not relax any existing constraint on either leaf, does not widen the closed set
of context kinds, does not change context generation resolution, does not add
general cross-structure field access, and does not add or change any opcode.
