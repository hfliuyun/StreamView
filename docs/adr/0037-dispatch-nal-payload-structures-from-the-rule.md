# Dispatch NAL Payload Structures From The Rule

Status: Accepted
Date: 2026-08-01

## Context

ADR-0025 derives a bounded RBSP logical view from each non-empty NAL unit
payload, and the Annex B runner publishes it as an uninterpreted `rbsp_payload`
region. Nothing parses inside that region. Every accepted DSL declaration so
far describes one structure over one view, and the entry sequence binds a
single element structure to the start-code scan. There is no way to say that
the syntax carried by a NAL unit depends on the `nal_unit_type` value that the
NAL unit header just produced.

The implementation plan requires formal formats to be implemented only through
the DSL. A dispatch table in the analysis core would put "type 9 is an access
unit delimiter" into C++, which is exactly the knowledge the rule is supposed
to own. The format-language reference already defers typed format declarations
and payload access to the official format rules.

The first payload structures are also the smallest ones. Clause 7.3.2.4 defines
`access_unit_delimiter_rbsp` as `primary_pic_type` followed by
`rbsp_trailing_bits`, and clauses 7.3.2.5 and 7.3.2.6 define `end_of_seq_rbsp`
and `end_of_stream_rbsp` as empty. They therefore exercise payload dispatch,
exact consumption, and empty-payload conformance without also requiring
parameter-set state, scaling lists, or a general expression solver.

Two existing behaviours block them. A structure must declare at least one
field, so an empty RBSP is not expressible as a structure. The derived RBSP
view is created only when the payload is non-empty, so a header-only NAL unit
is materialized without ever consulting a rule.

## Decision

The DSL adds one top-level payload-dispatch declaration:

```cpp
@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}
```

`payload` and `empty` are contextual identifiers, not reserved words. The
declaration names the view kind it decodes, the progressive sequence it
extends, the controller field it switches on, and one arm per handled
controller value. Annotations may precede it and are carried as the dispatch's
own metadata.

The only accepted view kind is `rbsp`, the mapped payload view that ADR-0025
already derives. A program may declare at most one payload dispatch.

### Static Rules

The named sequence must be a declared progressive scan. The controller must
name a field of that scan's element structure. It must be an unsigned scalar
`bits` field declared unconditionally at the top level of that structure, so it
is guaranteed present on every path, and it must be at most 64 bits wide. A
computed field, an Exp-Golomb field, an array element, a lazy region, and any
field inside a conditional, switch, or repeat body are all rejected as
controllers. This mirrors the controller rules already used by equality
conditionals, switches, and bounded repeats.

Case values must be distinct and representable in the controller's declared
width. Each `case` target must be either a declared structure or `empty`. A
`case` target structure must not be the scan element structure, because the VM
still has no call stack and no view-nesting opcode.

There is no `default` arm. An unlisted controller value keeps the existing
uninterpreted payload behaviour. A rule therefore never claims knowledge of a
NAL type it does not describe, and adding a new type is an additive change.

A payload dispatch requires an entry naming the same sequence.

### Typed IR

A payload dispatch lowers to a declaration-order list of cases holding the
controller value and either a target structure index or the empty marker. It
adds no opcode. The selected structure is executed by the existing
`begin-structure` through `end-structure` bytecode that the compiler already
emits for every declared structure. Validation of the controller index, case
distinctness, and target indexes happens before execution, and a malformed
dispatch is an invalid typed definition.

### Runtime

After the NAL unit header structure materializes, the runner reads the
controller value from the published header node and looks it up in the
dispatch.

When no case matches, behaviour is unchanged. A non-empty payload becomes an
uninterpreted `rbsp_payload` region and a header-only NAL unit gets no payload
node at all.

When a case matches, the derived RBSP view is always created, including for a
zero-length payload. Presence of a dispatch case, not payload length, decides
whether the view exists. This is the observable behaviour change: a rule that
describes a NAL type gets an exact, possibly empty, view to decode.

