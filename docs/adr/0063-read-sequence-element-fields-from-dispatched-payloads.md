# Read Sequence Element Fields From Dispatched Payloads

Status: Accepted
Date: 2026-08-08

## Context

ADR-0062 completed the bounded reference-list modification syntax. The next
bundled-profile increment is `dec_ref_pic_marking()`, which clause 7.3.3 reads
whenever `nal_ref_idc` is nonzero. That field belongs to `NalUnitHeader`, not to
the slice structure, and a dispatched payload structure currently has no way to
read it.

Three existing mechanisms were examined and none can express the dependency.

The payload request carries no header values. `RuleExecutionRequest` holds a
source, structure index, mapping, tree, parent, enclosing span, and options; the
virtual machine then builds a fresh environment sized to the executing structure
alone, so every slot starts unset.

The context mechanism resolves by source position and would select the wrong
generation. A payload lookup queries at the enclosing NAL unit's start, while a
definition only becomes selectable at its exclusive end, so a slice importing a
header-published generation would receive the *previous* NAL unit's header.
Context kinds are also a closed set of four parameter-set kinds.

The dispatch controller is not visible either. It is resolved against the
sequence element structure and consumed by the runner to select a case; it never
enters the payload structure's environment, and the dispatch deliberately adds
no opcode.

ADR-0037 rejected placing the dispatch inside `NalUnitHeader` partly because it
"would also conflate the header's own fields with the payload's". That reasoning
still holds: the fix must not merge the two namespaces.

## Decision

Add a restricted expression leaf `header_value(field_name)` that reads a field
of the sequence element structure from within a dispatched payload structure. It
deliberately mirrors the existing `context_value(...)` leaf rather than
introducing a second style of external reference.

```cpp
if (header_value(nal_ref_idc) != 0) {
    bits<1> adaptive_ref_pic_marking_mode_flag;
}
```

The grammar takes exactly one identifier argument. A `header_value` call with a
different argument count, or with any non-identifier argument, is a static
error, matching the existing arity check on `context_value`.

The referenced field resolves at compile time against the program's sequence
element structure. The language admits at most one top-level payload dispatch
and one scan, so the element structure is unambiguous program-wide; resolution
does not depend on knowing whether the enclosing structure is itself a case
target, which the compiler has not yet established while compiling a structure
body. The named field must exist in that structure and must be an unconditional,
top-level, non-array unsigned scalar, so the reachable surface is exactly the
surface already required of a context key or a dispatch controller. A guarded,
repeated, array, dynamic-width, or signed element field is rejected.

A program that declares no scan, or whose enclosing structure *is* the sequence
element structure, is a static error. The second case would otherwise let a
structure read itself through an external channel while its own field is still
unset.

The leaf is `u64`, is admitted only where the imported-context leaf is already
admitted, and counts against the same expression node and depth budgets. It is
not admitted in a pure-function body, a lazy byte count, or any other position
that already rejects `context_value`.

Typed IR gains a `SequenceElementReference` expression kind carrying the
resolved element field index. The compiler emits no new opcode, exactly as the
imported-context leaf reuses the existing expression evaluation.

At runtime the analyzer supplies the already-decoded element field values on the
execution request, and the virtual machine reads the resolved index from that
vector. The values are published before the payload runs, because the runner
already materializes the header and reads the controller out of it to select a
case. The virtual machine validates the descriptor before it reads source: an
out-of-range index, a missing value, or an absent value vector fails as an
invalid definition rather than decoding with a guessed value.

The header and payload namespaces stay separate. `header_value` is a call, not
an identifier, so no element field name can shadow or be shadowed by a payload
field name, and a rule author reading a payload structure can see every external
dependency syntactically.

Regression coverage spans the parser arity and identifier rules, the static
element-field constraints, the self-reference and missing-scan rejections, typed
IR lowering with no new opcode, VM preflight of malformed descriptors, a false
guard that consumes no source, and an end-to-end session case in which a
dispatched payload branches on a header field.

## Consequences

A payload structure can now depend on its own NAL unit's header, which unblocks
`dec_ref_pic_marking()` and any later syntax whose presence is a function of the
header rather than a parameter set. The bundled rule still needs a separate
increment to use it; this decision only adds the capability.

The mechanism is narrower than a general cross-structure read. It reaches only
the sequence element structure, only unconditional top-level unsigned scalars,
and only from a dispatched payload. A future need to read some other structure's
fields will require its own decision rather than a widening of this one.

Because the leaf resolves against the element structure program-wide, a
`header_value` call inside a structure that is never dispatched as a payload
compiles successfully but fails at execution with an invalid definition when no
value vector is supplied. The alternative, deferring resolution until the
dispatch is known, would require a second compilation pass over structure bodies
and was rejected as disproportionate.

## Non-goals

This decision does not add general cross-structure field access, does not let a
payload read another payload's fields, does not expose the dispatch controller as
an identifier, does not add a view or call opcode, does not change payload
dispatch selection, does not widen the closed set of context kinds, and does not
change context generation resolution. It does not add
`dec_ref_pic_marking()`, lift the type-1 `nal_ref_idc == 0` prerequisite, or
change any bundled rule syntax; those follow in the next increment.
