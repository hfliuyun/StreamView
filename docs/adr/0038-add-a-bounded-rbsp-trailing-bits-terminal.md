# Add A Bounded RBSP Trailing-Bits Terminal

Status: Accepted
Date: 2026-08-01

## Context

ADR-0037 dispatches selected NAL payloads to structures over a mapped RBSP
view. It requires a selected structure to consume that view exactly. Its first
access-unit delimiter rule represented `rbsp_trailing_bits()` with a fixed
`rbsp_stop_one_bit` field and four `rbsp_alignment_zero_bit` array elements.
That declaration is correct only because its preceding `primary_pic_type` is
three bits wide.

The H.264 sequence parameter set is the next formal-format slice. It ends in
`rbsp_trailing_bits()` after a variable number of syntax elements. The count
of zero alignment bits is determined by the current RBSP bit position, so the
stable DSL's fixed arrays, bounded repeats, and expressions cannot express it
without putting H.264 parsing knowledge into the analyzer or accepting a
partially consumed RBSP. Neither is acceptable under the implementation plan
or ADR-0037.

## Decision

The DSL adds an unannotated terminal structure item:

```cpp
rbsp_trailing_bits;
```

It is a contextual identifier, not a reserved word. It is valid exactly once
in a structure, only at that structure's top level, and it must be the final
item. It cannot occur inside a conditional, switch, or repeat body. A
structure containing only this item is non-empty.

The item reads and publishes one named `rbsp_stop_one_bit` constrained to `1`,
followed by zero through seven individually named
`rbsp_alignment_zero_bit[i]` fields constrained to `0`, stopping at the next
logical-byte boundary. The stop bit and each consumed alignment bit retain
their own source location, including multiple source spans across an
emulation-prevention exclusion. Missing bits are `truncated-source`; a zero
stop bit or a nonzero alignment bit is `invalid-syntax` at that field. As with
every structure, exact payload consumption remains the runner's responsibility
and catches any residual RBSP bits.

The compiler reserves eight typed field slots whenever the terminal is used:
one stop bit and seven possible alignment bits. The bytecode contains one
`read-rbsp-trailing-bits` instruction. At execution, unused alignment slots
are recorded as skipped so that the normal end-of-structure accounting remains
strict. This establishes a fixed upper bound of eight reads, eight generated
field nodes, and one VM instruction/cancellation point. The reservation counts
against the existing 99,999 fields per structure and 100,000 materialized-node
limit, preserving those sandbox contracts even when the maximum padding is
read.

The bundled H.264 access-unit delimiter rule migrates to the terminal. This
replaces the fixed array endorsed in ADR-0037; all other payload-dispatch
decisions in that ADR remain in force. The language compatibility string stays
at `0.1` because this is an additive implementation of a bounded v0.1 syntax
element. The official H.264 package makes its own patch version change because
its shipped rule source changes.

## Consequences

Formal H.264 rules can state `rbsp_trailing_bits()` faithfully without a
general expression evaluator, dynamic unbounded loop, analyzer special case,
or opaque suffix. Its maximum resource use is independent of input size and
visible in typed IR before execution.

The generated fields are deliberately visible rather than represented by one
aggregate node. That preserves the per-syntax-element source inspection and
diagnostic behaviour already established by the AUD rule. Unused possible
alignment fields do not appear in a materialized tree.

The terminal cannot be composed or followed by another item. Future formats
that need another position-dependent primitive require a separate bounded DSL
decision rather than silently expanding this one.

## Considered Options

- Keep declaring fixed arrays in each rule: exact only where the prior bit
  count is constant, so it cannot describe SPS and invites incorrect copies.
- Parse trailing bits in the Annex B analyzer: would move formal format
  knowledge out of the official rule and bypass compiler/VM budgets.
- Permit an opaque suffix after a parsed prefix: violates the exact-consumption
  contract that makes a dispatched payload a verified claim.
- Add a general alignment expression and repeat: a much wider language feature
  with larger static and runtime validation surface than H.264 needs here.