An `empty` case requires the mapped RBSP logical length to be exactly zero. A
non-empty payload is `invalid-syntax` reported at the payload path, and the
complete `rbsp_payload` region and excluded regions are retained.

A structure case executes the selected structure over the mapped RBSP view from
logical zero, parented under the `rbsp_payload` region node, using the same
execution options, sandbox budgets, and cancellation token as the header.

A materialized structure must consume the complete RBSP logical length.
Residual bits are `invalid-syntax` at the payload path. Exact consumption is
what makes `rbsp_trailing_bits` verifiable and what stops the runner from
silently accepting trailing bytes that no declaration describes.

A structure that requires more bits than the view holds is `truncated-source`,
including when the view is empty. A header-only access unit delimiter is
therefore truncated, while a header-only end of sequence is materialized.

Payload failure marks the NAL unit invalid or cancelled and retains the header,
the `rbsp_payload` region, the excluded emulation-prevention regions, and the
Annex B trailing zeros. It never terminates the scan, and the following NAL
units continue to be analysed.

Extension-header NAL types 14, 20, and 21 still have no derived RBSP view under
ADR-0025, so they cannot dispatch.

### Rule Asset

The bundled H.264 rule gains the access unit delimiter structure and the
dispatch above:

```cpp
@spec("ITU-T H.264", "7.3.2.4")
struct AccessUnitDelimiterRbsp {
    bits<3> primary_pic_type;
    bits<1> rbsp_stop_one_bit @equals(1);
    bits<1> rbsp_alignment_zero_bit[4] @equals(0);
}
```

The trailing bits are declared as a fixed four-element array rather than one
four-bit field. Each element is one `f(1)` syntax element in clause 7.3.2.11,
and the array form keeps a per-bit source span and a per-bit constraint
diagnostic. The count is static because `primary_pic_type` is three bits and
the stop bit is one, so exactly four alignment bits reach the byte boundary.

## Consequences

Format knowledge stays in the rule. The runner learns which structure to
execute by reading the compiled rule, not by consulting a table in C++. SEI,
sequence parameter sets, and picture parameter sets reuse the same declaration
without further runner changes.

Exact consumption turns the payload region from a container into a checked
claim. A NAL unit whose declared syntax does not account for every RBSP bit is
now reported instead of silently accepted.

Creating the view for a zero-length payload changes the published tree for
dispatched types only. Undescribed types keep the previous shape, so existing
sessions and cached materialized pages for those types stay comparable.

The absence of a `default` arm means an unknown NAL type is never reinterpreted
as something the rule happens to describe. It also means a rule cannot yet
express "everything else is opaque" other than by omission, which is the
intended default.

The declaration is deliberately top-level and single. It does not compose,
nest, or dispatch on computed values, and it cannot appear inside a structure.
Those are separate decisions that will need the call and view opcodes the VM
has reserved but not implemented.

## Considered Options

- Keep a dispatch table in the Annex B runner: smallest change, but it places
  "type 9 is an access unit delimiter" in the analysis core and contradicts the
  requirement that formal formats be implemented only through the DSL.
- Allow empty structures and give each empty RBSP its own structure: this
  relaxes a whole-language restriction to express two payloads that genuinely
  contain no syntax elements. `empty` states that intent at the one place it
  applies and leaves the structure rule intact.
- Add a `default` arm: it would force a rule to describe every controller value
  and would silently reinterpret NAL types the rule does not know.
- Express the dispatch as a `switch` inside `NalUnitHeader`: the payload is a
  different logical view, and the current VM has no view or call opcode. It
  would also conflate the header's own fields with the payload's.
- Accept partial consumption: `rbsp_trailing_bits` would become unverifiable
  and trailing bytes that no declaration describes would pass as conforming.
- Declare `rbsp_trailing_bits` as a shared structure: it needs a
  position-dependent alignment loop, which the current expression and repeat
  grammar cannot express. The fixed four-element array is exact for this
  payload and does not pretend to be the general form.
