# Allow Enum Domains On Unsigned Exp-Golomb Fields

Status: Accepted
Date: 2026-08-04

## Context

The bounded H.264 IDR slice header accepts `slice_type == 2`, but H.264 also
defines the equivalent all-I value 7. An equality constraint cannot express
the closed set `{2, 7}`, and encoding that set through arithmetic failure
would obscure the rule's intent and produce a misleading diagnostic.

The DSL already gives fixed-width `bits` fields a named closed domain through
`@enum(Type)`. Unsigned Exp-Golomb fields decode to the same `u64` value model,
have complete source locations, and are valid condition, switch, repeat, and
sentinel controllers. The only additional boundary is that the supported `ue`
domain ends at `2^64 - 2`; `2^64 - 1` has no representable Exp-Golomb codeword.

## Decision

Allow one `@enum(Type)` annotation on a `ue` field. The enum must be declared,
must be non-empty under the existing enum rules, and every member must lie in
`0..2^64 - 2`. A member equal to `2^64 - 1` is a static
`enum-value-out-of-range` definition error. `se @enum(Type)` remains invalid.

Typed IR keeps the field's `UnsignedExpGolomb` kind and stores the enum index
alongside it. This differs deliberately from a fixed-width enum, whose kind
remains `Enum`. Bytecode therefore continues to use
`ReadUnsignedExpGolomb`; no new opcode or encoding is introduced. Metadata uses
the enum type name for either representation.

The VM validates the enum reference and all member values before reading
source. It then decodes and materializes the complete Exp-Golomb codeword. A
value outside the declared enum is a fatal `invalid-syntax` failure at that
field's complete mapped codeword location, matching fixed-width enum behavior.
The field node and decoded value remain available as partial analysis, but no
later field in the structure executes.

`@enum`, `@equals`, and `@range` may coexist on one `ue` field. They inspect the
same materialized value independently in deterministic lowering order: enum
membership first, then equality, then the range bounds. Enum and equality
violations are fatal; a range violation retains the non-fatal warning semantics
from ADR-0040. A fatal earlier check prevents later instructions from
executing. Attaching an enum does not change controller eligibility or the
decoded `u64` used by conditions, switches, bounded repeats, or sentinels.

This is an additive DSL `0.1` change. Existing source and typed programs keep
their behavior.

## Consequences

Rules can state closed domains for variable-length unsigned syntax directly,
with readable names and precise diagnostics. The H.264 rule can accept both
all-I slice type values without weakening its rejection of P, B, SP, or SI
slice headers.

Malformed typed programs cannot smuggle an out-of-range enum index, an empty
enum, `2^64 - 1`, a signed Exp-Golomb enum, or an opcode/type mismatch into a
source read.

## Non-goals

This decision does not add enums to `se`, computed fields, lazy regions, or
compressed payloads. It does not add enum-member expressions, bit flags, enum
aliases at runtime, a general set constraint, or recovery after a fatal enum
membership failure. It does not by itself expand the remaining H.264 slice
header branches.
